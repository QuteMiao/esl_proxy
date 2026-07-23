/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#include <stdint.h>
#include <stdio.h>

#include "scheduler/dispatch.h"
#include "common/task.h"
#include "common/log.h"

extern atomic_bool g_is_done;

ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

void init_ctrl_t(void)
{
    for (int tid = 0; tid < DISPATCH_THREAD_CNT; tid++) {
        g_ctrl_t[tid].tid = (uint16_t)tid;

        // Initialize free_bitmap for TASK_TYPE
        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].free_bitmap[i][j] = (uint64_t)((1ULL << AIC_CNT) - 1);
            }
        }
        // set_mix(tid);
        // Initialize msg_bitmap for EXE_TYPE
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].msg_bitmap[i][j] = 0x0;
            }
        }
        
        // Initialize task_id_map
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_CNT; j++) {
                g_ctrl_t[tid].task_id_map1[i][j] = 0;
                g_ctrl_t[tid].task_id_map2[i][j] = 0;
            }
        }
        
        // Initialize queues
        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            memset(&g_ctrl_t[tid].ready_queue[i], 0, sizeof(queue_t));
            atomic_flag_clear_explicit(&g_ctrl_t[tid].ready_queue[i].lock, memory_order_release);
        }
        memset(&g_ctrl_t[tid].completed_queue, 0, sizeof(queue_t));
        atomic_flag_clear_explicit(&g_ctrl_t[tid].completed_queue.lock, memory_order_release);
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
        WORKER_LOGF("completed,task_id,%u,complete_cnt,%d,core,%d,bitmap,%u",task_id_map[idx], *complete_cnt,  idx, *bitmap);
        (*complete_cnt)++;
        cnt--;
        *bitmap &= (*bitmap - 1);
    }
}

static inline void push_2_completed_queue(int tid)
{
    uint16_t task_id[240];
    int complete_cnt = 0;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map1[i]);
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map2[i]);
    }
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
    batch_enqueue(&g_ctrl_t[tid].remote_completed_queue, task_id, (uint16_t)complete_cnt);
}

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
        
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }
        
        // Clear the free bit for this core/slot combination (mark as busy)
        ctrl->free_bitmap[type][slot] &= ~mask;

        // Fake Return
        ctrl->msg_bitmap[type][slot] |= mask;
        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
        free_bitmap &= ~mask;
    }
    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;
    get_free_exe(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

/*
 * Dispatch worker thread entry point Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int total_sent = 0;
    WORKER_LOGF("dispatch,%d,start", tid);
    bool is_done = false;
    while (!is_done) {
        total_sent += dispatch(tid);
        is_done = atomic_load(&g_is_done);
    }
    WORKER_LOGF("dispatch,%d,done", tid);
    return NULL;
}
