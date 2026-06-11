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

#include "conf.h"
#include "log.h"
#include "mpmc_queue.h"
#include "task.h"
#include "queue.h"
#include "dispatch.h"
#include "spin.h"

#ifdef USE_TENSORMAP
#include "tensor.h"
#else
#define Tensor uint64_t
#endif

extern atomic_int g_task_id;
extern _Atomic uint16_t g_min_uncomplete_task;
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
#ifdef USE_TENSORMAP
    g_basic_buf[task_id & RING_MASK].data[idx] = t->buffer_addr;
#else
    g_basic_buf[task_id & RING_MASK].data[idx] = *t;
#endif
}

static inline void add_output_ptr(uint16_t task_id, const Tensor *t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
#ifdef USE_TENSORMAP
    g_basic_buf[task_id & RING_MASK].data[idx] = t->buffer_addr;
#else
    g_basic_buf[task_id & RING_MASK].data[idx] = *t;
#endif
}

static inline void add_inout_ptr(uint16_t task_id, const Tensor *t)
{
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
#ifdef USE_TENSORMAP
    g_basic_buf[task_id & RING_MASK].data[idx] = t->buffer_addr;
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
    if (target < atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire))
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
                WORKER_LOGF("succeed, task_id,%u,predecessor_cnt,%d,predecessor_id,%u,idx,%d",
                            unique[i], g_predecessor_buf[unique[i] & RING_MASK],
                            target, idx);
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
            WORKER_LOGF("enqueue,task_id,%u, type,%u, ctrl_id,%d, cnt,%d", task_id[i], type, ctrl_id, queue->cnt);
        }
    }
}

static inline void submit(uint16_t task_id)
{
    uint16_t type = g_basic_buf[task_id & RING_MASK].type;
    WORKER_LOGF("new,task_id,%u, type,%u", task_id, type);
    uint16_t tmp = (uint16_t)atomic_fetch_sub_explicit(
        &g_predecessor_buf[task_id & RING_MASK], 1, memory_order_seq_cst);
    if (tmp == 1) {
        int ctrl_id = 0;
        queue_t *queue = &g_ctrl_t[ctrl_id].ready_queue[type];
        WORKER_LOGF("enqueue,task_id,%u, type,%u, ctrl_id,%d, cnt,%d", task_id,
                    type, ctrl_id, queue->cnt);
        enqueue(queue, task_id);
    }
}

static inline bool succeed(uint16_t task_id, uint16_t target)
{
    if (target < atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire))
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
            WORKER_LOGF("succeed,task_id,%u,predecessor_cnt,%d,predecessor_id,%u,idx,%d", task_id,
                        g_predecessor_buf[task_id & RING_MASK], target,idx);
            return true;
        }
    }
}

static inline bool try_new_task(uint32_t task_id)
{
    // Initialize predecessor count to 0 before marking task as PENDING
    // This ensures tasks with no predecessors can be submitted immediately
    atomic_store_explicit(&g_predecessor_buf[task_id & RING_MASK], 1, memory_order_relaxed);

    task_state expected;
    memset(&expected, 0, sizeof expected);
    expected.state = TASK_STATUS_EMPTY;
    expected.successor_cnt = 0;
    expected.task_id = 0;

    task_state desired;
    memset(&desired, 0, sizeof desired);
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
    task_state desired;
    memset(&desired, 0, sizeof desired);
    desired.state = TASK_STATUS_COMPLETED;
    desired.task_id = expected.task_id;
    desired.successor_cnt = 0;
    return atomic_compare_exchange_strong(&g_state_buf[slotIdx], &expected, desired);
}

static inline void update_min_uncompleted(void)
{
    uint32_t i = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire) & RING_MASK;
    for (; i < (atomic_load_explicit(&g_task_id, memory_order_acquire) & RING_MASK); i++) {
        task_state result = atomic_load_explicit(&g_state_buf[i], memory_order_acquire);
        if (result.state == TASK_STATUS_COMPLETED) {
            atomic_store_explicit(&g_min_uncomplete_task, result.task_id, memory_order_release);
        } else {
            break;
        }
    }
}

static inline uint32_t ring_min_uncompleted(void)
{
    update_min_uncompleted();
    return atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
}

#endif /* DAG_RING_BUF_H */