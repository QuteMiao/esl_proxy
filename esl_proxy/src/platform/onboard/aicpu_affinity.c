/* aicpu_affinity.c — onboard AICPU CPU 亲和性门控(对齐 simpler 的 filter gate)。
 *
 * host 侧探测 OCCUPY 掩码并算出允许的控制 CPU 列表,经 EslRuntime 传入设备;
 * CANN 把 launch_count 个线程拉起后,本门控让每个线程上报 sched_getcpu(),屏障同步
 * 后由一个 leader 线程把各线程映射到 allowed_cpus 的槽位,匹配上的存活并拿到 exec_idx
 * (角色 id),其余线程退出。这样控制线程被钉在 host 选好的核上,避免漂移/争用。
 */
#define _GNU_SOURCE

#include "aicpu_affinity.h"
#include "onboard_log.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* 16 = simpler 的 launch 预算上限(a5 14 个逻辑用户核 + 余量);a2a3 只拉 3~6 个,
 * 永远碰不到这个上界。所有静态槽数组都按此上界开。 */
#define ESL_GATE_MAX_THREADS 16

/* 每线程的 exec/角色 id,由门控写入:-1=被淘汰,否则为 allowed_cpus[] 中的下标。 */
static _Thread_local int32_t tl_exec_idx = -1;

/* 每轮 launch 的屏障 + 分类状态。
 * s_filter_claim 用 fetch_add 给每个线程发一个唯一槽位 idx,使其写入互不重叠的
 * s_filter_thread_cpu[idx]。s_filter_published 在 cpu 写入之后(release)自增 ——
 * 分类屏障等待 published(acquire),所以当它等于 total_launched 时每个线程的 cpu
 * 写入都已可见。单计数器做不到:若屏障等待的就是 fetch_add 已经推进的那个计数器,
 * 介于 fetch_add 与屏障检查之间的 cpu 存储是无序的。 */
static atomic_int s_filter_claim;
static atomic_int s_filter_published;
static atomic_int s_filter_classify_init;
static atomic_int s_filter_classify_ready;
static atomic_int s_filter_cleanup;
static int32_t s_filter_thread_cpu[ESL_GATE_MAX_THREADS];
static int32_t s_filter_thread_exec_idx[ESL_GATE_MAX_THREADS];

int esl_aicpu_affinity_gate(const int32_t *allowed_cpus, int32_t allowed_count,
                            int32_t total_launched, int *out_exec_idx)
{
    int32_t idx;
    int32_t cpu;
    bool survive;

    tl_exec_idx = -1;
    if (out_exec_idx != NULL) {
        *out_exec_idx = -1;
    }

    /* 在任何下标访问前对两个入参做边界检查,防止越界读 allowed_cpus[]/静态槽。 */
    if (allowed_cpus == NULL || allowed_count <= 0 || allowed_count > ESL_GATE_MAX_THREADS ||
        total_launched <= 0 || total_launched > ESL_GATE_MAX_THREADS) {
        LOG_ERROR("AICPU filter gate: invalid config allowed_count=%d total_launched=%d (max=%d)",
                  allowed_count, total_launched, ESL_GATE_MAX_THREADS);
        return 0;
    }

    idx = atomic_fetch_add_explicit(&s_filter_claim, 1, memory_order_acq_rel);
#if defined(__aarch64__) || defined(__x86_64__)
    cpu = (int32_t)sched_getcpu();
#else
    cpu = -1;
#endif

    if (idx >= 0 && idx < ESL_GATE_MAX_THREADS) {
        s_filter_thread_cpu[idx] = cpu;
    }

    /* 发布:release 自增确保上面对 s_filter_thread_cpu[idx] 的写入对任何通过
     * acquire 读到新 published 值的线程可见。 */
    atomic_fetch_add_explicit(&s_filter_published, 1, memory_order_release);
    /* 屏障:等待所有被拉起的线程都已发布自己的 cpu。 */
    while (atomic_load_explicit(&s_filter_published, memory_order_acquire) < total_launched) {
    }

    /* 由一个线程为所有线程做分类。 */
    {
        int32_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&s_filter_classify_init, &expected, 1,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            bool slot_filled[ESL_GATE_MAX_THREADS] = {false};
            int32_t filled_count = 0;
            int32_t i;
            int32_t tid;
            int32_t a;

            for (i = 0; i < total_launched && i < ESL_GATE_MAX_THREADS; ++i) {
                s_filter_thread_exec_idx[i] = -1;
            }

            /* 对每个上报线程,看其 cpu 是否在 allowed_cpus 中;命中则该线程存活并取得
             * exec_idx = 在 allowed_cpus[] 中的下标。允许多个线程落到同一 cpu(CANN 在
             * launch_count >= popcount(OCCUPY) 时会过订阅);首个落到某允许核的线程胜出,
             * 后续重复者被淘汰。O(total_launched * allowed_count),两者都 <= 16。 */
            for (tid = 0; tid < total_launched && tid < ESL_GATE_MAX_THREADS; ++tid) {
                int32_t my_cpu = s_filter_thread_cpu[tid];
                if (my_cpu < 0) {
                    continue;
                }
                for (a = 0; a < allowed_count && a < ESL_GATE_MAX_THREADS; ++a) {
                    if (allowed_cpus[a] == my_cpu && !slot_filled[a]) {
                        s_filter_thread_exec_idx[tid] = a;
                        slot_filled[a] = true;
                        ++filled_count;
                        break;
                    }
                }
            }

            /* 前进保证:launch_count=popcount(OCCUPY) 时期望每个 host 选中的 cpu 都有一个
             * 代表,但某些运行时分发模式可能重复某个 cpu 而漏掉另一个。若发生,保留精确匹配
             * 的结果,并按上报顺序补齐缺失的 exec 槽,使 sched/orch 角色都齐全,而不是让
             * AICore 侧握手超时。 */
            if (filled_count < allowed_count) {
                int32_t next_slot = 0;
                LOG_ERROR("AICPU filter gate: only matched %d/%d allowed cpus; filling by report order",
                          filled_count, allowed_count);
                for (tid = 0; tid < total_launched && tid < ESL_GATE_MAX_THREADS &&
                              filled_count < allowed_count; ++tid) {
                    if (s_filter_thread_exec_idx[tid] >= 0) {
                        continue;
                    }
                    while (next_slot < allowed_count && slot_filled[next_slot]) {
                        ++next_slot;
                    }
                    if (next_slot >= allowed_count) {
                        break;
                    }
                    s_filter_thread_exec_idx[tid] = next_slot;
                    slot_filled[next_slot] = true;
                    ++filled_count;
                }
            }

            atomic_store_explicit(&s_filter_classify_ready, 1, memory_order_release);
        }
    }

    while (atomic_load_explicit(&s_filter_classify_ready, memory_order_acquire) == 0) {
    }

    if (idx >= 0 && idx < total_launched && idx < ESL_GATE_MAX_THREADS) {
        tl_exec_idx = s_filter_thread_exec_idx[idx];
        survive = (tl_exec_idx >= 0);
    } else {
        tl_exec_idx = -1;
        survive = false;
    }

    /* 在最后一个线程读完结果后复位门控状态,便于下一轮 exec 复用。 */
    if (atomic_fetch_add_explicit(&s_filter_cleanup, 1, memory_order_acq_rel) + 1 == total_launched) {
        atomic_store_explicit(&s_filter_claim, 0, memory_order_release);
        atomic_store_explicit(&s_filter_published, 0, memory_order_release);
        atomic_store_explicit(&s_filter_classify_init, 0, memory_order_release);
        atomic_store_explicit(&s_filter_classify_ready, 0, memory_order_release);
        atomic_store_explicit(&s_filter_cleanup, 0, memory_order_release);
    }

    if (out_exec_idx != NULL) {
        *out_exec_idx = tl_exec_idx;
    }
    if (survive) {
        LOG_ERROR("AICPU filter gate: thread idx=%d cpu=%d exec_idx=%d ACTIVE", idx, cpu, tl_exec_idx);
    } else {
        LOG_ERROR("AICPU filter gate: thread idx=%d cpu=%d DROPPED", idx, cpu);
    }
    return survive ? 1 : 0;
}
