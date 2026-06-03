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

static inline Tensor tensor_view(Tensor t, uint32_t row0, uint32_t nrows)
{
    t.offsets[0] += row0;
    t.shapes[0] = nrows;
    return t;
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
extern ctrl_t g_ctrl_t[THREAD_CNT];

static inline void ring_buf_init(void)
{
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_successor_buf[i].next = &g_successor_exp_buf[i % HALF_RING_SIZE];
    }
    for (size_t i = 0; i < HALF_RING_SIZE; i++) {
        g_successor_exp_buf[i].next = NULL;
    }
}

static inline void add_input(uint16_t task_id, Tensor t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    g_basic_buf[task_id & RING_MASK].data[idx] = tensor_base(t);
#else
    g_basic_buf[task_id & RING_MASK].data[idx] = t;
#endif
}

static inline void add_output(uint16_t task_id, Tensor t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    g_basic_buf[task_id & RING_MASK].data[idx] = tensor_base(t);
#else
    g_basic_buf[task_id & RING_MASK].data[idx] = t;
#endif
}

static inline void add_inout(uint16_t task_id, Tensor t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    g_basic_buf[task_id & RING_MASK].data[idx] = tensor_base(t);
#else
    g_basic_buf[task_id & RING_MASK].data[idx] = t;
#endif
}

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

static inline bool batch_succeed(uint16_t cnt, uint16_t task_id[], uint16_t target)
{
    if (target < g_min_uncomplete_task)
        return false;
    int slotIdx = target & RING_MASK;

    task_state expected = atomic_load_explicit(&g_state_buf[slotIdx], memory_order_relaxed);
    expected.state = PENDING;

    task_state desired;
    desired.state = PENDING;
    desired.task_id = expected.task_id;
    desired.successor_cnt = expected.successor_cnt + cnt;
    if (atomic_compare_exchange_strong(&g_state_buf[slotIdx], &expected, desired)) {
        int idx = (int)expected.successor_cnt;
        struct succ_list *ptr = &g_successor_buf[slotIdx];
        for (int i = 0; i < cnt; i++) {
            while (idx >= SUCC_NODE_CNT) {
                idx -= SUCC_NODE_CNT;
                ptr = ptr->next;
            }
            ptr->successor[idx] = task_id[i];
            atomic_fetch_add_explicit(&g_predecessor_buf[task_id[i] & RING_MASK], 1,
                                      memory_order_relaxed);
            idx++;
        }
        return true;
    }
    return false;
}

static inline void batch_submit(uint16_t cnt, uint16_t task_id[])
{
    uint16_t tmp[512];
    for (int i = 0; i < cnt; i++) {
        tmp[i] = (uint16_t)atomic_fetch_sub_explicit(
            &g_predecessor_buf[task_id[i] & RING_MASK], 1, memory_order_relaxed);
    }

    for (int i = 0; i < cnt; i++) {
        if (tmp[i] == 1) {
            uint16_t type = g_basic_buf[task_id[i] & RING_MASK].type;
            queue_t* queue = &g_ctrl_t[g_task_id & (uint16_t)0x1].ready_queue[type];
            lock_q(queue);
            enqueue(queue, task_id[i]);
            unlock_q(queue);
        }
    }
}

static inline void submit(uint16_t task_id)
{
    uint16_t tmp = (uint16_t)atomic_fetch_sub_explicit(
        &g_predecessor_buf[task_id & RING_MASK], 1, memory_order_relaxed);
    if (tmp == 1)
        ready_enqueue(g_basic_buf[task_id & RING_MASK].type,
                      g_basic_buf[task_id & RING_MASK].mode, task_id);
}

static inline bool succeed(uint16_t task_id, uint16_t target)
{
    if (target < g_min_uncomplete_task)
        return false;
    int slotIdx = target & RING_MASK;

    task_state expected = atomic_load_explicit(&g_state_buf[slotIdx], memory_order_relaxed);
    expected.state = PENDING;

    task_state desired;
    desired.state = PENDING;
    desired.task_id = expected.task_id;
    desired.successor_cnt = expected.successor_cnt + 1;
    if (atomic_compare_exchange_strong(&g_state_buf[slotIdx], &expected, desired)) {
        int idx = (int)expected.successor_cnt;
        struct succ_list *ptr = &g_successor_buf[slotIdx];
        while (idx >= SUCC_NODE_CNT) {
            idx -= SUCC_NODE_CNT;
            ptr = ptr->next;
        }
        ptr->successor[idx] = task_id;
        atomic_fetch_add_explicit(&g_predecessor_buf[task_id & RING_MASK], 1,
                                  memory_order_relaxed);
        return true;
    }
    return false;
}

static inline bool try_new_task(uint32_t task_id)
{
    task_state expected;
    expected.state = EMPTY;
    expected.successor_cnt = 0;
    expected.task_id = 0;

    task_state desired;
    desired.state = PENDING;
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
    task_state expected = atomic_load_explicit(&g_state_buf[slotIdx], memory_order_relaxed);
    task_state desired;
    desired.state = COMPLETED;
    desired.task_id = expected.task_id;
    desired.successor_cnt = 0;
    return atomic_compare_exchange_strong(&g_state_buf[slotIdx], &expected, desired);
}

static inline void update_min_uncompleted(void)
{
    uint32_t i = g_min_uncomplete_task & RING_MASK;
    for (; i < (g_task_id & RING_MASK); i++) {
        task_state result = atomic_load_explicit(&g_state_buf[i], memory_order_acquire);
        if (result.state == COMPLETED) {
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