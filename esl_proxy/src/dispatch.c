/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */

#include "dispatch.h"
#include "log.h"
#include "ring_buf.h"

#include <stdint.h>

extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;
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

static inline void dispatch_init(int tid)
{
    g_ctrl_t[tid].tid = (uint16_t)tid;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] = 0xFFFFFFFFFFFFFFFULL;
            g_ctrl_t[tid].msg_bitmap[i][j] = 0x0;
        }
    }
    set_mix(tid);
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
        // 从二进制最最右边开始向高位看，连续的 0 的个数。
        uint64_t idx = (uint64_t)__builtin_ctzll(*bitmap);
        task_id[(*complete_cnt)++] = task_id_map[idx];
        cnt--;
        *bitmap &= (*bitmap - 1);
        WORKER_LOGF("completed task_id,%u,core,%d", task_id_map[idx], idx);
    }
}

// TODO: add counter for spmd
static inline void set_completed(int tid)
{
    uint16_t task_id[240];
    int complete_cnt = 0;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map1[i]);
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map2[i]);
    }
    for (int i = 0; i < complete_cnt; i++) {
        int slot = task_id[i] & RING_MASK;
        task_state s = atomic_load_explicit(&g_state_buf[slot], memory_order_relaxed);
        s.state = COMPLETED;
        atomic_store_explicit(&g_state_buf[slot], s, memory_order_release);
    }
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_acquire);
}

// TODO: Work Stealing
static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;  // TASK_TYPE_* maps to EXE_TYPE_*
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];
    int free_cnt = __builtin_popcountll(free_bitmap);
    int cnt = free_cnt > (int)ctrl->ready_queue[type].cnt ? (int)ctrl->ready_queue[type].cnt : free_cnt;
    if (cnt <= 0) {
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt);
    
    int sent = 0;
    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);

        uint64_t mask = (uint64_t)0x1 << idx;
        int slot = ctrl->free_bitmap[type][0] & mask == 1 ? 0 : 1;
        // Set executor's tasks and duration
        int core = (int)idx;
        g_executors[exe_type][core].tasks[slot] = task_id;
        g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].idx = slot;  // Point to the slot with the new task
        
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }
        
        ctrl->free_bitmap[type][slot] &= ~mask;
        WORKER_LOGF("task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
        free_bitmap &= ~mask;
    }
    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;
    get_free_exe(tid);
    set_completed(tid);
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
    int tid = (int)(intptr_t)arg;
    dispatch_init(tid);
    WORKER_LOGF("worker %d started, g_completed_cnt %d, g_task_id %d", tid, g_completed_cnt, g_task_id);

    int loop_cnt = 0;
    int total_sent = 0;
    while (g_completed_cnt < g_task_id) {
        total_sent += dispatch(tid);
    }
    WORKER_LOGF("worker %d finished, total_loops=%d total_sent=%d g_task_id=%d", tid, loop_cnt, total_sent, g_task_id);
    return NULL;
}