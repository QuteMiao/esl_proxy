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

#include "conf.h"
#include "log.h"
#include "mpmc_queue.h"
#include "task.h"
#include "queue.h"
#include "dispatch.h"
#include "spin.h"

typedef enum {
    BFLOAT16 = 2,
    FLOAT32  = 4,
} dtype_t;

#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
typedef struct {
    uint64_t base;
    uint32_t storage[2];
    uint32_t shapes[2];
    uint32_t offsets[2];
    uint32_t strides[2];
    dtype_t  dtype;
} Tensor;

static inline uint64_t tensor_base(Tensor t)
{
    return t.base;
}

static inline Tensor tensor_from_base(uint64_t base)
{
    Tensor t;
    t.base = base;
    t.storage[0] = t.storage[1] = 0;
    t.shapes[0] = t.shapes[1] = 0;
    t.offsets[0] = t.offsets[1] = 0;
    t.strides[0] = t.strides[1] = 0;
    t.dtype = (dtype_t)0;
    return t;
}

static inline Tensor tensor_make_2d(uint64_t base, uint32_t d0, uint32_t d1, dtype_t dtype)
{
    Tensor t;
    t.base = base;
    t.storage[0] = t.shapes[0] = d0;
    t.storage[1] = t.shapes[1] = d1;
    t.offsets[0] = 0;
    t.offsets[1] = 0;
    t.strides[0] = d1;
    t.strides[1] = 1;
    t.dtype = dtype;
    return t;
}

static inline Tensor tensor_view(Tensor t, uint32_t dim, uint32_t off, uint32_t n)
{
    t.offsets[dim] += off;
    t.shapes[dim] = n;
    return t;
}

static inline Tensor tensor_row_view(Tensor t, uint32_t row0, uint32_t nrows)
{
    return tensor_view(t, 0u, row0, nrows);
}

static inline Tensor tensor_chunk_2d(Tensor buf, uint32_t row0, uint32_t nrows,
                                     uint32_t col0, uint32_t ncols, dtype_t dtype)
{
    const uint64_t es = (uint64_t)dtype;
    const uint64_t off =
        (uint64_t)row0 * buf.strides[0] + (uint64_t)col0 * buf.strides[1];
    return tensor_make_2d(tensor_base(buf) + off * es, nrows, ncols, dtype);
}

static inline Tensor tensor_row_chunk(Tensor buf, uint32_t row0, uint32_t nrows)
{
    return tensor_chunk_2d(buf, row0, nrows, 0, buf.storage[1], buf.dtype);
}

static inline Tensor tensor_col_chunk(Tensor buf, uint32_t col0, uint32_t ncols)
{
    return tensor_chunk_2d(buf, 0, buf.storage[0], col0, ncols, buf.dtype);
}
#else
#define Tensor uint64_t
#endif

extern atomic_int g_task_id;
extern uint16_t g_min_uncomplete_task;
extern _Atomic task_state g_state_buf[RING_SIZE];
extern atomic_flag g_lock_buf[RING_SIZE];
extern struct task_desc g_basic_buf[RING_SIZE];
extern _Atomic uint16_t g_predecessor_buf[RING_SIZE];
extern struct succ_list g_successor_buf[RING_SIZE];
extern struct succ_list g_successor_exp_buf[HALF_RING_SIZE];
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

static inline void ring_buf_init(void)
{
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_successor_buf[i].next = &g_successor_exp_buf[i % HALF_RING_SIZE];
    }
    for (size_t i = 0; i < HALF_RING_SIZE; i++) {
        g_successor_exp_buf[i].next = NULL;
    }
}

static inline void add_input_ptr(uint16_t task_id, const Tensor *t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    g_basic_buf[task_id & RING_MASK].data[idx] = t->base;
#else
    g_basic_buf[task_id & RING_MASK].data[idx] = *t;
#endif
}

static inline void add_output_ptr(uint16_t task_id, const Tensor *t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    g_basic_buf[task_id & RING_MASK].data[idx] = t->base;
#else
    g_basic_buf[task_id & RING_MASK].data[idx] = *t;
#endif
}

static inline void add_inout_ptr(uint16_t task_id, const Tensor *t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    g_basic_buf[task_id & RING_MASK].data[idx] = t->base;
#else
    g_basic_buf[task_id & RING_MASK].data[idx] = *t;
#endif
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

static inline void add_duration(uint16_t task_id, int64_t t)
{
    g_basic_buf[task_id & RING_MASK].duration = (uint16_t)t;
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

/* Mirror simpler PTO2FaninBuilder::append_fanin_or_fail: per-producer fanin
 * list may list each consumer at most once (duplicate tensormap hits are no-ops). */
static inline bool fanin_has_consumer(uint16_t producer, uint16_t consumer)
{
    const int slotIdx = producer & RING_MASK;
    task_state st =
        atomic_load_explicit(&g_state_buf[slotIdx], memory_order_relaxed);
    if (st.task_id != producer)
        return false;

    const uint32_t succ_cnt = st.successor_cnt;
    struct succ_list *ptr = &g_successor_buf[slotIdx];
    for (uint32_t k = 0; k < succ_cnt; k++) {
        int idx = (int)k;
        struct succ_list *node = ptr;
        while (idx >= SUCC_NODE_CNT) {
            idx -= SUCC_NODE_CNT;
            node = node->next;
        }
        if (node->successor[idx] == consumer)
            return true;
    }
    return false;
}

static inline bool batch_succeed(uint16_t cnt, uint16_t task_id[], uint16_t target)
{
    if (target < g_min_uncomplete_task)
        return false;

    uint16_t unique[512];
    uint16_t unique_cnt = 0;
    for (int i = 0; i < cnt; i++) {
        const uint16_t consumer = task_id[i];
        if (fanin_has_consumer(target, consumer))
            continue;
        int j = 0;
        for (; j < (int)unique_cnt; j++) {
            if (unique[j] == consumer)
                break;
        }
        if (j < (int)unique_cnt)
            continue;
        unique[unique_cnt++] = consumer;
    }
    if (unique_cnt == 0)
        return true;

    int slotIdx = target & RING_MASK;

    for (;;) {
        task_state expected =
            atomic_load_explicit(&g_state_buf[slotIdx], memory_order_relaxed);
        if (expected.state != TASK_STATUS_CREATING ||
            expected.task_id != (uint16_t)target)
            return false;

        task_state desired = expected;
        desired.successor_cnt = expected.successor_cnt + (uint32_t)unique_cnt;

        if (atomic_compare_exchange_strong_explicit(
                &g_state_buf[slotIdx], &expected, desired, memory_order_release,
                memory_order_relaxed)) {
            int idx = (int)expected.successor_cnt;
            struct succ_list *ptr = &g_successor_buf[slotIdx];
            for (int i = 0; i < (int)unique_cnt; i++) {
                while (idx >= SUCC_NODE_CNT) {
                    idx -= SUCC_NODE_CNT;
                    ptr = ptr->next;
                }
                ptr->successor[idx] = unique[i];
                atomic_fetch_add_explicit(
                    &g_predecessor_buf[unique[i] & RING_MASK], 1,
                    memory_order_seq_cst);
                WORKER_LOGF("succeed task_id,%u,predecessor,%d,target,%u,idx,%d,successor,%d",
                            unique[i], g_predecessor_buf[unique[i] & RING_MASK],
                            target, idx, ptr->successor[idx]);
                idx++;
            }
            return true;
        }
    }
}

static inline void batch_submit(uint16_t cnt, uint16_t task_id[])
{
    uint16_t tmp[512];
    for (int i = 0; i < cnt; i++) {
        tmp[i] = (uint16_t)atomic_fetch_sub_explicit(
            &g_predecessor_buf[task_id[i] & RING_MASK], 1, memory_order_seq_cst);
    }

    for (int i = 0; i < cnt; i++) {
        if (tmp[i] == 1) {
            uint16_t type = g_basic_buf[task_id[i] & RING_MASK].type;
            // int ctrl_id = task_id[i] & (uint16_t)0x1;
            int ctrl_id = 0;
            queue_t* queue = &g_ctrl_t[ctrl_id].ready_queue[type];
            enqueue(queue, task_id[i]);
            WORKER_LOGF("enqueue task_id,%u, type,%u, ctrl_id,%d, cnt,%d", task_id[i], type, ctrl_id, queue->cnt);
        }
    }
}

static inline void submit(uint16_t task_id)
{
#if NO_DEPS
    /* Orchestration-only: clear submit sentinel; sched skipped in main. */
    atomic_fetch_sub_explicit(&g_predecessor_buf[task_id & RING_MASK], 1,
                              memory_order_seq_cst);
#else
    uint16_t tmp = (uint16_t)atomic_fetch_sub_explicit(
        &g_predecessor_buf[task_id & RING_MASK], 1, memory_order_seq_cst);
    if (tmp == 1) {
        uint16_t type = g_basic_buf[task_id & RING_MASK].type;
        int ctrl_id = 0;
        queue_t *queue = &g_ctrl_t[ctrl_id].ready_queue[type];
        WORKER_LOGF("enqueue task_id,%u, type,%u, ctrl_id,%d, cnt,%d", task_id,
                    type, ctrl_id, queue->cnt);
        enqueue(queue, task_id);
    }
#endif
}

static inline bool succeed(uint16_t task_id, uint16_t target)
{
    if (target < g_min_uncomplete_task)
        return false;
    if (fanin_has_consumer(target, task_id))
        return true;
    int slotIdx = target & RING_MASK;

    for (;;) {
        task_state expected =
            atomic_load_explicit(&g_state_buf[slotIdx], memory_order_relaxed);
        if (expected.state != TASK_STATUS_CREATING ||
            expected.task_id != (uint16_t)target)
            return false;

        task_state desired = expected;
        desired.successor_cnt = expected.successor_cnt + 1;

        if (atomic_compare_exchange_strong_explicit(
                &g_state_buf[slotIdx], &expected, desired, memory_order_release,
                memory_order_relaxed)) {
            int idx = (int)expected.successor_cnt;
            struct succ_list *ptr = &g_successor_buf[slotIdx];
            while (idx >= SUCC_NODE_CNT) {
                idx -= SUCC_NODE_CNT;
                ptr = ptr->next;
            }
            ptr->successor[idx] = task_id;
            atomic_fetch_add_explicit(&g_predecessor_buf[task_id & RING_MASK], 1,
                                      memory_order_seq_cst);
            WORKER_LOGF("succeed task_id,%u,predecessor,%d,target,%u", task_id,
                        g_predecessor_buf[task_id & RING_MASK], target);
            return true;
        }
    }
}

static inline bool try_new_task(uint32_t task_id)
{
    // Initialize predecessor count to 0 before marking task as PENDING
    // This ensures tasks with no predecessors can be submitted immediately
    atomic_store_explicit(&g_predecessor_buf[task_id & RING_MASK], 1, memory_order_relaxed);
    
    task_state expected = {0};
    expected.state = TASK_STATUS_EMPTY;
    expected.successor_cnt = 0;
    expected.task_id = 0;

    task_state desired = {0};
    desired.state = TASK_STATUS_CREATING;
    desired.successor_cnt = 0;
    desired.task_id = (uint16_t)task_id;
    return atomic_compare_exchange_strong_explicit(
        &g_state_buf[task_id & RING_MASK], &expected, desired, memory_order_acquire,
        memory_order_acquire);
}

static inline bool set_task_completed(uint32_t task_id, uint32_t state)
{
    (void)state;
    int slotIdx = task_id & RING_MASK;
    task_state expected =
        atomic_load_explicit(&g_state_buf[slotIdx], memory_order_relaxed);
    task_state desired = expected;
    desired.state = TASK_STATUS_COMPLETED;
    desired.successor_cnt = 0;
    return atomic_compare_exchange_strong(&g_state_buf[slotIdx], &expected, desired);
}

static inline void update_min_uncompleted(void)
{
    uint32_t i = g_min_uncomplete_task & RING_MASK;
    for (; i < (g_task_id & RING_MASK); i++) {
        task_state result = atomic_load_explicit(&g_state_buf[i], memory_order_acquire);
        if (result.state == TASK_STATUS_COMPLETED) {
            g_min_uncomplete_task = result.task_id;
        } else {
            break;
        }
    }
}

static inline uint32_t ring_min_uncompleted(void)
{
    update_min_uncompleted();
    return g_min_uncomplete_task;
}

#endif /* DAG_RING_BUF_H */