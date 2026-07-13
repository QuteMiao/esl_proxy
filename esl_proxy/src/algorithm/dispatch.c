/*
 * dispatch.c - Dispatch Worker Thread Implementation (MIX dispatch, SPMD range-claim)
 *
 * Worker thread entry point for Dispatch.
 *
 * Layout (section order 0-1-2-5-6-4-3):
 *   0 includes/externs  1 static state  2 basic infra
 *   5 schedule path     6 worker
 *   4 MIX               3 SPMD
 *
 * Double-buffer mode: -DESL_DISPATCH_DOUBLE_BUFFER=1; differs only inside
 * send_task / dispatch_prefetch / dispatch (local #if).
 *
 * SPMD block cursor (g_next_block) is non-atomic: batch_dequeue's queue lock
 * serializes pops. g_finished_blocks stays _Atomic for concurrent completions.
 */
#define _GNU_SOURCE

#include "dispatch.h"
#include "handshake.h"
#include "runtime.h"
#include "cutter.h"
#include "executor.h"
#include "log.h"
#include "memory_barrier.h"
#include "queue.h"
#include "ring_buf.h"
#include "spin.h"
#include "swimlane_aicpu.h"
#include "worker_map.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_regs.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

extern struct task_desc g_basic_buf[RING_SIZE];
extern atomic_int g_task_id;
extern atomic_bool g_orch_is_done;
extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
extern _Atomic uint16_t g_predecessor_cnt[RING_SIZE];
extern int g_subtask_cnt;

EslRuntime *g_runtime;

/* ===== 1. Static state ===== */

/* Per-task SPMD block cursor. Non-atomic: pop-serialized. */
static uint16_t g_next_block[RING_SIZE];

/* Per-task finished-block counter. STAYS _Atomic. */
static _Atomic uint16_t g_finished_blocks[RING_SIZE];

static uint32_t g_slot_block_idx[EXE_TYPE_CNT][AIC_CNT][AIC_OSTD];
static uint8_t g_mix_active[AIC_CNT][AIC_OSTD];
static uint16_t g_mix_task[AIC_CNT][AIC_OSTD];

/* Forward decls for MIX / SPMD used by section 2 and 5 (defs in 4 / 3). */
static int dispatch_mix_aic_phys(int core);
static int dispatch_mix_aiv0_phys(int core);
static int dispatch_mix_aiv1_phys(int core);
static int dispatch_mix_cluster_idle(ctrl_t *ctrl, int core, int *out_slot);
static int dispatch_mix_cluster_all_done(ctrl_t *ctrl, int core, int slot);
static int dispatch_mix_defer_slot_clear(int exe_type, int core, int slot);
static int dispatch_mix_partial_pending(ctrl_t *ctrl, int exe_type, int core, int slot);
static void dispatch_mix_occupy_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx);
static void dispatch_mix_release_cluster(ctrl_t *ctrl, int core, int slot);
static int dispatch_mix_prepare_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx,
                                        EslPublishHandle pubs[], int phys_arr[], int *np);
static void dispatch_mix_flush(EslPublishHandle pubs[], const int phys_arr[], int np);
static int dispatch_mix_harvest_completed(ctrl_t *ctrl, uint16_t done_tasks[], int max_out);
#if !ESL_DISPATCH_DOUBLE_BUFFER
static int dispatch_mix_core_busy(int core);
static int dispatch_mix_cluster_pending(ctrl_t *ctrl, int core, int *out_busy_slot, int *out_free_slot);
static int dispatch_mix_cluster_subtasks_acked(int core, int slot);
static int dispatch_mix_publish_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx);
static int dispatch_mix_prefetch(ctrl_t *ctrl);
#endif
static void dispatch_merge_msg_to_free(ctrl_t *ctrl);
static int dispatch_push_completed_slots(ctrl_t *ctrl, uint16_t out_tasks[], int max_out);

static int dispatch_spmd_claim_range(uint16_t task_id, int avail, uint32_t *start_block);
static int dispatch_spmd_claim_block(uint16_t task_id, uint32_t *block_idx);
static int dispatch_spmd_rewind(uint16_t task_id, uint32_t claimed_end, uint32_t next_block);
static int dispatch_spmd_has_remaining(uint16_t task_id);
static int dispatch_spmd_note_block_done(uint16_t task_id);

/* ===== 2. Basic infrastructure ===== */

static uint64_t dispatch_core_reg_addr(int worker_id)
{
    uint64_t reg_addr = esl_handshake_reg_addr(worker_id);

    if (reg_addr != 0) {
        return reg_addr;
    }
    const uint64_t table = get_platform_regs();
    int hal_idx;

    if (table == 0) {
        return 0;
    }
    hal_idx = esl_worker_to_hal_reg_index(worker_id);
    if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
        return 0;
    }
    return ((uint64_t *)table)[hal_idx];
}

/* Cached COND-register pointer for a physical worker. */
static volatile uint32_t *dispatch_core_cond_ptr(int worker_id)
{
    volatile uint32_t *cond_ptr = esl_handshake_cond_ptr(worker_id);

    if (cond_ptr != NULL) {
        return cond_ptr;
    }
    const uint64_t table = get_platform_regs();
    int hal_idx;

    if (table == 0) {
        return NULL;
    }
    hal_idx = esl_worker_to_hal_reg_index(worker_id);
    if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
        return NULL;
    }
    const uint64_t reg_addr = ((uint64_t *)table)[hal_idx];

    if (reg_addr == 0) {
        return NULL;
    }
    return platform_reg_cond_ptr(reg_addr);
}

static void dispatch_mark_slot_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                        uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (!platform_reg_task_finished(reg_addr, reg_task)) {
        return;
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] |= mask;
    if (!dispatch_mix_defer_slot_clear(exe_type, core, slot)) {
        g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
        g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    }
}

/* Complete a already-decided slot without re-checking FIN (poll inference path). */
static void dispatch_force_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                    uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] & mask) {
        return;
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] |= mask;
    if (!dispatch_mix_defer_slot_clear(exe_type, core, slot)) {
        g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
        g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    }
}

/* Pull AICore FIN events into msg_bitmap. */
void dispatch_poll(int tid)
{
    if (g_runtime == NULL) {
        return;
    }
    const int n_workers = g_runtime->worker_count;
    const int n_cores = AIC_CNT;

    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int core = 0; core < n_cores; core++) {
            if (CORE_LANE(core) != tid) {
                continue;
            }
            const uint64_t mask = (uint64_t)1 << core;
            /* 收集本 (exe_type,core) 仍在飞、未完成、phys 合法的 slot。 */
            int bs[AIC_OSTD];
            int nb = 0;

            for (int slot = 0; slot < AIC_OSTD; slot++) {
                if (g_executors[exe_type][core].tasks[slot] == EXEC_SLOT_EMPTY) {
                    continue;
                }
                if (g_ctrl_t[tid].msg_bitmap[exe_type][slot] & mask) {
                    continue;
                }
                const int phys = (int)g_executors[exe_type][core].block_idx[slot];
                if (phys < 0 || phys >= n_workers) {
                    continue;
                }
                if ((uint32_t)g_executors[exe_type][core].base[slot] == 0U) {
                    continue;
                }
                bs[nb++] = slot;
            }
            if (nb == 0) {
                continue;
            }
            const int phys0 = (int)g_executors[exe_type][core].block_idx[bs[0]];
            if (nb == 2 && (int)g_executors[exe_type][core].block_idx[bs[1]] == phys0) {
                /* 同一物理核 2 个在飞:COND 单寄存器只留最新值。仿照 simpler:读一次 COND,
                 * 按 seq 定 running(旧)/pending(新),用"看到 pending 事件即推断 running 完成"
                 * (串行执行)化解覆盖丢失 —— 见 simpler decide_slot_transition。
                 *
                 * 双 slot 完成推断 vs 双缓冲(prefetch):
                 *  - 双 slot 完成推断是基础设施(正确性要求):同一物理核 2 个在飞时,单 COND
                 *    寄存器只留最新值,先发任务的 FIN 会被后发任务覆盖丢失;不推断则先发任务
                 *    永远无法完成(挂死)。无论是否启用双缓冲都必须处理。
                 *  - 双缓冲(dispatch_prefetch)是性能优化:主动利用第二 slot 提前下发下一
                 *    任务,提高并行度。
                 *  - 两者关系:双缓冲创建 2 在飞场景,完成推断处理该场景的完成事件。
                 *  - 对应 simpler 的 decide_slot_transition(scheduler_completion.cpp:46-61),
                 *    由 zhusy54 在 PR #477 (2026-04-12) 引入。 */
                const int s0 = bs[0];
                const int s1 = bs[1];
                const uint32_t b0 = (uint32_t)g_executors[exe_type][core].base[s0];
                const uint32_t b1 = (uint32_t)g_executors[exe_type][core].base[s1];
                int run_slot;
                int pend_slot;
                const uint32_t fwd01 = (b1 - b0) & (uint32_t)TASK_ID_MASK;

                if (fwd01 != 0U && fwd01 < (1U << 30)) {
                    run_slot = s0; /* b1 在 b0 之后 → s1 为 pending */
                    pend_slot = s1;
                } else {
                    run_slot = s1;
                    pend_slot = s0;
                }
                const uint32_t run_reg = (uint32_t)g_executors[exe_type][core].base[run_slot];
                const uint32_t pend_reg = (uint32_t)g_executors[exe_type][core].base[pend_slot];
                volatile uint32_t *cond_ptr = dispatch_core_cond_ptr(phys0);

                if (cond_ptr == NULL) {
                    continue;
                }
                const uint32_t cond = *cond_ptr;
                OUT_OF_ORDER_LOAD_BARRIER();
                const int id = EXTRACT_TASK_ID((uint64_t)cond);
                const int st = EXTRACT_TASK_STATE((uint64_t)cond);

                if (id == (int)pend_reg) {
                    /* 看到 pending 的 ACK/FIN ⇒ running 必已完成(串行)。仅在此刻(确有完成)
                     * 才解析 reg_addr 供 ack 使用。 */
                    const uint64_t reg_addr = dispatch_core_reg_addr(phys0);

                    dispatch_force_complete(exe_type, core, run_slot, reg_addr, run_reg);
                    if (st == TASK_FIN_STATE) {
                        dispatch_force_complete(exe_type, core, pend_slot, reg_addr, pend_reg);
                    }
                } else if (id == (int)run_reg && st == TASK_FIN_STATE) {
                    /* running 已 FIN 但 pending 仍在飞:瞬态,等 pending 事件统一收割。 */
                    (void)0;
                }
                /* 其余(running ACK / 无匹配):跳过,下轮再看。 */
            } else {
                /* 不同物理核(VECTOR 轮替 lane)或单个在飞:各 slot 独立判 FIN。 */
                for (int k = 0; k < nb; k++) {
                    const int slot = bs[k];
                    const uint32_t reg_task = (uint32_t)g_executors[exe_type][core].base[slot];
                    const int phys = (int)g_executors[exe_type][core].block_idx[slot];
                    volatile uint32_t *cond_ptr = dispatch_core_cond_ptr(phys);

                    if (cond_ptr == NULL) {
                        continue;
                    }
                    const uint32_t cond = *cond_ptr;
                    OUT_OF_ORDER_LOAD_BARRIER();
                    if (EXTRACT_TASK_STATE((uint64_t)cond) == TASK_FIN_STATE &&
                        EXTRACT_TASK_ID((uint64_t)cond) == (int)reg_task) {
                        /* 单次读已确认 FIN → force_complete(不再二次读 COND)。 */
                        const uint64_t reg_addr = dispatch_core_reg_addr(phys);

                        dispatch_force_complete(exe_type, core, slot, reg_addr, reg_task);
                    }
                }
            }
        }
    }
}

static int dispatch_publish_block(ctrl_t *ctrl, int exe_type, int queue_type, uint16_t task_id,
                                  uint32_t block_idx, int core, int slot)
{
    const uint64_t mask = (uint64_t)1 << core;
    int rc = 0;

    if (g_runtime != NULL && (int)g_executors[exe_type][core].block_idx[slot] >= g_runtime->worker_count) {
        g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] |= mask;
        g_executors[exe_type][core].base[slot] = 0;
        return 0;
    }
    const int phys = (int)g_executors[exe_type][core].block_idx[slot];
    const uint64_t reg_addr = dispatch_core_reg_addr(phys);

    if (reg_addr == 0) {
        rc = -1;
    } else {
        EslPublishHandle pub = esl_prepare_subtask_to_core(g_runtime, phys, task_id, block_idx);

        if (pub.reg_task_id == 0U) {
            rc = -1;
        } else {
            pub.reg_addr = reg_addr;
            g_executors[exe_type][core].base[slot] = pub.reg_task_id;
            ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
            wmb();
            esl_publish_subtask_to_core(pub);
            dispatch_mark_slot_complete(exe_type, core, slot, reg_addr, pub.reg_task_id);
        }
    }
    if (rc != 0) {
        ctrl->free_bitmap[queue_type][slot] |= mask;
        g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    }
    return rc;
}

/* Batched range-publish: prepare only; caller does one wmb() then publish+mark. */
static int dispatch_publish_block_prepare(ctrl_t *ctrl, int exe_type, int queue_type, uint16_t task_id,
                                          uint32_t block_idx, int core, int slot,
                                          EslPublishHandle *out, int *fast)
{
    const uint64_t mask = (uint64_t)1 << core;

    *fast = 0;
    if (g_runtime != NULL && (int)g_executors[exe_type][core].block_idx[slot] >= g_runtime->worker_count) {
        g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] |= mask;
        g_executors[exe_type][core].base[slot] = 0;
        *fast = 1;
        return 0;
    }
    const int phys = (int)g_executors[exe_type][core].block_idx[slot];
    const uint64_t reg_addr = dispatch_core_reg_addr(phys);
    EslPublishHandle pub;

    if (reg_addr == 0) {
        goto fail;
    }
    pub = esl_prepare_subtask_to_core(g_runtime, phys, task_id, block_idx);
    if (pub.reg_task_id == 0U) {
        goto fail;
    }
    pub.reg_addr = reg_addr;
    g_executors[exe_type][core].base[slot] = pub.reg_task_id;
    ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
    *out = pub;
    return 0;
fail:
    ctrl->free_bitmap[queue_type][slot] |= mask;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    return -1;
}

/* ===== 5. Schedule path ===== */

static int send_task_mix(ctrl_t *ctrl)
{
    int sent = 0;
    int cl_core[AIC_CNT];
    int cl_slot[AIC_CNT];
    int ncl = 0;
    int used = 0;

    /* Snapshot this lane's idle MIX clusters; a task's block range is fanned
     * across them (simpler dispatch_shape range-claim over available clusters). */
    for (int core = 0; core < AIC_CNT; core++) {
        int slot;

        if (CORE_LANE(core) != ctrl->tid) {
            continue;
        }
        if (dispatch_mix_cluster_idle(ctrl, core, &slot)) {
            cl_core[ncl] = core;
            cl_slot[ncl] = slot;
            ncl++;
        }
    }

    while (used < ncl) {
        uint16_t one;
        uint16_t cnt1 = 1;
        uint32_t start;
        int n;
        EslPublishHandle pubs[3 * AIC_CNT];
        int phys_arr[3 * AIC_CNT];
        int np = 0;
        int fail = 0;
        int b = 0;

        if (!batch_dequeue(&g_shared_ready[TASK_TYPE_MIX], &one, &cnt1) || cnt1 < 1) {
            break;
        }
        n = dispatch_spmd_claim_range(one, ncl - used, &start);
        if (n <= 0) {
            continue;
        }
        for (; b < n; b++) {
            int core = cl_core[used];
            int slot = cl_slot[used];

            used++;
            dispatch_mix_occupy_cluster(ctrl, core, slot, one, start + (uint32_t)b);
            if (dispatch_mix_prepare_cluster(ctrl, core, slot, one, start + (uint32_t)b, pubs, phys_arr, &np) != 0) {
                dispatch_mix_release_cluster(ctrl, core, slot);
                fail = 1;
                break;
            }
            sent++;
        }
        dispatch_mix_flush(pubs, phys_arr, np);
        if (fail) {
            dispatch_spmd_rewind(one, start + (uint32_t)n, start + (uint32_t)b);
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
            break;
        }
        if (dispatch_spmd_has_remaining(one)) {
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
        }
    }
    return sent;
}

// TODO: Work Stealing
static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];

#if !ESL_DISPATCH_DOUBLE_BUFFER
    for (int core = 0; core < AIC_CNT; core++) {
        if (dispatch_mix_core_busy(core)) {
            free_bitmap &= ~((uint64_t)1 << core);
        }
    }
#endif

    int cnt = __builtin_popcountll(free_bitmap);

    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    uint16_t pop_cnt = (uint16_t)cnt;
    if (!batch_dequeue(&g_shared_ready[type], task_ids, &pop_cnt)) {
        return 0;
    }
    cnt = (int)pop_cnt;

    int sent = 0;
    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        int avail = __builtin_popcountll(free_bitmap);
        uint32_t start;
        int n;

        if (avail <= 0) {
            batch_enqueue(&g_shared_ready[type], &task_ids[i], (uint16_t)(cnt - i));
            break;
        }
        n = dispatch_spmd_claim_range(task_id, avail, &start);
        if (n <= 0) {
            continue;
        }

        struct {
            int core;
            int slot;
            EslPublishHandle h;
        } pend[AIC_CNT];
        int np = 0;
        int fail = 0;
        int b = 0;

        for (; b < n; b++) {
            uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
            uint64_t mask = (uint64_t)0x1 << idx;
            int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
            int core = (int)idx;
            EslPublishHandle h;
            int fast = 0;

            g_executors[exe_type][core].tasks[slot] = task_id;
            g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
            g_executors[exe_type][core].idx = (uint8_t)slot;
            if (slot == 1) {
                ctrl->task_id_map2[type][idx] = task_id;
            } else {
                ctrl->task_id_map1[type][idx] = task_id;
            }
            ctrl->free_bitmap[type][slot] &= ~mask;
            g_executors[exe_type][core].block_idx[slot] = (uint16_t)platform_pick_phys_worker(core, exe_type);
            free_bitmap &= ~mask;

            if (dispatch_publish_block_prepare(ctrl, exe_type, type, task_id, start + (uint32_t)b, core, slot, &h,
                                               &fast) != 0) {
                fail = 1;
                break;
            }
            sent++;
            if (!fast) {
                pend[np].core = core;
                pend[np].slot = slot;
                pend[np].h = h;
                np++;
            }
        }

        if (np > 0) {
            wmb();
            for (int p = 0; p < np; p++) {
                esl_publish_subtask_to_core(pend[p].h);
                dispatch_mark_slot_complete(exe_type, pend[p].core, pend[p].slot, pend[p].h.reg_addr,
                                            pend[p].h.reg_task_id);
            }
        }

        if (fail) {
            dispatch_spmd_rewind(task_id, start + (uint32_t)n, start + (uint32_t)b);
            batch_enqueue(&g_shared_ready[type], &task_ids[i], (uint16_t)(cnt - i));
            break;
        }
        if (dispatch_spmd_has_remaining(task_id)) {
            batch_enqueue(&g_shared_ready[type], &task_id, 1);
        }
    }
    return sent;
}

static int dispatch_prefetch(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    int sent = 0;

    for (int core = 0; core < AIC_CNT; core++) {
        int busy_slot = -1;
        int free_slot = -1;

        if (CORE_LANE(core) != ctrl->tid) {
            continue;
        }
#if !ESL_DISPATCH_DOUBLE_BUFFER
        if (dispatch_mix_core_busy(core)) {
            continue;
        }
#endif
        for (int s = 0; s < AIC_OSTD; s++) {
            if (g_executors[exe_type][core].tasks[s] != EXEC_SLOT_EMPTY) {
                busy_slot = (busy_slot < 0) ? s : -2;
            } else {
                free_slot = (free_slot < 0) ? s : -2;
            }
        }
        if (busy_slot < 0 || free_slot < 0) {
            continue;
        }
        const uint64_t mask = (uint64_t)1 << core;
        if (!(ctrl->free_bitmap[type][free_slot] & mask)) {
            continue;
        }
        const uint32_t reg_busy = (uint32_t)g_executors[exe_type][core].base[busy_slot];
        const int phys = (int)g_executors[exe_type][core].block_idx[busy_slot];
        const uint64_t reg_addr = dispatch_core_reg_addr(phys);
        if (reg_addr == 0 || !platform_reg_task_acked(reg_addr, reg_busy)) {
            continue;
        }

        uint16_t one;
        uint16_t cnt1 = 1;
        uint32_t block_idx;

        if (!batch_dequeue(&g_shared_ready[type], &one, &cnt1) || cnt1 < 1) {
            break;
        }
        if (!dispatch_spmd_claim_block(one, &block_idx)) {
            break;
        }
        g_executors[exe_type][core].idx = (uint8_t)free_slot;
        if (free_slot == 1) {
            ctrl->task_id_map2[type][core] = one;
        } else {
            ctrl->task_id_map1[type][core] = one;
        }
        ctrl->free_bitmap[type][free_slot] &= ~mask;
        g_executors[exe_type][core].tasks[free_slot] = one;
        g_executors[exe_type][core].duration[free_slot] = g_basic_buf[one & RING_MASK].duration;
        g_executors[exe_type][core].block_idx[free_slot] =
            (uint16_t)platform_pick_phys_worker(core, exe_type);
        if (dispatch_publish_block(ctrl, exe_type, type, one, block_idx, core, free_slot) != 0) {
            batch_enqueue(&g_shared_ready[type], &one, 1);
            break;
        }
        if (dispatch_spmd_has_remaining(one)) {
            batch_enqueue(&g_shared_ready[type], &one, 1);
        }
        WORKER_LOGF("prefetch,task_id,%u,core,%d,slot,%d,type,%d,block,%u", one, core, free_slot,
                    type, (unsigned)block_idx);
        sent++;
    }
    return sent;
}

static int dispatch(int tid)
{
    int total_sent = 0;
    uint16_t task_id[DISPATCH_COMPLETE_BATCH];
    int complete_cnt;

    atomic_thread_fence(memory_order_acquire);
    dispatch_merge_msg_to_free(&g_ctrl_t[tid]);
    complete_cnt = dispatch_push_completed_slots(&g_ctrl_t[tid], task_id, DISPATCH_COMPLETE_BATCH);
    if (complete_cnt > 0) {
        batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
        atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_release);
        wmb();
    }
    total_sent += send_task_mix(&g_ctrl_t[tid]);
#if !ESL_DISPATCH_DOUBLE_BUFFER
    total_sent += dispatch_mix_prefetch(&g_ctrl_t[tid]);
#endif
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    total_sent += dispatch_prefetch(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += dispatch_prefetch(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

/* ===== 6. Worker ===== */

static void dispatch_publish_final_stats(uint64_t elapsed_ns)
{
    int end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    int first_uncomp = -1;
    int n_uncomp = 0;

    for (int i = 0; i < end; i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            if (first_uncomp < 0) {
                first_uncomp = i;
            }
            n_uncomp++;
        }
    }

    uint64_t pred0 = (first_uncomp >= 0) ? (uint64_t)g_predecessor_cnt[first_uncomp] : 0;
    uint64_t rqc = (uint64_t)g_shared_ready[TASK_TYPE_CUBE].cnt;
    uint64_t rqv = (uint64_t)g_shared_ready[TASK_TYPE_VECTOR].cnt;

    platform_stats_publish((uint64_t)end, (uint64_t)g_subtask_cnt, (uint64_t)g_completed_cnt,
                           ((uint64_t)(uint32_t)atomic_load_explicit(&g_commit_task_id, memory_order_acquire)),
                           (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                           (rqc & 0xffffffffULL) | (rqv << 32), elapsed_ns);
}

void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;

    int total_sent = 0;
    uint64_t start_ns = get_time_ns();

#if ESL_ORCH_FIRST
    /* Orchestrator-first model: wait for the orchestrator to fully finish before
     * dispatching (see cutter_worker). The loop below then falls straight into
     * the drain loop since g_orch_is_done is already set. */
    while (!atomic_load_explicit(&g_orch_is_done, memory_order_acquire)) {
        spin_wait();
    }
#endif

    /* Poll BEFORE dispatch (simpler's Phase-1 completion poll ahead of Phase-4
     * dispatch): harvest FINs first so dispatch() merges free cores and
     * re-dispatches the freed cores in the SAME iteration, instead of carrying the
     * completion across the spin to next iteration. */
    while (!atomic_load_explicit(&g_orch_is_done, memory_order_relaxed)) {
        dispatch_poll(tid);
        total_sent += dispatch(tid);
        spin_wait();
    }
    while (atomic_load_explicit(&g_completed_cnt, memory_order_acquire) <
           atomic_load_explicit(&g_task_id, memory_order_acquire)) {
        dispatch_poll(tid);
        total_sent += dispatch(tid);
        spin_wait();
    }

    atomic_store_explicit(&g_is_done, true, memory_order_release);
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[scheduler] lane %d dispatched %d subtasks", tid, total_sent);
    MAIN_LOGF("[scheduler] task_cnt = %u", g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",(float)(g_completed_cnt * 1000.0 / elapsed_ns));
    if (tid == 0) {
        dispatch_publish_final_stats(elapsed_ns);
    }

    return NULL;
}

/* ===== 4. MIX ===== */

static int dispatch_mix_aic_phys(int core)
{
    return core;
}

static int dispatch_mix_aiv0_phys(int core)
{
    return ESL_PROXY_WORKER_BLOCK_DIM + core * ESL_PROXY_AIV_LANES_PER_BLOCK;
}

static int dispatch_mix_aiv1_phys(int core)
{
    return ESL_PROXY_WORKER_BLOCK_DIM + core * ESL_PROXY_AIV_LANES_PER_BLOCK + 1;
}

static int dispatch_mix_cluster_idle(ctrl_t *ctrl, int core, int *out_slot)
{
    const uint64_t mask = (uint64_t)1 << core;

    for (int s = 0; s < AIC_OSTD; s++) {
        const int other = 1 - s;

        if (g_executors[TASK_TYPE_CUBE][core].tasks[s] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (g_executors[TASK_TYPE_VECTOR][core].tasks[s] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (g_executors[TASK_TYPE_VECTOR][core].tasks[other] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_CUBE][s] & mask)) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_VECTOR][s] & mask)) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_VECTOR][other] & mask)) {
            continue;
        }
        if (out_slot != NULL) {
            *out_slot = s;
        }
        return 1;
    }
    return 0;
}

#if !ESL_DISPATCH_DOUBLE_BUFFER
static int dispatch_mix_cluster_pending(ctrl_t *ctrl, int core, int *out_busy_slot, int *out_free_slot)
{
    const uint64_t mask = (uint64_t)1 << core;

    for (int busy = 0; busy < AIC_OSTD; busy++) {
        const int free = 1 - busy;

        if (!g_mix_active[core][busy]) {
            continue;
        }
        if (g_executors[TASK_TYPE_CUBE][core].tasks[free] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (g_executors[TASK_TYPE_VECTOR][core].tasks[free] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (g_executors[TASK_TYPE_VECTOR][core].tasks[busy] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_CUBE][free] & mask)) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_VECTOR][free] & mask)) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_VECTOR][busy] & mask)) {
            continue;
        }
        if (out_busy_slot != NULL) {
            *out_busy_slot = busy;
        }
        if (out_free_slot != NULL) {
            *out_free_slot = free;
        }
        return 1;
    }
    return 0;
}
#endif

#if !ESL_DISPATCH_DOUBLE_BUFFER
static int dispatch_mix_cluster_subtasks_acked(int core, int slot)
{
    const int other = 1 - slot;
    const int phys_list[3] = {dispatch_mix_aic_phys(core), dispatch_mix_aiv0_phys(core),
                              dispatch_mix_aiv1_phys(core)};
    const int exe_list[3] = {TASK_TYPE_CUBE, TASK_TYPE_VECTOR, TASK_TYPE_VECTOR};
    const int slot_list[3] = {slot, slot, other};
    int p;

    for (p = 0; p < 3; p++) {
        const uint32_t reg_task = (uint32_t)g_executors[exe_list[p]][core].base[slot_list[p]];
        const uint64_t reg_addr = dispatch_core_reg_addr(phys_list[p]);

        if (reg_task == 0U || reg_addr == 0 ||
            !platform_reg_task_acked(reg_addr, reg_task)) {
            return 0;
        }
    }
    return 1;
}
#endif

#if !ESL_DISPATCH_DOUBLE_BUFFER
static int dispatch_mix_core_busy(int core)
{
    for (int s = 0; s < AIC_OSTD; s++) {
        if (g_mix_active[core][s] != 0) {
            return 1;
        }
    }
    return 0;
}
#endif

static int dispatch_mix_prepare_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx,
                                 EslPublishHandle pubs[], int phys_arr[], int *np)
{
    const int other = 1 - slot;
    const int phys_aic = dispatch_mix_aic_phys(core);
    const int phys_aiv0 = dispatch_mix_aiv0_phys(core);
    const int phys_aiv1 = dispatch_mix_aiv1_phys(core);

    (void)ctrl;
    g_executors[TASK_TYPE_CUBE][core].base[slot] = 0;
    g_executors[TASK_TYPE_VECTOR][core].base[slot] = 0;
    g_executors[TASK_TYPE_VECTOR][core].base[other] = 0;
    if (g_runtime != NULL && phys_aic >= g_runtime->worker_count) {
        const uint64_t mask = (uint64_t)1 << core;

        g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_CUBE][slot] |= mask;
        g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_VECTOR][slot] |= mask;
        g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_VECTOR][other] |= mask;
        return 0; /* fast-complete: no handles */
    }
    const int phys_list[3] = {phys_aic, phys_aiv0, phys_aiv1};
    const int exe_list[3] = {TASK_TYPE_CUBE, TASK_TYPE_VECTOR, TASK_TYPE_VECTOR};
    const int slot_list[3] = {slot, slot, other};
    const int start_np = *np;
    int p;

    for (p = 0; p < 3; p++) {
        const uint64_t reg_addr = dispatch_core_reg_addr(phys_list[p]);
        EslPublishHandle pub;

        if (reg_addr == 0) {
            *np = start_np;
            return -1;
        }
        pub = esl_prepare_subtask_to_core(g_runtime, phys_list[p], task_id, block_idx);
        if (pub.reg_task_id == 0U) {
            *np = start_np;
            return -1;
        }
        pub.reg_addr = reg_addr;
        g_executors[exe_list[p]][core].base[slot_list[p]] = pub.reg_task_id;
        pubs[*np] = pub;
        phys_arr[*np] = phys_list[p];
        (*np)++;
    }
    return 0;
}


static void dispatch_mix_flush(EslPublishHandle pubs[], const int phys_arr[], int np)
{
    int i;

    if (np <= 0) {
        return;
    }
    wmb();
    for (i = 0; i < np; i++) {
        ESL_SWIMLANE_AICPU_ON_DISPATCH(phys_arr[i], ESL_AICPU_ROLE_DISPATCH);
        esl_publish_subtask_to_core(pubs[i]);
    }
}

#if !ESL_DISPATCH_DOUBLE_BUFFER
static int dispatch_mix_publish_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id,
                                 uint32_t block_idx)
{
    EslPublishHandle pubs[3];
    int phys_arr[3];
    int np = 0;

    if (dispatch_mix_prepare_cluster(ctrl, core, slot, task_id, block_idx, pubs, phys_arr, &np) != 0) {
        return -1;
    }
    dispatch_mix_flush(pubs, phys_arr, np);
    return 0;
}
#endif

#if !ESL_DISPATCH_DOUBLE_BUFFER
static int dispatch_mix_prefetch(ctrl_t *ctrl)
{
    int sent = 0;

    for (int core = 0; core < AIC_CNT; core++) {
        int slot;
        int busy_slot = -1;
        uint16_t one;
        uint16_t cnt1 = 1;
        uint32_t block_idx;

        if (CORE_LANE(core) != ctrl->tid) {
            continue;
        }
        if (!dispatch_mix_cluster_idle(ctrl, core, &slot)) {
            if (!dispatch_mix_cluster_pending(ctrl, core, &busy_slot, &slot)) {
                continue;
            }
            if (!dispatch_mix_cluster_subtasks_acked(core, busy_slot)) {
                continue;
            }
        }
        if (!batch_dequeue(&g_shared_ready[TASK_TYPE_MIX], &one, &cnt1) || cnt1 < 1) {
            continue;
        }
        if (!dispatch_spmd_claim_block(one, &block_idx)) {
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
            continue;
        }
        dispatch_mix_occupy_cluster(ctrl, core, slot, one, block_idx);
        if (dispatch_mix_publish_cluster(ctrl, core, slot, one, block_idx) != 0) {
            dispatch_mix_release_cluster(ctrl, core, slot);
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
            continue;
        }
        if (dispatch_spmd_has_remaining(one)) {
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
        }
        sent++;
    }
    return sent;
}
#endif

static int dispatch_mix_cluster_all_done(ctrl_t *ctrl, int core, int slot)
{
    const int other = 1 - slot;
    const uint64_t mask = (uint64_t)1 << core;

    return (ctrl->msg_bitmap[TASK_TYPE_CUBE][slot] & mask) &&
           (ctrl->msg_bitmap[TASK_TYPE_VECTOR][slot] & mask) &&
           (ctrl->msg_bitmap[TASK_TYPE_VECTOR][other] & mask);
}

static int dispatch_mix_defer_slot_clear(int exe_type, int core, int slot)
{
    if (exe_type == TASK_TYPE_CUBE) {
        return g_mix_active[core][slot] != 0;
    }
    for (int s = 0; s < AIC_OSTD; s++) {
        if (g_mix_active[core][s] != 0 && (slot == s || slot == (1 - s))) {
            return 1;
        }
    }
    return 0;
}

static int dispatch_mix_partial_pending(ctrl_t *ctrl, int exe_type, int core, int slot)
{
    if (exe_type == TASK_TYPE_CUBE) {
        if (!g_mix_active[core][slot]) {
            return 0;
        }
        return !dispatch_mix_cluster_all_done(ctrl, core, slot);
    }
    for (int s = 0; s < AIC_OSTD; s++) {
        if (!g_mix_active[core][s]) {
            continue;
        }
        if (slot == s || slot == (1 - s)) {
            return !dispatch_mix_cluster_all_done(ctrl, core, s);
        }
    }
    return 0;
}

static void dispatch_merge_msg_to_free(ctrl_t *ctrl)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            uint64_t bitmap = ctrl->msg_bitmap[i][j];
            uint64_t safe = 0;

            while (bitmap != 0) {
                const int core = (int)__builtin_ctzll(bitmap);
                const uint64_t mask = (uint64_t)1 << core;

                if (!dispatch_mix_partial_pending(ctrl, i, core, j)) {
                    safe |= mask;
                }
                bitmap &= ~mask;
            }
            ctrl->free_bitmap[i][j] |= safe;
        }
    }
    for (int j = 0; j < AIC_OSTD; j++) {
        ctrl->free_bitmap[TASK_TYPE_MIX][j] =
            ctrl->free_bitmap[TASK_TYPE_CUBE][j] & ctrl->free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

static void dispatch_mix_occupy_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx)
{
    const int other = 1 - slot;
    const uint64_t mask = (uint64_t)1 << core;
    const uint32_t dur = g_basic_buf[task_id & RING_MASK].duration;

    ctrl->free_bitmap[TASK_TYPE_CUBE][slot] &= ~mask;
    ctrl->free_bitmap[TASK_TYPE_VECTOR][slot] &= ~mask;
    ctrl->free_bitmap[TASK_TYPE_VECTOR][other] &= ~mask;
    ctrl->free_bitmap[TASK_TYPE_MIX][slot] &= ~mask;

    g_executors[TASK_TYPE_CUBE][core].tasks[slot] = task_id;
    g_executors[TASK_TYPE_CUBE][core].duration[slot] = dur;
    g_executors[TASK_TYPE_CUBE][core].block_idx[slot] = (uint16_t)dispatch_mix_aic_phys(core);
    g_slot_block_idx[TASK_TYPE_CUBE][core][slot] = block_idx;

    g_executors[TASK_TYPE_VECTOR][core].tasks[slot] = task_id;
    g_executors[TASK_TYPE_VECTOR][core].duration[slot] = dur;
    g_executors[TASK_TYPE_VECTOR][core].block_idx[slot] = (uint16_t)dispatch_mix_aiv0_phys(core);
    g_slot_block_idx[TASK_TYPE_VECTOR][core][slot] = block_idx;

    g_executors[TASK_TYPE_VECTOR][core].tasks[other] = task_id;
    g_executors[TASK_TYPE_VECTOR][core].duration[other] = dur;
    g_executors[TASK_TYPE_VECTOR][core].block_idx[other] = (uint16_t)dispatch_mix_aiv1_phys(core);
    g_slot_block_idx[TASK_TYPE_VECTOR][core][other] = block_idx;

    if (slot == 1) {
        ctrl->task_id_map2[TASK_TYPE_CUBE][core] = task_id;
        ctrl->task_id_map2[TASK_TYPE_VECTOR][core] = task_id;
    } else {
        ctrl->task_id_map1[TASK_TYPE_CUBE][core] = task_id;
        ctrl->task_id_map1[TASK_TYPE_VECTOR][core] = task_id;
    }

    g_mix_active[core][slot] = 1;
    g_mix_task[core][slot] = task_id;
}

static void dispatch_mix_release_cluster(ctrl_t *ctrl, int core, int slot)
{
    const int other = 1 - slot;
    const uint64_t mask = (uint64_t)1 << core;

    g_mix_active[core][slot] = 0;
    g_mix_task[core][slot] = 0;

    g_executors[TASK_TYPE_CUBE][core].tasks[slot] = EXEC_SLOT_EMPTY;
    g_executors[TASK_TYPE_VECTOR][core].tasks[slot] = EXEC_SLOT_EMPTY;
    g_executors[TASK_TYPE_VECTOR][core].tasks[other] = EXEC_SLOT_EMPTY;
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_CUBE][slot] &= ~mask;
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_VECTOR][slot] &= ~mask;
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_VECTOR][other] &= ~mask;
    ctrl->free_bitmap[TASK_TYPE_CUBE][slot] |= mask;
    ctrl->free_bitmap[TASK_TYPE_VECTOR][slot] |= mask;
    ctrl->free_bitmap[TASK_TYPE_VECTOR][other] |= mask;
    ctrl->free_bitmap[TASK_TYPE_MIX][slot] |= mask;
}

static int dispatch_mix_harvest_completed(ctrl_t *ctrl, uint16_t done_tasks[], int max_out)
{
    int out = 0;

    for (int core = 0; core < AIC_CNT; core++) {
        if (CORE_LANE(core) != ctrl->tid) {
            continue;
        }
        for (int s = 0; s < AIC_OSTD; s++) {
            uint16_t task_id;

            if (!g_mix_active[core][s]) {
                continue;
            }
            task_id = g_mix_task[core][s];
            if (!dispatch_mix_cluster_all_done(ctrl, core, s)) {
                continue;
            }
            dispatch_mix_release_cluster(ctrl, core, s);
            if (dispatch_spmd_note_block_done(task_id)) {
                if (out < max_out) {
                    done_tasks[out++] = task_id;
                }
            }
        }
    }
    return out;
}

static int dispatch_push_completed_slots(ctrl_t *ctrl, uint16_t out_tasks[], int max_out)
{
    int out = dispatch_mix_harvest_completed(ctrl, out_tasks, max_out);
    int i;

    for (i = 0; i < EXE_TYPE_CNT; i++) {
        uint64_t bitmap0 = ctrl->msg_bitmap[i][0];
        uint64_t bitmap1 = ctrl->msg_bitmap[i][1];
        uint64_t keep0 = ctrl->msg_bitmap[i][0];
        uint64_t keep1 = ctrl->msg_bitmap[i][1];

        while (bitmap0 != 0) {
            const int core = (int)__builtin_ctzll(bitmap0);
            const uint64_t mask = (uint64_t)1 << core;
            uint16_t tid_done = ctrl->task_id_map1[i][core];

            bitmap0 &= ~mask;
            if (g_basic_buf[tid_done & RING_MASK].type == TASK_TYPE_MIX ||
                dispatch_mix_partial_pending(ctrl, i, core, 0)) {
                continue;
            }
            keep0 &= ~mask;
            if (dispatch_spmd_note_block_done(tid_done)) {
                if (out < max_out) {
                    out_tasks[out++] = tid_done;
                }
            }
        }
        while (bitmap1 != 0) {
            const int core = (int)__builtin_ctzll(bitmap1);
            const uint64_t mask = (uint64_t)1 << core;
            uint16_t tid_done = ctrl->task_id_map2[i][core];

            bitmap1 &= ~mask;
            if (g_basic_buf[tid_done & RING_MASK].type == TASK_TYPE_MIX ||
                dispatch_mix_partial_pending(ctrl, i, core, 1)) {
                continue;
            }
            keep1 &= ~mask;
            if (dispatch_spmd_note_block_done(tid_done)) {
                if (out < max_out) {
                    out_tasks[out++] = tid_done;
                }
            }
        }
        ctrl->msg_bitmap[i][0] = keep0;
        ctrl->msg_bitmap[i][1] = keep1;
    }
    return out;
}

/* ===== 3. SPMD ===== */

__attribute__((weak)) void dispatch_spmd_on_ready(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;

    g_next_block[slot] = 0;
    g_finished_blocks[slot] = 0;
}


static int dispatch_spmd_claim_range(uint16_t task_id, int avail, uint32_t *start_block)
{
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;

    if (avail <= 0) {
        return 0;
    }
    uint16_t cur = g_next_block[slot];
    uint32_t start;
    uint32_t n;

    start = cur;
    if (total <= 1U) {
        if (start > 0U) {
            return 0;
        }
        n = 1U;
    } else {
        if (start >= total) {
            return 0;
        }
        n = ((uint32_t)avail < (total - start)) ? (uint32_t)avail : (total - start);
    }
    g_next_block[slot] = (uint16_t)(start + n);
    if (start_block != NULL) {
        *start_block = start;
    }
    return (int)n;
}

static int dispatch_spmd_rewind(uint16_t task_id, uint32_t claimed_end, uint32_t next_block)
{
    (void)claimed_end;
    g_next_block[task_id & RING_MASK] = (uint16_t)next_block;
    return 1;
}

static int dispatch_spmd_claim_block(uint16_t task_id, uint32_t *block_idx)
{
    uint32_t s = 0;
    int n = dispatch_spmd_claim_range(task_id, 1, &s);

    if (n > 0 && block_idx != NULL) {
        *block_idx = s;
    }
    return n > 0 ? 1 : 0;
}

static int dispatch_spmd_has_remaining(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;

    if (total <= 1U) {
        return 0;
    }
    return g_next_block[slot] < total;
}

static int dispatch_spmd_note_block_done(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;

    uint16_t prev = atomic_fetch_add_explicit(&g_finished_blocks[slot], 1, memory_order_acq_rel);

    return (uint32_t)(prev + 1U) == total;
}

