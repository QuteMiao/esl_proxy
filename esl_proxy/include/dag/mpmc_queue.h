/*
 * mpmc_queue.h - Lock-free Multi-Producer-Multi-Consumer Queue (BlkRing Non-Block)
 *
 * BlkRing implementation: Each slot has atomic state (EMPTY/FILL/COMPLETE).
 * No CAS retry loops - single atomic operation per state transition.
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 *
 * This header provides:
 * - Core MPMC queue (mpmc_queue_t) with BlkRing slot design
 * - 2D ReadyQueue matrix (task_type × org_mode) for task dispatch
 * - Global CompleteQueue for task completion tracking
 */

#ifndef DAG_MPMC_QUEUE_H
#define DAG_MPMC_QUEUE_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* === Forward declarations === */
typedef enum {
    TASK_TYPE_CUBE   = 0,
    TASK_TYPE_VECTOR = 1,
    TASK_TYPE_MIX    = 2,
} task_type_t;

typedef enum {
    ORG_MODE_SINGLE     = 0,
    ORG_MODE_GROUP      = 1,
    ORG_MODE_SPMD_SYNC  = 2,
    ORG_MODE_SPMD_ASYNC = 3,
} org_mode_t;

/* === Status Codes === */
typedef enum {
    MPMC_OK    = 0,
    MPMC_FULL  = 1,
    MPMC_EMPTY = 2,
} mpmc_status_t;

/* === BlkRing Slot States === */
typedef enum {
    SLOT_EMPTY    = 0,
    SLOT_FILL    = 1,
    SLOT_COMPLETE = 2,
} slot_state_t;

/* === BlkRing Slot === */
typedef struct {
    char *data;
    size_t elem_size;
    _Atomic slot_state_t state;
} blkring_slot_t;

/* === Core MPMC Queue (BlkRing) === */
typedef struct {
    blkring_slot_t *slots;
    size_t capacity;
    size_t elem_size;
    _Atomic size_t producer_idx;
    _Atomic size_t consumer_idx;
} mpmc_queue_t;

/* === Queue Dimensions === */
#define TASK_TYPE_COUNT 3
#define ORG_MODE_COUNT  4

/* === 2D ReadyQueue Matrix === */
/* 3 task types × 4 org modes = 12 queues */
extern mpmc_queue_t g_ready_queues[TASK_TYPE_COUNT][ORG_MODE_COUNT];

/* === CompleteQueue === */
extern mpmc_queue_t g_complete_queue;

/* === Inline Function Declarations === */
static inline int mpmc_init(mpmc_queue_t *q, size_t capacity, size_t elem_size);
static inline size_t mpmc_idx(mpmc_queue_t *q, size_t pos);
static inline slot_state_t slot_state_load(blkring_slot_t *slot);
static inline void slot_state_store(blkring_slot_t *slot, slot_state_t state);
static inline mpmc_status_t blkring_produce(mpmc_queue_t *q, const void *item);
static inline mpmc_status_t blkring_consume(mpmc_queue_t *q, void *item);
static inline size_t blkring_produce_batch(mpmc_queue_t *q, const void *items, size_t count);
static inline size_t blkring_consume_batch(mpmc_queue_t *q, void *items, size_t count);
static inline size_t mpmc_size(mpmc_queue_t *q);
static inline mpmc_queue_t *ready_queue_get(task_type_t type, org_mode_t mode);
static inline mpmc_status_t ready_enqueue(task_type_t type, org_mode_t mode, const void *item);
static inline mpmc_status_t ready_dequeue(task_type_t type, org_mode_t mode, void *item);
static inline mpmc_status_t complete_enqueue(const void *item);
static inline mpmc_status_t complete_dequeue(void *item);

/* === Inline Implementations === */

static inline int mpmc_init(mpmc_queue_t *q, size_t capacity, size_t elem_size) {
    q->slots = (blkring_slot_t *)malloc(capacity * sizeof(blkring_slot_t));
    if (!q->slots) return -1;

    q->capacity = capacity;
    q->elem_size = elem_size;

    for (size_t i = 0; i < capacity; i++) {
        q->slots[i].data = (char *)malloc(elem_size);
        q->slots[i].elem_size = elem_size;
        atomic_init(&q->slots[i].state, SLOT_EMPTY);
    }

    atomic_init(&q->producer_idx, 0);
    atomic_init(&q->consumer_idx, 0);
    return 0;
}

static inline size_t mpmc_idx(mpmc_queue_t *q, size_t pos) {
    return pos % q->capacity;
}

static inline slot_state_t slot_state_load(blkring_slot_t *slot) {
    return atomic_load_explicit(&slot->state, memory_order_acquire);
}

static inline void slot_state_store(blkring_slot_t *slot, slot_state_t state) {
    atomic_store_explicit(&slot->state, state, memory_order_release);
}

/*
 * BlkRing produce (enqueue) - single atomic state transition
 * EMPTY -> FILL: Producer writes data then marks slot as FILL
 * Returns MPMC_OK on success, MPMC_FULL if no EMPTY slots available
 */
static inline mpmc_status_t blkring_produce(mpmc_queue_t *q, const void *item) {
    size_t prod_idx = atomic_load_explicit(&q->producer_idx, memory_order_relaxed);

    for (size_t i = 0; i < q->capacity; i++) {
        size_t slot_idx = mpmc_idx(q, prod_idx + i);
        blkring_slot_t *slot = &q->slots[slot_idx];

        /* Try to claim this slot - must be EMPTY */
        slot_state_t expected = SLOT_EMPTY;
        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected, SLOT_FILL,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            /* Slot claimed - copy data */
            memcpy(slot->data, item, q->elem_size);
            /* State is already FILL, no need to store again */

            /* Update producer index */
            atomic_store_explicit(&q->producer_idx, prod_idx + i + 1, memory_order_release);
            return MPMC_OK;
        }
    }

    return MPMC_FULL;
}

/*
 * BlkRing consume (dequeue) - single atomic state transition
 * FILL -> COMPLETE -> EMPTY: Consumer reads data then marks slot as EMPTY
 * Returns MPMC_OK on success, MPMC_EMPTY if no FILL slots available
 */
static inline mpmc_status_t blkring_consume(mpmc_queue_t *q, void *item) {
    size_t cons_idx = atomic_load_explicit(&q->consumer_idx, memory_order_relaxed);

    for (size_t i = 0; i < q->capacity; i++) {
        size_t slot_idx = mpmc_idx(q, cons_idx + i);
        blkring_slot_t *slot = &q->slots[slot_idx];

        /* Try to claim this slot - must be FILL */
        slot_state_t expected = SLOT_FILL;
        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected, SLOT_COMPLETE,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            /* Slot claimed - copy data out */
            memcpy(item, slot->data, q->elem_size);

            /* Mark slot as EMPTY for reuse */
            slot_state_store(slot, SLOT_EMPTY);

            /* Update consumer index */
            atomic_store_explicit(&q->consumer_idx, cons_idx + i + 1, memory_order_release);
            return MPMC_OK;
        }
    }

    return MPMC_EMPTY;
}

static inline size_t blkring_produce_batch(mpmc_queue_t *q, const void *items, size_t count) {
    size_t prod_idx = atomic_load_explicit(&q->producer_idx, memory_order_relaxed);
    size_t enqueued = 0;
    const char *src = (const char *)items;

    for (size_t i = 0; i < count; i++) {
        size_t slot_idx = mpmc_idx(q, prod_idx + enqueued);
        blkring_slot_t *slot = &q->slots[slot_idx];

        slot_state_t expected = SLOT_EMPTY;
        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected, SLOT_FILL,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            memcpy(slot->data, src + enqueued * q->elem_size, q->elem_size);
            enqueued++;
        } else {
            break; /* No more EMPTY slots */
        }
    }

    if (enqueued > 0) {
        atomic_store_explicit(&q->producer_idx, prod_idx + enqueued, memory_order_release);
    }

    return enqueued;
}

static inline size_t blkring_consume_batch(mpmc_queue_t *q, void *items, size_t count) {
    size_t cons_idx = atomic_load_explicit(&q->consumer_idx, memory_order_relaxed);
    size_t dequeued = 0;
    char *dst = (char *)items;

    for (size_t i = 0; i < count; i++) {
        size_t slot_idx = mpmc_idx(q, cons_idx + dequeued);
        blkring_slot_t *slot = &q->slots[slot_idx];

        slot_state_t expected = SLOT_FILL;
        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected, SLOT_COMPLETE,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            memcpy(dst + dequeued * q->elem_size, slot->data, q->elem_size);
            slot_state_store(slot, SLOT_EMPTY);
            dequeued++;
        } else {
            break; /* No more FILL slots */
        }
    }

    if (dequeued > 0) {
        atomic_store_explicit(&q->consumer_idx, cons_idx + dequeued, memory_order_release);
    }

    return dequeued;
}

static inline size_t mpmc_size(mpmc_queue_t *q) {
    size_t prod = atomic_load_explicit(&q->producer_idx, memory_order_relaxed);
    size_t cons = atomic_load_explicit(&q->consumer_idx, memory_order_relaxed);

    size_t diff = prod - cons;
    return (diff <= q->capacity) ? diff : q->capacity;
}

/* === 2D ReadyQueue Matrix Implementation === */

static inline mpmc_queue_t *ready_queue_get(task_type_t type, org_mode_t mode) {
    return &g_ready_queues[type][mode];
}

static inline mpmc_status_t ready_enqueue(task_type_t type, org_mode_t mode, const void *item) {
    return blkring_produce(&g_ready_queues[type][mode], item);
}

static inline mpmc_status_t ready_dequeue(task_type_t type, org_mode_t mode, void *item) {
    return blkring_consume(&g_ready_queues[type][mode], item);
}

/* === CompleteQueue Implementation === */

static inline mpmc_status_t complete_enqueue(const void *item) {
    return blkring_produce(&g_complete_queue, item);
}

static inline mpmc_status_t complete_dequeue(void *item) {
    return blkring_consume(&g_complete_queue, item);
}

#endif /* DAG_MPMC_QUEUE_H */