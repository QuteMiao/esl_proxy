/*
 * ring_buf.h - Ring Buffer API for task data storage
 *
 * 4 global Ring Buffers for O(1) task data indexed by TaskID.
 * Lock-free operations using C11 atomics.
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#ifndef DAG_RING_BUF_H
#define DAG_RING_BUF_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "conf.h"
#include "log.h"
#include "mpmc_queue.h"
#include "task.h"
#include "queue.h"
#include "dispatch.h"
#include "spin.h"

#include "tensor.h"

extern atomic_int g_task_id;
extern atomic_int g_min_uncomplete_task;
extern atomic_flag g_lock_buf[RING_SIZE];
extern struct task_desc g_basic_buf[RING_SIZE];
extern struct predecessor_list g_predecessors[RING_SIZE];
extern struct ring_buf g_predecessor_ring;
extern struct node_list g_successor_buf[RING_SIZE];
extern struct node_list g_successor_exp_buf[HALF_RING_SIZE];
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

extern int g_subtask_cnt;

struct ring_buf {
    uint16_t size;
    uint16_t* head;
    uint16_t* _Atomic start;
    uint16_t* _Atomic tail;
};

static inline void ring_buf_init(void)
{
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_successor_buf[i].next = NULL;
    }
    g_predecessor_ring.head = malloc(sizeof(uint16_t) * NODE_BUFF_SIZE);
    atomic_store(&g_predecessor_ring.tail, g_predecessor_ring.head);
    atomic_store(&g_predecessor_ring.start, g_predecessor_ring.head);
}

static inline void add_input_ptr(uint16_t task_id, const Tensor *t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
    g_basic_buf[task_id & RING_MASK].data[idx] = t->buffer_addr;
}

static inline void add_output_ptr(uint16_t task_id, const Tensor *t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
    g_basic_buf[task_id & RING_MASK].data[idx] = t->buffer_addr;
}

static inline void add_inout_ptr(uint16_t task_id, const Tensor *t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
    g_basic_buf[task_id & RING_MASK].data[idx] = t->buffer_addr;
}

#define add_input(task_id, t)                                          \
    do {                                                               \
        Tensor _tm_tensor_tmp_ = (t);                                    \
        add_input_ptr((task_id), &_tm_tensor_tmp_);                    \
    } while (0)
#define add_output(task_id, t)                                         \
    do {                                                               \
        Tensor _tm_tensor_tmp_ = (t);                                  \
        add_output_ptr((task_id), &_tm_tensor_tmp_);                   \
    } while (0)
#define add_inout(task_id, t)                                          \
    do {                                                               \
        Tensor _tm_tensor_tmp_ = (t);                                  \
        add_inout_ptr((task_id), &_tm_tensor_tmp_);                    \
    } while (0)

static inline void add_scalar(uint16_t task_id, int64_t t)
{
    int idx = g_basic_buf[task_id & RING_MASK].scalar_cnt++;
    g_basic_buf[task_id & RING_MASK].scalar[idx] = t;
}

static inline void lock(int slotIdx)
{
    while (atomic_flag_test_and_set_explicit(&g_lock_buf[slotIdx], memory_order_acquire)) {
        spin_wait();
    }
}

static inline void unlock(int slotIdx)
{
    atomic_flag_clear_explicit(&g_lock_buf[slotIdx], memory_order_release);
}

static int add_predecessors(uint16_t task_id, uint16_t target[], uint16_t n, uint16_t start)
{
    // int slotIdx = task_id & RING_MASK;
    int slotIdx = task_id;
    struct predecessor_list *ptr = &g_predecessors[slotIdx];
    int cnt = start;
    if (ptr->cnt <= 0)
        ptr->exp = atomic_load(&g_predecessor_ring.tail);
    
    uint16_t min_uncomplete_task = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    for (uint16_t i = 0; i < n; i++)
    {
        if (target[i] < min_uncomplete_task)
            continue;
        WORKER_LOGF("succeed,task_id,%u,predecessor_id,%u,idx,%d", task_id, target[i], cnt);
        uint16_t* idx = atomic_fetch_add(&g_predecessor_ring.tail, 1);
        *idx = target[i];
        cnt++;
    }
    ptr->cnt = cnt;
    return cnt;
}

static inline bool new_task(uint32_t task_id, uint16_t type, uint16_t count, uint16_t duration)
{
    while ((task_id - atomic_load(&g_min_uncomplete_task)) >= RING_SIZE ) {
        MAIN_LOGF("[orchestration] task_id = %u g_min_uncomplete_task = %u", task_id, g_min_uncomplete_task);
        spin_wait();
    }
    if (count > 1)
        g_basic_buf[task_id & RING_MASK].mode = ORG_MODE_SPMD_SYNC;
    g_basic_buf[task_id & RING_MASK].count = count; 
    g_basic_buf[task_id & RING_MASK].duration = duration;
    g_subtask_cnt += count;
    WORKER_LOGF("new,task_id,%u,type,%d,subtask_cnt,%d", task_id, type, count);
}

#endif /* DAG_RING_BUF_H */