# Implementation Plan: MPMC Queue

**Branch**: `010-mpmc-queue` | **Date**: 2026-05-26 | **Spec**: [link](spec.md)

**Input**: Lock-free MPMC queue with batch operations using C11 atomics

## Summary

A bounded multi-producer-multi-consumer (MPMC) queue using C11 atomics for lock-free concurrent access. Circular buffer provides O(1) enqueue/dequeue. Supports batch enqueue/dequeue operations for high-throughput task dispatch.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: Standard C library only (`<stdint.h>`, `<stdatomic.h>`, `<stddef.h>`)

**Storage**: Circular buffer in memory with fixed capacity

**Testing**: Unit tests via dependency injection

**Target Platform**: Cross-platform (Linux/macOS)

**Project Type**: Header-only C library for DAG scheduling

**Performance Goals**:
- O(1) enqueue and dequeue
- Support 4+ producers and 4+ consumers concurrently
- Batch operations process 10+ items per call
- Lock-free: no mutexes/spinlocks in hot paths

**Constraints**:
- Bounded queue with configurable capacity
- C11 atomics only (no mutexes in hot path)
- All inputs assumed valid (Trust the Caller)
- Naming per Constitution XI (no redundant prefixes)
- Header-only library design

**Scale/Scope**:
- Queue capacity: 100-10000 (configurable)
- 8 user stories covering basic ops + batch operations

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance Requirement |
|-----------|----------------------|
| Modern C11 | C11 standard only; `_Generic`, atomics, `restrict` |
| Callback-Based Async | Completion via atomic bits; function pointers |
| DAG-Based Task Scheduling | DAG structure; Work-Stealing scheduler |
| Zero-Copy Task Data Flow | Buffer descriptors in Ring Buffers |
| Lock-Free Concurrency | C11 atomics only; no mutexes in hot paths |
| No Blocking in Hot Paths | No sync I/O; async waits with continuation |
| Deterministic Scheduling | Same DAG+inputs → same results |
| Testability | Dependency injection via function pointers |
| Header-Only Library | `static inline` functions for API |
| Trust the Caller | No validation; undefined on invalid input |
| Concise Naming | No redundant prefixes; concise names |

## Project Structure

### Source Code (include/dag/)

```text
include/dag/
├── mpmc_queue.h     # MPMC Queue API (static inline functions)
└── mpmc_queue.c     # Global queue instance definition
```

**Header-Only Enforcement**: All API in headers with `static inline`. Only .c file for global instance definition.

## Phase 1: Design

### mpmc_queue.h - MPMC Queue API

```c
#ifndef DAG_MPMC_QUEUE_H
#define DAG_MPMC_QUEUE_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * MPMC Queue - lock-free multi-producer-multi-consumer queue
 * Uses circular buffer with atomic head/tail indices
 */

typedef struct {
    void *buffer;           /* ring buffer data */
    size_t capacity;        /* max items */
    size_t elem_size;       /* size of each element in bytes */
    _Atomic size_t head;    /* consumer side */
    _Atomic size_t tail;    /* producer side */
} mpmc_queue_t;

/*
 * Queue status codes
 */
typedef enum {
    MPMC_OK       = 0,
    MPMC_FULL     = 1,
    MPMC_EMPTY    = 2,
    MPMC_AGAIN    = 3,      /* non-blocking retry */
} mpmc_status_t;

/*
 * Initialize queue with given capacity and element size
 */
static inline int mpmc_queue_init(mpmc_queue_t *q, size_t capacity, size_t elem_size) {
    q->buffer = malloc(capacity * elem_size);
    q->capacity = capacity;
    q->elem_size = elem_size;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    return 0;
}

/*
 * Calculate circular buffer index
 */
static inline size_t mpmc_idx(mpmc_queue_t *q, size_t pos) {
    return pos % q->capacity;
}

/*
 * Single-item enqueue (non-blocking)
 * Returns: MPMC_OK on success, MPMC_FULL if queue is full
 */
static inline mpmc_status_t mpmc_enqueue(mpmc_queue_t *q, const void *item) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail - head >= q->capacity) {
        return MPMC_FULL;  /* queue is full */
    }

    /* Copy item to buffer */
    char *slot = (char *)q->buffer + mpmc_idx(q, tail) * q->elem_size;
    memcpy(slot, item, q->elem_size);

    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return MPMC_OK;
}

/*
 * Single-item dequeue (non-blocking)
 * Returns: MPMC_OK on success, MPMC_EMPTY if queue is empty
 */
static inline mpmc_status_t mpmc_dequeue(mpmc_queue_t *q, void *item) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (tail == head) {
        return MPMC_EMPTY;  /* queue is empty */
    }

    /* Copy item from buffer */
    char *slot = (char *)q->buffer + mpmc_idx(q, head) * q->elem_size;
    memcpy(item, slot, q->elem_size);

    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return MPMC_OK;
}

/*
 * Batch enqueue - enqueue multiple items
 * Returns: number of items actually enqueued
 */
static inline size_t mpmc_enqueue_batch(mpmc_queue_t *q, const void *items, size_t count) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t avail = q->capacity - (tail - head);

    size_t to_enq = (count < avail) ? count : avail;
    const char *src = (const char *)items;

    for (size_t i = 0; i < to_enq; i++) {
        char *slot = (char *)q->buffer + mpmc_idx(q, tail + i) * q->elem_size;
        memcpy(slot, src + i * q->elem_size, q->elem_size;
    }

    atomic_store_explicit(&q->tail, tail + to_enq, memory_order_release);
    return to_enq;
}

/*
 * Batch dequeue - dequeue multiple items
 * Returns: number of items actually dequeued
 */
static inline size_t mpmc_dequeue_batch(mpmc_queue_t *q, void *items, size_t count) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    size_t avail = tail - head;

    size_t to_deq = (count < avail) ? count : avail;
    char *dst = (char *)items;

    for (size_t i = 0; i < to_deq; i++) {
        char *slot = (char *)q->buffer + mpmc_idx(q, head + i) * q->elem_size;
        memcpy(dst + i * q->elem_size, slot, q->elem_size);
    }

    atomic_store_explicit(&q->head, head + to_deq, memory_order_release);
    return to_deq;
}

/*
 * Get approximate current size (may be stale)
 */
static inline size_t mpmc_size(mpmc_queue_t *q) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    return tail - head;
}

#endif /* DAG_MPMC_QUEUE_H */
```

### Key Design Decisions

1. **Circular Buffer**: Fixed-size ring buffer with atomic head/tail indices
2. **Atomic Indices**: C11 atomics for lock-free concurrent access
3. **Batch Operations**: Support enqueueing/dequeueing multiple items per call
4. **Non-blocking**: All operations return immediately with status
5. **Concise Naming**: Per Constitution XI - `mpmc_*` prefix is sufficient
6. **Trust the Caller**: No validation; caller ensures valid inputs

---

**Status**: Plan complete. Ready for `/speckit-tasks`.