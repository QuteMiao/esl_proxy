/*
 * dispatch_a3.c - A3 onboard dispatch (doorbell prepare/publish + COND).
 *
 * Selected by cmake/sources.cmake and Makefile. Legacy Fake Return remains in
 * dispatch.c for historical / non-A3 builds (PR #13).
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
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern uint16_t g_commit_task_id;
extern int g_subtask_cnt;

EslRuntime *g_runtime;

/* ===== Payload prepare / publish (inlined from former dispatch_payload.c) ===== */

#define ESL_FAKE_KERNEL_FUNC_ID_AIC 0U
#define ESL_FAKE_KERNEL_FUNC_ID_AIV 1U

static uint32_t g_core_dispatch_seq[RUNTIME_MAX_WORKER];

static uint32_t dispatch_next_reg_task_id(int phys)
{
    uint32_t seq;
    uint32_t reg_id;

    if (phys < 0 || phys >= RUNTIME_MAX_WORKER) {
        return 0;
    }
    seq = ++g_core_dispatch_seq[phys];
    reg_id = seq & (uint32_t)TASK_ID_MASK;
    if (reg_id >= (uint32_t)AICORE_EXIT_SIGNAL) {
        g_core_dispatch_seq[phys] = seq + ((uint32_t)AICORE_EXIT_SIGNAL - reg_id);
        reg_id = (uint32_t)(g_core_dispatch_seq[phys] & (uint32_t)TASK_ID_MASK);
    }
    return reg_id;
}

void esl_init_global_context(EslRuntime *runtime)
{
    (void)runtime;
    /* task_desc slots need no PTO2-style LocalContext init. */
}

EslPublishHandle esl_prepare_subtask_to_core(EslRuntime *runtime, int core, uint16_t task_id, uint32_t block_idx)
{
    return esl_prepare_subtask_to_core_gated(runtime, core, task_id, block_idx, 0);
}

EslPublishHandle esl_prepare_subtask_to_core_gated(EslRuntime *runtime, int core, uint16_t task_id,
                                                   uint32_t block_idx, int not_ready)
{
    EslPublishHandle handle = {0, 0};
    struct task_desc *slot;
    uint64_t base;
    uint32_t reg_task_id;
    int slot_i;
    const struct task_desc *desc;
    uint64_t fake_kernel_addr;

    if (runtime == NULL || core < 0 || core >= RUNTIME_MAX_WORKER) {
        return handle;
    }

    base = runtime->workers[core].task;
    if (base == 0) {
        return handle;
    }

    reg_task_id = dispatch_next_reg_task_id(core);
    slot_i = (int)(reg_task_id & 1u);
    slot = (struct task_desc *)(uintptr_t)(base + (uint64_t)slot_i * sizeof(struct task_desc));
    desc = &g_basic_buf[task_id & RING_MASK];
    fake_kernel_addr = runtime->func_id_to_addr_[runtime->workers[core].core_type == 0
                                                      ? ESL_FAKE_KERNEL_FUNC_ID_AIC
                                                      : ESL_FAKE_KERNEL_FUNC_ID_AIV];
    /* Build in place: copy g_basic_buf entry into GM slot (no separate payload type). */
    *slot = *desc;
    slot->kernel = (void *)(uintptr_t)fake_kernel_addr;
    slot->index = block_idx;
    slot->scalar[ESL_GM_TASK_NOT_READY_SCALAR] = not_ready ? 1 : 0;
    handle.reg_task_id = reg_task_id;
    return handle;
}

void esl_publish_subtask_to_core(EslPublishHandle handle)
{
    if (handle.reg_addr == 0U || handle.reg_task_id == 0U) {
        return;
    }
    write_reg(handle.reg_addr, REG_ID_DATA_MAIN_BASE, handle.reg_task_id);
}

/* ===== 1. Forward decls ===== */

static void dispatch_merge_msg_to_free(ctrl_t *ctrl);
static int dispatch_push_completed_slots(ctrl_t *ctrl, uint16_t out_tasks[], int max_out);

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
    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
}

/* Complete a already-decided slot without re-checking FIN (poll inference path). */
static void dispatch_force_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                    uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (g_ctrl_t[0].msg_bitmap[exe_type][slot] & mask) {
        return;
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
}

/* Pull AICore FIN events into msg_bitmap. */
void dispatch_poll(int tid)
{
    (void)tid;
    if (g_runtime == NULL) {
        return;
    }
    const int n_workers = g_runtime->worker_count;
    const int n_cores = AIC_CNT;

    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int core = 0; core < n_cores; core++) {
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
                 * (串行执行)化解覆盖丢失 —— 见 simpler decide_slot_transition。 */
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
        g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
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

/* Batched range-publish: prepare only; caller then publish+mark. */
static int dispatch_publish_block_prepare(ctrl_t *ctrl, int exe_type, int queue_type, uint16_t task_id,
                                          uint32_t block_idx, int core, int slot,
                                          EslPublishHandle *out, int *fast)
{
    const uint64_t mask = (uint64_t)1 << core;

    *fast = 0;
    if (g_runtime != NULL && (int)g_executors[exe_type][core].block_idx[slot] >= g_runtime->worker_count) {
        g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
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
    *out = pub;
    return 0;
fail:
    ctrl->free_bitmap[queue_type][slot] |= mask;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    return -1;
}

/* ===== 5. Schedule path ===== */

// TODO: Work Stealing
static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    // Check both slots - slot is free if neither slot 0 nor slot 1 has been sent a task
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];
    int cnt = __builtin_popcountll(free_bitmap);

    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    uint16_t pop_cnt = (uint16_t)cnt;
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &pop_cnt)) {
        return 0;
    }
    cnt = (int)pop_cnt;

    int sent = 0;
    struct {
        int core;
        int slot;
        EslPublishHandle h;
    } pend[AIC_CNT];
    int np = 0;

    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        uint64_t idx;
        uint64_t mask;
        int slot;
        int core;
        EslPublishHandle h;
        int fast = 0;

        if (free_bitmap == 0) {
            batch_enqueue(&ctrl->ready_queue[type], &task_ids[i], (uint16_t)(cnt - i));
            break;
        }
        /* count==1: one core per task, always block_idx=0 (no SPMD claim). */
        idx = (uint64_t)__builtin_ctzll(free_bitmap);
        mask = (uint64_t)0x1 << idx;
        slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        core = (int)idx;

        g_executors[exe_type][core].tasks[slot] = task_id;
        g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].idx = (uint8_t)slot;
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }
        // Clear the free bit for this core/slot combination (mark as busy)
        ctrl->free_bitmap[type][slot] &= ~mask;
        g_executors[exe_type][core].block_idx[slot] = (uint16_t)platform_pick_phys_worker(core, exe_type);
        free_bitmap &= ~mask;

        // Fake Return — replaced by prepare/publish + COND poll on onboard
        if (dispatch_publish_block_prepare(ctrl, exe_type, type, task_id, 0 /* block_idx */, core, slot, &h,
                                           &fast) != 0) {
            batch_enqueue(&ctrl->ready_queue[type], &task_ids[i], (uint16_t)(cnt - i));
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
        for (int p = 0; p < np; p++) {
            esl_publish_subtask_to_core(pend[p].h);
            dispatch_mark_slot_complete(exe_type, pend[p].core, pend[p].slot, pend[p].h.reg_addr,
                                        pend[p].h.reg_task_id);
        }
    }
    return sent;
}

static void dispatch_merge_msg_to_free(ctrl_t *ctrl)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            ctrl->free_bitmap[i][j] |= ctrl->msg_bitmap[i][j];
        }
    }
    /* Keep MIX free mask derived from CUBE&VECTOR (unused in PR-A schedule). */
    for (int j = 0; j < AIC_OSTD; j++) {
        ctrl->free_bitmap[TASK_TYPE_MIX][j] =
            ctrl->free_bitmap[TASK_TYPE_CUBE][j] & ctrl->free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

// TODO: add counter for spmd
/* count==1: each msg bit means the task is done; clear and push immediately. */
static int dispatch_push_completed_slots(ctrl_t *ctrl, uint16_t out_tasks[], int max_out)
{
    int out = 0;
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
            if (g_basic_buf[tid_done & RING_MASK].type == TASK_TYPE_MIX) {
                continue;
            }
            keep0 &= ~mask;
            if (out < max_out) {
                out_tasks[out++] = tid_done;
            }
        }
        while (bitmap1 != 0) {
            const int core = (int)__builtin_ctzll(bitmap1);
            const uint64_t mask = (uint64_t)1 << core;
            uint16_t tid_done = ctrl->task_id_map2[i][core];

            bitmap1 &= ~mask;
            if (g_basic_buf[tid_done & RING_MASK].type == TASK_TYPE_MIX) {
                continue;
            }
            keep1 &= ~mask;
            if (out < max_out) {
                out_tasks[out++] = tid_done;
            }
        }
        ctrl->msg_bitmap[i][0] = keep0;
        ctrl->msg_bitmap[i][1] = keep1;
    }
    return out;
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
    }
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
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
    uint64_t rqc = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE].cnt;
    uint64_t rqv = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_VECTOR].cnt;

    platform_stats_publish((uint64_t)end, (uint64_t)g_subtask_cnt, (uint64_t)g_completed_cnt,
                           ((uint64_t)(uint32_t)g_commit_task_id),
                           (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                           (rqc & 0xffffffffULL) | (rqv << 32), elapsed_ns);
}

void *dispatch_worker(void *arg)
{
    // atomic_store(&g_is_done, true);
    // return NULL;
    int tid = (int)(intptr_t)arg;

    int total_sent = 0;
    uint64_t start_ns = get_time_ns();

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

    MAIN_LOGF("[scheduler] dispatched %d subtasks", total_sent);
    (void)tid;
    MAIN_LOGF("[scheduler] task_cnt = %u", g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",(float)(g_completed_cnt * 1000.0 / elapsed_ns));
    if (tid == 0) {
        dispatch_publish_final_stats(elapsed_ns);
    }

    return NULL;
}

