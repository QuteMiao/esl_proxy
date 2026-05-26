/*
 * task.h - Task descriptor and related types for DAG engine
 *
 * Task descriptor contains ONLY description information - NO execution state.
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#ifndef DAG_TASK_H
#define DAG_TASK_H

#include <stdint.h>

/* Ring Buffer constants for TaskID indexing */
#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)

typedef uint16_t task_id_t;

/*
 * O(1) index computation via bitwise AND
 * Requires RING_SIZE to be power of 2
 */
static inline uint32_t ring_idx(task_id_t id) {
    return id & RING_MASK;
}

/*
 * Task type enum - execution model
 */
typedef enum {
    TASK_TYPE_CUBE   = 0,
    TASK_TYPE_VECTOR = 1,
    TASK_TYPE_MIX    = 2,
} task_type_t;

/*
 * Organization mode enum - how task instances are created
 */
typedef enum {
    ORG_MODE_SINGLE     = 0,
    ORG_MODE_GROUP      = 1,
    ORG_MODE_SPMD_SYNC  = 2,
    ORG_MODE_SPMD_ASYNC = 3,
} org_mode_t;

/*
 * Task descriptor - description ONLY, no execution state
 *
 * Contains: id, type, mode, kernel, index, count, prio, data
 * Does NOT contain: state, completion, executor assignment, etc.
 *
 * Owned by Orchestrator. Execution state managed separately by Dispatcher/Executor.
 */
struct task_desc {
    uint16_t    id;        /* Task identifier (2 bytes) */
    task_type_t type;      /* CUBE/VECTOR/MIX */
    org_mode_t  mode;      /* SINGLE/GROUP/SPMD_SYNC/SPMD_ASYNC */
    void       *kernel;    /* KERNEL code pointer */
    uint32_t    index;     /* base INDEX for SPMD */
    uint32_t    count;     /* number of instances */
    uint32_t    prio;      /* scheduling priority */
    void       *data;      /* user context pointer */
};

/*
 * Derive per-instance INDEX for SPMD tasks
 * Instance n gets INDEX = base_index + n
 */
static inline uint32_t task_instance_idx(uint32_t base_index, uint32_t instance) {
    return base_index + instance;
}

#endif /* DAG_TASK_H */