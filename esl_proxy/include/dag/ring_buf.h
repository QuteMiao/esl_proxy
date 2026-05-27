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
#include "task.h"

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)

typedef enum {
    RING_CAT_STATE  = 0,
    RING_CAT_BASIC  = 1,
    RING_CAT_DEP    = 2,
    RING_CAT_RUNTIME = 3,
} ring_cat_t;

/*
 * 4 globally visible ring buffers - direct access via variable name
 * Usage: g_state_buf[ring_idx(task_id)]
 */
extern _Atomic uint32_t g_state_buf[RING_SIZE];
extern _Atomic struct task_desc g_basic_buf[RING_SIZE];
extern _Atomic uint32_t g_dep_buf[RING_SIZE];
extern void *g_runtime_buf[RING_SIZE];

/*
 * Conditional state insert - atomic empty check + insert
 * Returns: 0 on success, -1 on error (non-empty or race)
 */
static inline int state_put_if_empty(uint32_t idx, uint32_t val) {
    _Atomic uint32_t *entry = &g_state_buf[idx];
    uint32_t expected = 0;
    return atomic_compare_exchange_strong_explicit(
        entry, &expected, val,
        memory_order_acquire, memory_order_acquire
    ) ? 0 : -1;
}

/*
 * Get task state from ring buffer by task ID
 */
static inline task_state_t task_state_get(uint32_t task_id) {
    uint32_t idx = task_id & RING_MASK;
    return (task_state_t)atomic_load_explicit(
        &g_state_buf[idx], memory_order_acquire);
}

/*
 * Set task state in ring buffer
 */
static inline void task_state_set(uint32_t task_id, task_state_t state) {
    uint32_t idx = task_id & RING_MASK;
    atomic_store_explicit(&g_state_buf[idx], (uint32_t)state, memory_order_release);
}

/*
 * Compute minimum uncompleted task ID from ring buffer
 * Returns: minimum ID with state != COMPLETED, or UINT32_MAX if all completed
 */
static inline uint32_t ring_min_uncompleted(void) {
    uint32_t min_id = UINT32_MAX;
    for (uint32_t i = 0; i < RING_SIZE; i++) {
        task_state_t state = (task_state_t)atomic_load_explicit(
            &g_state_buf[i], memory_order_acquire);
        if (state != TASK_STATE_COMPLETED && state != TASK_STATE_EMPTY) {
            return i;
        }
    }
    return min_id;
}

#endif /* DAG_RING_BUF_H */