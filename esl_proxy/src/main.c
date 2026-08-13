#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#if ORCHESTRATION_TIME
#include <time.h>
#endif

#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "early_dispatch.h"
#include "executor.h"
#include "lat_trace.h"
#include "log.h"
#include "manager.h"
#include "mem_pool.h"
#include "swimlane.h"

#ifndef ORCH_CASE
#define ORCH_CASE qwen3_dynamic_manual_scope.h
#endif

/*
 * ED_PAD_BYTES：代码布局对照实验专用，默认不定义、零影响。
 *
 * 生成一段体积精确为 N 字节、永不被调用的死代码（内容全是 nop）。必须放在
 * main.c 里：链接顺序把 main.o 排在最前，所以这里插多少字节，后面所有热函数
 * （executor_worker/cutter_worker/dispatch/...）的地址就整体后移多少。
 *
 * 用途是分离「ED 的运行时开销」和「ED 代码把热函数地址推移带来的布局效应」：
 * ED_ENABLE=1 时 main 因为多打印 ED 统计而变大约 896 字节，热函数因此整体
 * 后移 896 字节。用 ED_ENABLE=0 + ED_PAD_BYTES=896 可以复现同样的地址分布，
 * 而运行时一行 ED 代码都不执行。
 *
 * 用法：make ED_ENABLE=0 EXTRA_CFLAGS=-DED_PAD_BYTES=896
 * 验证生效：nm bin/esl_proxy | grep executor_worker，地址应比未填充版本大 896。
 */
#ifdef ED_PAD_BYTES
#define ED_PAD_STR2(x) #x
#define ED_PAD_STR(x) ED_PAD_STR2(x)
__asm__(".pushsection .text\n\t"
        ".balign 16\n\t"
        ".globl esl_layout_pad_never_called\n\t"
        ".type esl_layout_pad_never_called, @function\n"
        "esl_layout_pad_never_called:\n\t"
        ".space " ED_PAD_STR(ED_PAD_BYTES) ", 0x90\n\t"
        ".size esl_layout_pad_never_called, .-esl_layout_pad_never_called\n\t"
        ".popsection");
#endif

/* Macro to stringify the include directive properly */
#define INCLUDE(x) #x
#define INCLUDE_FILE(x) INCLUDE(x)
#include INCLUDE_FILE(ORCH_CASE)

/* Forward declaration for the orchestration entry point provided by the case */
void aicpu_orchestration_entry(const uint64_t orch_args);

#define MEM_POOL_BYTES (1024UL * 1024UL * 1024UL)
#define WHEN2FREE_CAP 4096

static uint8_t g_mem_pool_storage[MEM_POOL_BYTES];
static when2free_entry_t g_when2free_entries[WHEN2FREE_CAP];

extern atomic_bool g_orch_is_done;
extern uint32_t g_completed_task_cnt;

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t cutter_threads[CUTTER_THREAD_CNT];
    pthread_t executor_threads[EXECUTOR_THREAD_CNT];

#if ORCHESTRATION_TIME
    uint64_t total_start_ns = get_time_ns();
    (void)total_start_ns;
#endif

#if WORKER_LOG
    const char *log_env = getenv("WORKER_LOG");
    if (log_env != NULL && log_env[0] == '1') {
        g_worker_log = 1;
        log_init("pto.");
    }
#endif

    mem_pool_init(&g_mem_pool, g_mem_pool_storage, sizeof g_mem_pool_storage);
    mem_pool_init_fifo(&g_mem_pool, g_when2free_entries, WHEN2FREE_CAP);
    ring_buf_init();
    init_predecessors();
    init_ctrl_t();
    ed_init();
    lat_trace_init();
    /* 必须早于任何线程启动：t0 与缓冲都在这里就位 */
    swim_init();

    executor_init();

    // pthread_create(&manager_thread, NULL, manager_worker, &g_mem_pool);

    for (int i = 0; i < EXECUTOR_THREAD_CNT; i++) {
        pthread_create(&executor_threads[i], NULL, executor_worker, (void *)(intptr_t)i);
    }
    for (int i = 0; i < CUTTER_THREAD_CNT; i++) {
        pthread_create(&cutter_threads[i], NULL, cutter_worker,
                       (void *)(intptr_t)i);
    }

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_create(&dispatch_threads[i], NULL, dispatch_worker,
                       (void *)(intptr_t)i);
    }
#if ORCHESTRATION_TIME
    uint64_t swim_orch_start = swim_now();
    uint64_t start_ns = get_time_ns();
    aicpu_orchestration_entry(0);
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;
    swim_phase(SWIM_ROLE_ORCH, SWIM_PH_ORCH, 0, 0, g_task_id, swim_orch_start,
               swim_now());

    MAIN_LOGF("[orchestration] task_cnt = %u", g_task_id);
    MAIN_LOGF("[orchestration] subtask_cnt = %llu", (unsigned long long)g_subtask_cnt);
    MAIN_LOGF("[orchestration] elapsed_time = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[orchestration] task_tp = %f MTasks/s", (float)(g_task_id * 1000.0 / elapsed_ns));
    MAIN_LOGF("[orchestration] subtask_tp = %f MTasks/s", (float)(g_subtask_cnt * 1000.0 / elapsed_ns));
#else
    uint64_t swim_orch_start = swim_now();
    aicpu_orchestration_entry(0);
    swim_phase(SWIM_ROLE_ORCH, SWIM_PH_ORCH, 0, 0, g_task_id, swim_orch_start,
               swim_now());
#endif
    atomic_store(&g_orch_is_done, true);

    for (int i = 0; i < EXECUTOR_THREAD_CNT; i++) {
        pthread_join(executor_threads[i], NULL);
    }
    for (int i = 0; i < CUTTER_THREAD_CNT; i++) {
        pthread_join(cutter_threads[i], NULL);
    }
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_join(dispatch_threads[i], NULL);
    }
    // pthread_join(manager_thread, NULL);

    MAIN_LOGF("[summary] completed_task_cnt = %u", (unsigned)g_completed_task_cnt);

#if ED_ENABLE
    uint64_t stage_cnt = atomic_load_explicit(&g_ed_stage_cnt, memory_order_relaxed);
    uint64_t stage_spmd_cnt = atomic_load_explicit(&g_ed_stage_spmd_cnt, memory_order_relaxed);
    uint64_t hit_cnt = atomic_load_explicit(&g_ed_hit_cnt, memory_order_relaxed);
    uint64_t self_notify_cnt = atomic_load_explicit(&g_ed_self_notify_cnt, memory_order_relaxed);
    uint64_t slot_retry_cnt = atomic_load_explicit(&g_ed_slot_retry_cnt, memory_order_relaxed);
    uint64_t block_cas_fail_cnt = atomic_load_explicit(&g_ed_block_cas_fail_cnt, memory_order_relaxed);
    uint64_t send_skip_cnt = atomic_load_explicit(&g_ed_send_skip_cnt, memory_order_relaxed);
    uint64_t late_arrival_cnt = atomic_load_explicit(&g_ed_late_arrival_cnt, memory_order_relaxed);

    MAIN_LOGF("[ed] stage_cnt = %llu", (unsigned long long)stage_cnt);
    /* stage_cnt 的 SPMD 分档；stage_spmd_cnt/stage_cnt 即本次覆盖率提升的来源占比 */
    MAIN_LOGF("[ed] stage_spmd_cnt = %llu", (unsigned long long)stage_spmd_cnt);
    MAIN_LOGF("[ed] hit_cnt = %llu", (unsigned long long)hit_cnt);
    MAIN_LOGF("[ed] self_notify_cnt = %llu", (unsigned long long)self_notify_cnt);
    MAIN_LOGF("[ed] slot_retry_cnt = %llu", (unsigned long long)slot_retry_cnt);
    MAIN_LOGF("[ed] block_cas_fail_cnt = %llu", (unsigned long long)block_cas_fail_cnt);
    MAIN_LOGF("[ed] send_skip_cnt = %llu", (unsigned long long)send_skip_cnt);
    MAIN_LOGF("[ed] late_arrival_cnt = %llu", (unsigned long long)late_arrival_cnt);
    /* 对账：notify 只敲门铃，实际开闸由 executor 完成，两者应一一对应 */
    MAIN_LOGF("[ed] gate_open_cnt = %llu",
              (unsigned long long)atomic_load_explicit(&g_ed_gate_open_cnt,
                                                       memory_order_relaxed));

    if (stage_cnt > 0) {
        double doorbell_ratio = (double)(hit_cnt + self_notify_cnt) / (double)stage_cnt;
        double self_notify_ratio = (hit_cnt + self_notify_cnt) > 0
                                       ? (double)self_notify_cnt / (double)(hit_cnt + self_notify_cnt)
                                       : 0.0;
        MAIN_LOGF("[ed] doorbell_ratio = %.6f", doorbell_ratio);
        MAIN_LOGF("[ed] self_notify_ratio = %.6f", self_notify_ratio);
    }

    uint32_t leaked_staging = 0;
    uint32_t block_leaked = 0;
    uint32_t slot_leaked = 0;
    uint16_t last = (uint16_t)((atomic_load_explicit(&g_task_id, memory_order_acquire) >= RING_SIZE)
                                    ? RING_SIZE
                                    : atomic_load_explicit(&g_task_id, memory_order_relaxed));
    for (uint16_t i = 0; i < last; i++) {
        if (g_basic_buf[i].count == 0) {
            continue;
        }
        uint8_t st = atomic_load_explicit(&g_spec_state[i], memory_order_relaxed);
        if (st == ED_SPEC_STAGING) {
            leaked_staging++;
        }
        uint16_t nbi = atomic_load_explicit(&g_next_block_idx[i], memory_order_relaxed);
        if (nbi != g_basic_buf[i].count) {
            block_leaked++;
        }
    }
    for (int t = 0; t < EXE_TYPE_CNT; t++) {
        for (int c = 0; c < AIC_CNT; c++) {
            for (int s = 0; s < AIC_OSTD; s++) {
                if (atomic_load_explicit(&g_executors[t][c].slot_state[s], memory_order_relaxed) !=
                    EXE_SLOT_EMPTY) {
                    slot_leaked++;
                }
            }
        }
    }

    MAIN_LOGF("[ed] leaked_staging = %u", leaked_staging);
    MAIN_LOGF("[ed] block_leaked = %u", block_leaked);
    MAIN_LOGF("[ed] slot_leaked = %u", slot_leaked);

    uint64_t sum_target = 0;
    for (uint16_t i = 0; i < last; i++) {
        if (g_basic_buf[i].count == 0) {
            continue;
        }
        /*
         * 这里曾经跳过 count != 1。fanin 记账现在对所有 count 无条件累加
         * （propagate_dispatch_fanin 与 cutter 两个生产者同口径），所以自检也必须
         * 覆盖 count>1——否则最需要盯的新增人群恰好处于盲区。
         */
        uint16_t cur = atomic_load_explicit(&g_dispatch_fanin[i], memory_order_relaxed);
        uint16_t tgt = g_dispatch_fanin_target[i];
        sum_target += tgt;
        if (cur != tgt) {
            MAIN_LOGF("[ed] fanin_check, s=%u, cur=%u, tgt=%u MISMATCH", i, cur, tgt);
        }
    }
    MAIN_LOGF("[ed] sum_fanin_target = %llu", (unsigned long long)sum_target);
#if ED_HOOK0_CONTRIB_STATS
    MAIN_LOGF("[ed] hook0_contrib_cnt = %llu",
              (unsigned long long)atomic_load_explicit(&g_ed_hook0_contrib_cnt, memory_order_relaxed));
#endif
#endif

    /*
     * ready->runnable 延迟：ED 的直接 KPI，ED_ENABLE=0/1 都输出。
     * 判据是 ed 路径的 mean/p50 应显著低于 normal 路径；若两者接近，
     * 说明 ED 的槽位预占没有换来可执行时机的提前。
     */
    static const char *lat_path_name[ED_LAT_PATH_CNT] = {"normal", "ed"};
    for (int k = 0; k < ED_LAT_PATH_CNT; k++) {
        uint64_t n = atomic_load_explicit(&g_ed_lat_cnt[k], memory_order_relaxed);
        if (n == 0) {
            MAIN_LOGF("[lat] %s: samples = 0", lat_path_name[k]);
            continue;
        }
        uint64_t sum_ns = atomic_load_explicit(&g_ed_lat_sum_ns[k], memory_order_relaxed);
        uint64_t max_ns = atomic_load_explicit(&g_ed_lat_max_ns[k], memory_order_relaxed);

        /* 按桶累计推分位数；桶 b 的上界是 2^b ns，故为"不超过"口径 */
        uint64_t acc = 0;
        uint64_t p50 = 0;
        uint64_t p99 = 0;
        bool p50_done = false;
        bool p99_done = false;
        for (int b = 0; b < ED_LAT_BUCKET_CNT; b++) {
            acc += atomic_load_explicit(&g_ed_lat_hist[k][b], memory_order_relaxed);
            if (!p50_done && acc * 100 >= n * 50) {
                p50 = (b == 0) ? 0 : ((uint64_t)1 << b);
                p50_done = true;
            }
            if (!p99_done && acc * 100 >= n * 99) {
                p99 = (b == 0) ? 0 : ((uint64_t)1 << b);
                p99_done = true;
            }
        }
        /* 桶上界可能超过真实最大值，收敛到 max 便于阅读 */
        if (p50 > max_ns) {
            p50 = max_ns;
        }
        if (p99 > max_ns) {
            p99 = max_ns;
        }

        /* MAIN_LOGF 单次最多 5 个变参，故分两行输出 */
        MAIN_LOGF("[lat] %s: samples = %llu, mean = %.1f ns",
                  lat_path_name[k], (unsigned long long)n,
                  (double)sum_ns / (double)n);
        MAIN_LOGF("[lat] %s: p50 <= %llu ns, p99 <= %llu ns, max = %llu ns",
                  lat_path_name[k], (unsigned long long)p50,
                  (unsigned long long)p99, (unsigned long long)max_ns);
    }

    lat_trace_dump();
    /* 线程都已 join，落盘无需同步 */
    swim_dump();

#if WORKER_LOG
    log_close();
#endif

    return 0;
}