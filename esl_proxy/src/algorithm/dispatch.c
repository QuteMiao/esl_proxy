/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#define _GNU_SOURCE

#include "dispatch.h"
#include "runtime.h"
#include "executor.h"
#include "log.h"
#include "memory_barrier.h"
#include "ring_buf.h"
#include "spin.h"
#include "swimlane_aicpu.h"
#include "platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

extern atomic_int g_task_id;
extern atomic_bool g_orch_is_done;
extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern struct task_desc g_basic_buf[RING_SIZE];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

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
        /* 下发任务；被拒绝时回退入队此单任务并结束本轮。 */
        int rc = 0;
        cache_invalidate_range(&g_basic_buf[task_id], sizeof(g_basic_buf[task_id]));
        cache_invalidate_range(&g_predecessors[task_id], sizeof(g_predecessors[task_id]));
        cache_invalidate_range(&g_predecessor_cnt[task_id], sizeof(g_predecessor_cnt[task_id]));

        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }
        ctrl->free_bitmap[type][slot] &= ~mask;   // mark busy

        const int phys = esl_pick_phys_worker(core, exe_type);
        g_executors[exe_type][core].tasks[slot] = task_id;
        g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].idx = slot;
        g_executors[exe_type][core].block_idx[slot] = (uint16_t)phys;

        if (g_runtime != NULL && phys >= g_runtime->worker_count) {
            g_ctrl_t[0].msg_bitmap[exe_type][slot] |= (uint64_t)1 << core;
            g_executors[exe_type][core].base[slot] = 0;
        } else {
            const uint64_t reg_addr = dispatch_core_reg_addr(phys);
            if (reg_addr == 0) {
                rc = -1;
            } else {
                EslPublishHandle pub = esl_prepare_subtask_to_core(g_runtime, phys, task_id, 0);
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
            batch_enqueue(&ctrl->ready_queue[type], &task_id, 1);
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

    /* 本轮读取共享状态前 acquire 调度 counter */
    atomic_thread_fence(memory_order_acquire);
    get_free_exe(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

/*
 * Dispatch worker thread entry point
 * Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    // atomic_store(&g_is_done, true);
    // return NULL;
    int tid = (int)(intptr_t)arg;

    int total_sent = 0;
    uint64_t start_ns = get_time_ns();
    
    while (!atomic_load(&g_orch_is_done)) {
        total_sent += dispatch(tid);
        /* 把硬件 AICore 完成事件拉到 msg_bitmap，供下一轮 get_completed 解码回收 */
        dispatch_poll(tid);
        spin_wait();
    }
    while (atomic_load(&g_completed_cnt) < atomic_load(&g_task_id)) {
        total_sent += dispatch(tid);
        /* 把硬件 AICore 完成事件拉到 msg_bitmap，供下一轮 get_completed 解码回收 */
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
