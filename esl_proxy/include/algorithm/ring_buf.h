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
#include "platform.h"
#include "memory_barrier.h"

extern atomic_int g_task_id;
extern atomic_int g_min_uncomplete_task;
extern atomic_flag g_lock_buf[RING_SIZE];
extern struct task_desc g_basic_buf[RING_SIZE];
extern struct predecessor_list g_predecessors[RING_SIZE];
extern _Atomic uint16_t g_predecessor_cnt[RING_SIZE];
extern struct ring_buf g_predecessor_ring;
extern uint16_t predecessor_storage[NODE_BUFF_SIZE];
extern struct node_list g_successor_buf[RING_SIZE];
extern struct node_list g_successor_exp_buf[HALF_RING_SIZE];
extern task_state g_state_buf[RING_SIZE];
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
    g_predecessor_ring.head = predecessor_storage;
    atomic_store(&g_predecessor_ring.tail, g_predecessor_ring.head);
    atomic_store(&g_predecessor_ring.start, g_predecessor_ring.head);
}

/* input/output/inout tensors are all recorded the same way: append the
 * tensor's buffer_addr to data[] and bump tensor_cnt. task_desc keeps a single
 * flat data[]/tensor_cnt with no direction field, so there is one implementation
 * here; the dependency direction (when needed) is tracked by the tensormap
 * layer, not the ring buffer. The distinct add_input/output/inout spellings are
 * kept only for call-site readability. */
static inline void add_tensor_addr(uint16_t task_id, uint64_t addr)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
    g_basic_buf[task_id & RING_MASK].data[idx] = addr;
}

/* Only buffer_addr is recorded, so the macros read that 8-byte field directly
 * instead of copying the whole 128B Tensor. (t).buffer_addr is valid for both
 * lvalue and rvalue arguments, e.g. add_input(id, view(x, ...)). */
#define add_input(task_id, t)  add_tensor_addr((task_id), (t).buffer_addr)
#define add_output(task_id, t) add_tensor_addr((task_id), (t).buffer_addr)
#define add_inout(task_id, t)  add_tensor_addr((task_id), (t).buffer_addr)

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
        ptr->exp = atomic_load_explicit(&g_predecessor_ring.tail, memory_order_relaxed);
    
    uint16_t min_uncomplete_task = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    for (uint16_t i = 0; i < n; i++)
    {
        /* Full mathematical DAG edge (logged BEFORE the runtime prune below).
         * Baseline/DAG tooling needs every declared predecessor edge, including
         * those pruned here because the predecessor already completed. */
        WORKER_LOGF("depall,task_id,%u,predecessor_id,%u", task_id, target[i]);
        if (target[i] < min_uncomplete_task)
            continue;
        /* If the predecessor has already completed by the time this edge is
         * declared, the dependency is already satisfied — don't record it. This
         * shrinks the predecessor list (less cutter work + ring storage) and is
         * safe: g_state_buf[].state is a monotonic COMPLETED flag on the coherent
         * AICPU, so a COMPLETED read is always true; a stale not-completed read
         * just records the edge and the cutter (add_successors) re-checks state. */
        if (g_state_buf[target[i]].state == TASK_STATUS_COMPLETED)
            continue;
        WORKER_LOGF("succeed,task_id,%u,predecessor_id,%u,idx,%d", task_id, target[i], cnt);
        uint16_t* idx = atomic_load_explicit(&g_predecessor_ring.tail, memory_order_relaxed);
        atomic_store_explicit(&g_predecessor_ring.tail, idx + 1, memory_order_relaxed);
        *idx = target[i];
        cnt++;
    }
    ptr->cnt = cnt;
    wmb();

    return cnt;
}

static inline bool new_task(uint32_t task_id, uint16_t type, uint16_t count, uint32_t duration_ns,
                            uint32_t jitter_mask)
{
    while ((task_id - (uint32_t)atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire)) >= RING_SIZE) {
        MAIN_LOGF("[orchestration] task_id = %u g_min_uncomplete_task = %u", task_id,
                  (unsigned)atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire));
        spin_wait();
    }
    if (count > 1)
        g_basic_buf[task_id & RING_MASK].mode = ORG_MODE_SPMD_SYNC;
    g_basic_buf[task_id & RING_MASK].type = (task_type_t)type;
    g_basic_buf[task_id & RING_MASK].count = count;
    g_basic_buf[task_id & RING_MASK].id = (uint16_t)task_id;
    g_basic_buf[task_id & RING_MASK].duration = duration_ns;
    g_basic_buf[task_id & RING_MASK].jitter_mask = jitter_mask;
    g_basic_buf[task_id & RING_MASK].tensor_cnt = 0;
    g_basic_buf[task_id & RING_MASK].scalar_cnt = 0;
    g_subtask_cnt += count;
    if (type == TASK_TYPE_MIX) {
        g_subtask_cnt += (uint32_t)count * 2U;
    }
    WORKER_LOGF("new,task_id,%u,type,%d,subtask_cnt,%d,dur,%u", task_id, type, count,
                (unsigned)duration_ns);
    return true;
}

static inline void advance_task_id(void)
{
    wmb(); /* slot 已在 new_task / add_predecessors / tm_submit 写好 */
    atomic_fetch_add_explicit(&g_task_id, 1, memory_order_release);
}

#endif /* DAG_RING_BUF_H */