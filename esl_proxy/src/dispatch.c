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

extern atomic_int g_task_cnt;
extern atomic_int g_completed_cnt;
ctrl_t g_ctrl_t[THREAD_CNT];

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

static inline void update_exe_state(int tid)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] |= g_ctrl_t[tid].msg_bitmap[i][j];
        }
    }
    set_mix(tid);
}

static inline void get_completed(uint64_t free_bitmap, uint16_t task_id[], int *complete_cnt,
                                 const uint16_t task_id_map[])
{
    int cnt = __builtin_popcountll(free_bitmap);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        task_id[(*complete_cnt)++] = task_id_map[idx];
        cnt--;
        free_bitmap &= free_bitmap - 1;
    }
}

// TODO: add counter for spmd
static inline void set_completed(int tid)
{
    uint16_t task_id[240];
    int complete_cnt = 0;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map1[i]);
        get_completed(g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt,
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
    if (complete_cnt > 0) {
        WORKER_LOGF("dispatch", "tid=%d completed=%d", tid, complete_cnt);
    }
}

// TODO: Work Stealing
static inline void send_task(ctrl_t *ctrl, int type)
{
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];
    int free_cnt = __builtin_popcountll(free_bitmap);
    int cnt = free_cnt > (int)ctrl->ready_cnt[type] ? (int)ctrl->ready_cnt[type] : free_cnt;
    int sent = cnt;
    uint16_t task_id;
    uint16_t head = (uint16_t)ctrl->ready_queue[type].head;
    ctrl->ready_queue[type].head += (uint64_t)cnt;
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        task_id = ctrl->ready_queue[type].tasks[head++];
        if ((ctrl->free_bitmap[type][0] & ((uint64_t)0x1 << idx)) == 0) {
            ctrl->task_id_map1[type][idx] = task_id;
            ctrl->free_bitmap[type][0] &= ~((uint64_t)0x1 << idx);
            ctrl->msg_bitmap[type][0] &= ~((uint64_t)0x1 << idx);
        } else {
            ctrl->task_id_map2[type][idx] = task_id;
            ctrl->free_bitmap[type][1] &= ~((uint64_t)0x1 << idx);
            ctrl->msg_bitmap[type][1] &= ~((uint64_t)0x1 << idx);
        }
        cnt--;
        free_bitmap &= free_bitmap - 1;
    }
    if (sent > 0) {
        WORKER_LOGF("dispatch", "tid=%d type=%d sent=%d", ctrl->tid, type, sent);
    }
}

void dispatch(int tid)
{
    update_exe_state(tid);
    set_completed(tid);
    send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
}

/*
 * Dispatch worker thread entry point
 * Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    dispatch_init(tid);
    WORKER_LOGF("dispatch", "worker %d started", tid);
    while (1) {
        dispatch(tid);
    }
    return NULL;
}
