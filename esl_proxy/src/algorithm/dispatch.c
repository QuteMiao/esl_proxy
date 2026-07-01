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
extern struct task_desc g_basic_buf[RING_SIZE];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern int g_subtask_cnt;

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


void dispatch_poll(int tid)
{
    (void)tid;
    if (g_runtime == NULL) {
        return;
    }
    const int n_workers = g_runtime->worker_count;
    const int n_cores = AIC_CNT;

    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            for (int core = 0; core < n_cores; core++) {
                uint16_t task_id = g_executors[exe_type][core].tasks[slot];

                if (task_id == EXEC_SLOT_EMPTY) {
                    continue;
                }
                uint64_t mask = (uint64_t)1 << core;
                if (g_ctrl_t[0].msg_bitmap[exe_type][slot] & mask) {
                    continue;
                }
                const int phys = (int)g_executors[exe_type][core].block_idx[slot];
                if (phys < 0 || phys >= n_workers) {
                    continue;
                }
                const uint32_t reg_task = (uint32_t)g_executors[exe_type][core].base[slot];
                if (reg_task == 0U) {
                    continue;
                }
                const uint64_t reg_addr = dispatch_core_reg_addr(phys);
                if (reg_addr != 0 && platform_reg_task_finished(reg_addr, reg_task)) {
                    dispatch_mark_slot_complete(exe_type, core, slot, reg_addr, reg_task);
                }
            }
        }
    }
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
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_acquire);
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

        /* No cache_civac: g_basic_buf is AICPU-only + the cluster is coherent;
         * dequeuing task_id from ready_queue (acquire) already orders the write. */
        g_executors[exe_type][core].tasks[slot] = task_id;
        // Scale down duration for faster simulation (divide by 10000 to handle large durations)
        uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].duration[slot] = (raw_duration > 10000) ? (raw_duration / 10000) : 1;
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

int dispatch(int tid)
{
    int total_sent = 0;
    atomic_thread_fence(memory_order_acquire);
    get_free_exe(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
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
