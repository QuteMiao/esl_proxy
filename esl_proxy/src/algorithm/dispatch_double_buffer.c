/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#define _GNU_SOURCE

#include "dispatch.h"
#include "handshake.h"
#include "runtime.h"

#include "cutter.h"
#include "executor.h"
#include "log.h"
#include "memory_barrier.h"
#include "ring_buf.h"
#include "spin.h"
#include "swimlane_aicpu.h"

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
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern int g_subtask_cnt;
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

EslRuntime *g_runtime;

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

/* 完成一个已判定的 slot（不再校验 FIN——调用方已通过 running/pending 推断或直接 FIN 定夺）。
 * 用于双缓冲:同一物理核 2 个在飞时,单 COND 寄存器只留最新值,先发任务的 FIN 会被后发任务
 * 的 ACK/FIN 覆盖;看到后发(pending)事件即可推断先发(running)已完成(AICore 串行执行)。 */
static void dispatch_force_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                    uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (g_ctrl_t[0].msg_bitmap[exe_type][slot] & mask) {
        return; /* 已完成 */
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
}

static inline void set_mix(int tid)
{
    for (int j = 0; j < AIC_OSTD; j++) {
        g_ctrl_t[tid].free_bitmap[TASK_TYPE_MIX][j] =
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_CUBE][j] &
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

static inline void get_free_exe(int tid)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] |= g_ctrl_t[tid].msg_bitmap[i][j];
        }
    }
    set_mix(tid);
}

/* 把 AICore 完成事件拉到 msg_bitmap，供 push_2_completed_queue 解码。 */
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
                if (g_ctrl_t[0].msg_bitmap[exe_type][slot] & mask) {
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
                const uint64_t reg_addr = dispatch_core_reg_addr(phys0);
                if (reg_addr == 0) {
                    continue;
                }
                const uint32_t cond = platform_reg_cond_raw(reg_addr);
                const int id = EXTRACT_TASK_ID((uint64_t)cond);
                const int st = EXTRACT_TASK_STATE((uint64_t)cond);

                if (id == (int)pend_reg) {
                    /* 看到 pending 的 ACK/FIN ⇒ running 必已完成(串行)。 */
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
                    const uint64_t reg_addr = dispatch_core_reg_addr(phys);

                    if (reg_addr != 0 && platform_reg_task_finished(reg_addr, reg_task)) {
                        dispatch_mark_slot_complete(exe_type, core, slot, reg_addr, reg_task);
                    }
                }
            }
        }
    }
}

static inline void get_completed(uint64_t* bitmap, uint16_t task_id[], int *complete_cnt,
                                 const uint16_t task_id_map[])
{
    int cnt = __builtin_popcountll(*bitmap);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(*bitmap);
        task_id[(*complete_cnt)] = task_id_map[idx];
        WORKER_LOGF("completed,complete_cnt,%d,task_id,%u,core,%d,bitmap,%u",*complete_cnt, task_id_map[idx], idx, *bitmap);
        (*complete_cnt)++;
        cnt--;
        *bitmap &= (*bitmap - 1);
    }
}

// TODO: add counter for spmd
static inline void push_2_completed_queue(int tid)
{
    uint16_t task_id[DISPATCH_COMPLETE_BATCH];
    int complete_cnt = 0;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map1[i]);
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map2[i]);
    }
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_release);
    wmb();
}

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
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt)){
        return 0;
    }
    
    int sent = 0;
    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);

        uint64_t mask = (uint64_t)0x1 << idx;
        // Determine which slot to use - prefer slot 0 if it's not busy
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        // Set executor's tasks and duration
        int core = (int)idx;

        /* No cache_civac: g_basic_buf is AICPU-only + the cluster is coherent
         * (atomic-only ready/completed queues prove it); ready_queue acquire orders it. */
        g_executors[exe_type][core].tasks[slot] = task_id;
        g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].idx = slot;  // Point to the slot with the new task
        
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }
        
        // Clear the free bit for this core/slot combination (mark as busy)
        ctrl->free_bitmap[type][slot] &= ~mask;

        const int phys = platform_pick_phys_worker(core, exe_type);
        g_executors[exe_type][core].block_idx[slot] = (uint16_t)phys;
        int rc = 0;
        if (g_runtime != NULL && phys >= g_runtime->worker_count) {
            g_ctrl_t[0].msg_bitmap[exe_type][slot] |= (uint64_t)1 << core;
            g_executors[exe_type][core].base[slot] = 0;
        } else {
            const uint64_t reg_addr = dispatch_core_reg_addr(phys);
            if (reg_addr == 0) {
                rc = -1;
            } else {
                EslPublishHandle pub;

                pub = esl_prepare_subtask_to_core(g_runtime, phys, task_id, 0);
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
        }
        if (rc != 0) {
            ctrl->free_bitmap[type][slot] |= mask;
            g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
            uint16_t one = task_id;

            batch_enqueue(&ctrl->ready_queue[type], &one, 1);
            break;
        }
        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
        free_bitmap &= ~mask;
    }
    return sent;
}

static int dispatch_prefetch(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    int sent = 0;

    for (int core = 0; core < AIC_CNT; core++) {
        /* 24 核完整测试:core 0 也参与预取。 */
        /* 找出恰好一个 busy slot 和一个 free slot。 */
        int busy_slot = -1;
        int free_slot = -1;
        for (int s = 0; s < AIC_OSTD; s++) {
            if (g_executors[exe_type][core].tasks[s] != EXEC_SLOT_EMPTY) {
                busy_slot = (busy_slot < 0) ? s : -2;
            } else {
                free_slot = (free_slot < 0) ? s : -2;
            }
        }
        if (busy_slot < 0 || free_slot < 0) {
            continue; /* 不是恰好一忙一闲 */
        }
        const uint64_t mask = (uint64_t)1 << core;
        if (!(ctrl->free_bitmap[type][free_slot] & mask)) {
            continue; /* free slot 必须真正标记为空闲 */
        }
        /* gate:首任务必须已被 AICore ACK。 */
        const uint32_t reg_busy = (uint32_t)g_executors[exe_type][core].base[busy_slot];
        const int phys = (int)g_executors[exe_type][core].block_idx[busy_slot];
        const uint64_t reg_addr = dispatch_core_reg_addr(phys);
        if (reg_addr == 0 || !platform_reg_task_acked(reg_addr, reg_busy)) {
            continue;
        }
        /* 取一个就绪任务。 */
        uint16_t one;
        uint16_t cnt1 = 1;
        if (!batch_dequeue(&ctrl->ready_queue[type], &one, &cnt1) || cnt1 < 1) {
            break; /* 无就绪任务 — 停止 */
        }
        {
            /* No cache_civac: AICPU-only metadata on a coherent cluster; the
             * ready_queue dequeue (acquire) already orders orchestration's writes. */
            g_executors[exe_type][core].idx = (uint8_t)free_slot;
            if (free_slot == 1) {
                ctrl->task_id_map2[type][core] = one;
            } else {
                ctrl->task_id_map1[type][core] = one;
            }
            ctrl->free_bitmap[type][free_slot] &= ~mask; /* mark busy */
            g_executors[exe_type][core].tasks[free_slot] = one;
            g_executors[exe_type][core].duration[free_slot] = g_basic_buf[one & RING_MASK].duration;
            g_executors[exe_type][core].idx = (uint8_t)free_slot;
            const int phys = platform_pick_phys_worker(core, exe_type);
            g_executors[exe_type][core].block_idx[free_slot] = (uint16_t)phys;
            int rc = 0;
            if (g_runtime != NULL && phys >= g_runtime->worker_count) {
                g_ctrl_t[0].msg_bitmap[exe_type][free_slot] |= (uint64_t)1 << core;
                g_executors[exe_type][core].base[free_slot] = 0;
            } else {
                const uint64_t reg_addr = dispatch_core_reg_addr(phys);
                if (reg_addr == 0) {
                    rc = -1;
                } else {
                    EslPublishHandle pub;

                    pub = esl_prepare_subtask_to_core(g_runtime, phys, one, 0);
                    if (pub.reg_task_id == 0U) {
                        rc = -1;
                    } else {
                        pub.reg_addr = reg_addr;
                        g_executors[exe_type][core].base[free_slot] = pub.reg_task_id;
                        ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
                        wmb();
                        esl_publish_subtask_to_core(pub);
                        dispatch_mark_slot_complete(exe_type, core, free_slot, reg_addr, pub.reg_task_id);
                    }
                }
            }
            if (rc != 0) {
                ctrl->free_bitmap[type][free_slot] |= mask;
                g_executors[exe_type][core].tasks[free_slot] = EXEC_SLOT_EMPTY;
                batch_enqueue(&ctrl->ready_queue[type], &one, 1); /* 被拒 — 放回并停止 */
                break;
            }
        }
        WORKER_LOGF("prefetch,task_id,%u,core,%d,slot,%d,type,%d", one, core, free_slot, type);
        sent++;
    }
    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;

    /* 本轮读取共享状态前 acquire 调度 counter */
    atomic_thread_fence(memory_order_acquire);
    get_free_exe(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    total_sent += dispatch_prefetch(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += dispatch_prefetch(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

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
                           ((uint64_t)(uint32_t)atomic_load_explicit(&g_commit_task_id, memory_order_acquire)),
                           (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                           (rqc & 0xffffffffULL) | (rqv << 32), elapsed_ns);
}

/*
 * Dispatch worker thread entry point
 * Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;

    int total_sent = 0;
    uint64_t start_ns = get_time_ns();
    
    while (!atomic_load(&g_orch_is_done)) {
        total_sent += dispatch(tid);
        dispatch_poll(tid);
        spin_wait();
    }
    while (atomic_load(&g_completed_cnt) <
           atomic_load_explicit(&g_task_id, memory_order_acquire)) {
        total_sent += dispatch(tid);
        dispatch_poll(tid);
        spin_wait();
    }
    
    atomic_store(&g_is_done, true);
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[scheduler] task_cnt = %u", g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",(float)(g_completed_cnt * 1000.0 / elapsed_ns));
    dispatch_publish_final_stats(elapsed_ns);

    return NULL;
}
