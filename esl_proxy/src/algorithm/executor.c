/*
 * executor.c - Executor Implementation
 *
 * Provides task execution utilities including delay functionality.
 *
 * C11 standard with _Atomic for lock-free concurrency.
 */

#define _POSIX_C_SOURCE 200809L

#include "conf.h"
#include <time.h>
#include "log.h"
#include "executor.h"
#include "dispatch.h"
#include "early_dispatch.h"
#include "ed_gate.h"
#include "ring_buf.h"
#include "swimlane.h"

#ifndef ED_A11_PROBE
#define ED_A11_PROBE 0
#endif

#ifndef ED_A11_DUMP_PERIOD
#define ED_A11_DUMP_PERIOD 500
#endif

extern _Atomic bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

void executor_init(void)
{
    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int core = 0; core < AIC_CNT; core++) {
            g_executors[exe_type][core].idx = AIC_OSTD;
            for (int i = 0; i < AIC_OSTD; i++) {
                g_executors[exe_type][core].tasks[i] = 0;
                g_executors[exe_type][core].block_idx[i] = 0;
                g_executors[exe_type][core].duration[i] = 0;
                g_executors[exe_type][core].base[i] = 0;
                atomic_init(&g_executors[exe_type][core].slot_state[i],
                            EXE_SLOT_EMPTY);
#if ED_ENABLE
                atomic_init(&g_executors[exe_type][core].doorbell[i], 0);
#endif
            }
        }
    }
}

/*
 * slot 完成的唯一收口：
 * 1) 先 release 置 EMPTY，让 dispatcher 可安全回收 free bit。
 * 2) 再 release 置 msg_bitmap done bit，触发 dispatcher drain。
 */
static inline void complete_slot(int type, int core, int slot, uint16_t task_id_done)
{
    executor_t *e = &g_executors[type][core];
    /*
     * 必须早于下面发布 EMPTY / done bit：泳道要求 end 严格早于 dispatcher 回填的
     * finish，一旦发布出去 dispatcher 下一拍就可能 drain 到它。
     * 用 e->tasks[slot] 而不是 task_id_done——后者被截成 16 位，任务数过 65535 会回绕。
     */
    swim_task_end(e->tasks[slot]);
    e->block_idx[slot] = 0;
    e->idx = AIC_OSTD;
#if ED_ENABLE && !ED_ABLATE_COMPLETE
    /* Step 2：仅做 tag 校验后清记录，不接 Hook。 */
    ed_task_dispatch_record_clear(task_id_done);
    uint32_t live_tag = atomic_load_explicit(
        &g_ring_task_tag[task_id_done & RING_MASK], memory_order_acquire);
    WORKER_LOGF("slot_free, task=%u, tag=%u", task_id_done, live_tag);
#else
    (void)task_id_done;
#endif
    atomic_store_explicit(&e->slot_state[slot], EXE_SLOT_EMPTY,
                          memory_order_release);
    atomic_fetch_or_explicit(&g_ctrl_t[core % DISPATCH_THREAD_CNT].msg_bitmap[type][slot],
                             ((uint64_t)0x1 << core), memory_order_release);
}

void* executor_worker(void *arg)
{
    (void)arg;
    int total_write_cnt = 0;
#if ED_ENABLE && !ED_ABLATE_GATE
    /* executor 私有的「待开闸核」集合，跨轮保留，详见 ed_drain_gated_cores */
    uint64_t gated_pending[EXE_TYPE_CNT] = {0};
#endif
#if ED_ENABLE && ED_A11_PROBE
    uint64_t iterations = 0;
#endif
    while (!atomic_load(&g_is_done))
    {
#if ED_ENABLE && !ED_ABLATE_GATE
        /* 必须早于主扫描：本轮开闸的槽位应当本轮就能被认领起跑 */
        ed_drain_gated_cores(gated_pending);
#endif
#if ED_ENABLE && ED_A11_PROBE
        iterations++;
        if ((iterations % ED_A11_DUMP_PERIOD) == 0) {
            for (int t = 0; t < EXE_TYPE_CNT; t++) {
                for (int c = 0; c < AIC_CNT; c++) {
                    for (int sl = 0; sl < AIC_OSTD; sl++) {
                        uint32_t task = g_executors[t][c].tasks[sl];
                        uint16_t s_idx = (uint16_t)(task & RING_MASK);
                        WORKER_LOGF("slot_state_dump, s=%u, unfin=%u, spec=%u, state=%u, doorbell=%u",
                                    task,
                                    (unsigned)atomic_load_explicit(&g_unfin_pred_cnt[s_idx], memory_order_relaxed),
                                    (unsigned)atomic_load_explicit(&g_spec_state[s_idx], memory_order_relaxed),
                                    (unsigned)atomic_load_explicit(&g_executors[t][c].slot_state[sl], memory_order_relaxed),
                                    (unsigned)atomic_load_explicit(&g_executors[t][c].doorbell[sl], memory_order_relaxed));
                    }
                }
            }
        }
#endif
        for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
            for (int core = 0; core < AIC_CNT; core++) {
                executor_t *e = &g_executors[exe_type][core];
                uint8_t active = e->idx;
                if (active >= AIC_OSTD) {
                    /*
                     * core 空闲时才挑选可运行 slot；同一时刻只允许一个 slot 执行。
                     */
                    for (int slot = 0; slot < AIC_OSTD; slot++) {
                        uint8_t state = atomic_load_explicit(&e->slot_state[slot],
                                                             memory_order_acquire);
                        /*
                         * 这条循环每轮扫 EXE_TYPE_CNT*AIC_CNT*AIC_OSTD 个槽位，
                         * 整段运行是百万次量级，因此不允许出现任何 ED 相关判断——
                         * GATED 槽位统一由循环外的 ed_drain_gated_cores 处理，
                         * 开闸后 slot_state 已是 RUNNABLE，这里照常认领。
                         */
                        if (state != EXE_SLOT_RUNNABLE) {
                            continue;
                        }
                        e->idx = (uint8_t)slot;
                        active = (uint8_t)slot;
                        /*
                         * 泳道的 run 起点。放在认领处而不是槽位变 RUNNABLE 处：
                         * 该核此刻可能正忙着别的 slot，两者之间那段等待就是
                         * 预装载位上的排队，正是要看的东西。
                         */
                        swim_task_run(e->tasks[slot]);
                        break;
                    }
                    if (active >= AIC_OSTD) {
                        continue;
                    }
                }

                int slot = (int)active;
                uint8_t active_state = atomic_load_explicit(&e->slot_state[slot],
                                                            memory_order_acquire);
                if (active_state != EXE_SLOT_RUNNABLE) {
                    e->idx = AIC_OSTD;
                    continue;
                }

                uint16_t task_id = e->tasks[slot];
                uint32_t block_count = g_basic_buf[task_id & RING_MASK].count;

                if (block_count > 1) {
                    /*
                     * 保留 SPMD 语义：单次 dispatch 的条目要执行完所有 block，
                     * 不能在首块结束就直接发布 completed。
                     */
                    if (e->duration[slot] > 0) {
                        e->duration[slot]--;
                        continue;
                    }
                    uint16_t next_block = ++e->block_idx[slot];
                    if (next_block < block_count) {
                        uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
                        e->duration[slot] = SCALE_EXEC_DURATION(raw_duration);
                        continue;
                    }
                    total_write_cnt++;
                    WORKER_LOGF("total,%d,core,%d,type,%d,blocks,%u",
                                total_write_cnt, core, exe_type, block_count);
                    complete_slot(exe_type, core, slot, task_id);
                } else {
                    if (e->duration[slot] > 0) {
                        e->duration[slot]--;
                    }
                    if (e->duration[slot] == 0) {
                        total_write_cnt++;
                        WORKER_LOGF("total,%d,core,%d,type,%d", total_write_cnt, core, exe_type);
                        complete_slot(exe_type, core, slot, task_id);
                    }
                }
            }
        }
    }
    WORKER_LOGF("finished, total_write_cnt=%d", total_write_cnt);
    return NULL;
}
