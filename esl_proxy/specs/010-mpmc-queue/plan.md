# Implementation Plan: MPMC Queue

**Branch**: `010-mpmc-queue` | **Date**: 2026-05-27 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/010-mpmc-queue/spec.md`

## Summary

Implement a lock-free MPMC (Multi-Producer-Multi-Consumer) queue using BlkRing design with atomic slot states (EMPTY/FILL/COMPLETE). The system provides 12 ReadyQueues (3 task types × 4 org_modes) for task dispatch and 1 global CompleteQueue for task completion tracking. All operations are non-blocking with O(1) enqueue/dequeue using single CAS per slot.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: None (header-only, standard C library only)

**Storage**: N/A (in-memory queues)

**Testing**: Custom unit tests via `/specs/010-mpmc-queue/tasks.md` verification

**Target Platform**: Linux/macOS (C11 atomics)

**Project Type**: Header-only C library (DAG engine component)

**Performance Goals**: O(1) enqueue/dequeue, single CAS per slot, no retry loops

**Constraints**: No mutexes in hot paths, C11 atomics only, no blocking in task dispatch

**Scale/Scope**: 12 ReadyQueues (3 types × 4 modes), 1 CompleteQueue; capacity 1024 each

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance Requirement |
|-----------|----------------------|
| Modern C11 | C11 standard (`-std=c11`) only; `_Generic`, atomics, `restrict` pointers required; unsafe practices prohibited |
| Callback-Based Async Architecture | All APIs async with callbacks; no blocking in hot paths; function pointers + userdata replace C++ lambdas |
| DAG-Based Task Scheduling | All tasks form a DAG; cycles are defects; scheduler must respect dependency order; Work-Stealing required |
| Zero-Copy Task Data Flow | Buffer descriptors (pointer+size), shared memory, in-place transforms; copies require benchmark justification |
| Lock-Free Concurrency | C11 atomics required; mutexes/spinlocks prohibited in hot paths; lock-free SPSC queues for task distribution |
| No Blocking in Hot Paths | No sync I/O or blocking waits; all waits async with continuation enqueue; bounded timeouts required |
| Deterministic Scheduling | Same DAG+inputs produce same results; hidden global state (time, random, env) prohibited |
| Testability & Reproducibility | Dependency injection via function pointers; mock scheduler support required; chaos testing encouraged |
| Header-Only Library | All implementation in headers; `static inline` functions; no binary dependencies |
| Trust the Caller | All inputs assumed correct; no validation, no exception handling, no edge case testing; undefined behavior on invalid input |
| Concise Naming | Variable and function names MUST be concise and avoid unnecessary prefixes |

**Rationale**: This is a high-performance async DAG engine in C with Work-Stealing scheduler. Header-only C design ensures maximum inlining and zero linking overhead.

## Project Structure

### Documentation (this feature)

```text
specs/010-mpmc-queue/
├── plan.md              # This file (/speckit-plan command output)
├── spec.md              # Feature specification (already exists)
├── tasks.md             # Task list (already exists, all completed)
└── contracts/           # Not applicable (internal library, no external APIs)
```

### Source Code (repository root)

```text
include/dag/
├── mpmc_queue.h         # BlkRing queue implementation (EMPTY/FILL/COMPLETE states)
├── mpmc_queue.c         # Global queue definitions (g_ready_queues[3][4], g_complete_queue)
├── task.h               # Task types (task_type_t, org_mode_t) - already exists
└── ring_buf.h           # Ring buffer API - existing, provides task_state_get/set

tests/                   # Unit tests (to be created)
```

**Structure Decision**: Single-header library with one .c file for global definitions. MPMC queue complements ring_buf.h for task dispatch.

## BlkRing Non-Blocking Design

### Slot State Machine

```
EMPTY (0) → FILL (1) → COMPLETE (2) → EMPTY (0)
```

Each slot cycles through states:
1. **EMPTY**: Slot available for producer
2. **FILL**: Data written, waiting for consumer
3. **COMPLETE**: Consumer finished, transitioning back to EMPTY

### Core Operations

**blkring_produce()** - Enqueue:
1. Load producer_idx
2. Find next EMPTY slot via single CAS
3. Write data to slot (state already FILL from CAS success)
4. Advance producer_idx

**blkring_consume()** - Dequeue:
1. Load consumer_idx
2. Find next FILL slot via single CAS
3. Read data from slot
4. Mark COMPLETE then EMPTY for slot reuse
5. Advance consumer_idx

**Key property**: No CAS retry loops - each slot operation is a single CAS attempt.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| None | All Constitution principles satisfied | N/A |

## Phase 0: Research Summary

Based on existing implementation in tasks.md:

### Key Decisions

1. **BlkRing over simple CAS retry**: Atomic slot states (EMPTY/FILL/COMPLETE) provide true non-blocking behavior without retry loops
2. **Global queue matrix**: `g_ready_queues[3][4]` for O(1) 2D indexing by (task_type, org_mode)
3. **Separate CompleteQueue**: `g_complete_queue` for task completion tracking
4. **Default capacity 1024**: Sufficient for workload (100-10000 configured at runtime)

### Alternatives Considered

- Simple CAS with retry loop: Rejected because retry loops add unpredictable latency
- Lock-based queue: Rejected (Constitution VI prohibits locks in hot paths)
- Single queue with tagging: Rejected (per-type+mode queues enable Work-Stealing)

## Phase 1: Design Artifacts

### Data Model (mpmc_queue.h)

```c
/* Slot states - atomic, no mutex */
typedef enum {
    SLOT_EMPTY    = 0,
    SLOT_FILL     = 1,
    SLOT_COMPLETE = 2,
} slot_state_t;

/* Queue slot - state machine transitions */
typedef struct {
    void *data;
    _Atomic slot_state_t state;
} blkring_slot_t;

/* MPMC Queue - circular buffer with atomic slot states */
typedef struct {
    blkring_slot_t *slots;
    uint32_t capacity;
    _Atomic uint32_t producer_idx;  /* Points to next EMPTY slot */
    _Atomic uint32_t consumer_idx; /* Points to next FILL slot */
} mpmc_queue_t;

/* Return codes */
typedef enum {
    MPMC_OK      = 0,
    MPMC_FULL    = -1,
    MPMC_EMPTY   = -2,
} mpmc_ret_t;
```

### Global Queues (mpmc_queue.c)

```c
#define READY_QUEUE_CAPACITY 1024
#define COMPLETE_QUEUE_CAPACITY 1024

/* 3 task types × 4 org_modes = 12 ready queues */
extern mpmc_queue_t g_ready_queues[3][4];
/* Single complete queue */
extern mpmc_queue_t g_complete_queue;

/* Inline accessors */
static inline mpmc_queue_t *ready_queue_get(task_type_t type, org_mode_t mode);
static inline mpmc_ret_t ready_enqueue(task_type_t type, org_mode_t mode, void *item);
static inline mpmc_ret_t ready_dequeue(task_type_t type, org_mode_t mode, void **item);
static inline mpmc_ret_t complete_enqueue(void *item);
static inline mpmc_ret_t complete_dequeue(void **item);
```

### Batch Operations

```c
/* Batch enqueue - returns count of items enqueued */
uint32_t blkring_produce_batch(mpmc_queue_t *q, void **items, uint32_t count);

/* Batch dequeue - returns count of items dequeued */
uint32_t blkring_consume_batch(mpmc_queue_t *q, void **items, uint32_t count);
```

### API Summary

| Function | Description | Return |
|----------|-------------|--------|
| `mpmc_init(q, capacity)` | Initialize queue with capacity | `void` |
| `blkring_produce(q, item)` | Enqueue single item | `MPMC_OK`/`MPMC_FULL` |
| `blkring_consume(q, item)` | Dequeue single item | `MPMC_OK`/`MPMC_EMPTY` |
| `blkring_produce_batch(q, items, count)` | Enqueue multiple items | count enqueued |
| `blkring_consume_batch(q, items, count)` | Dequeue multiple items | count dequeued |
| `ready_enqueue(type, mode, item)` | Enqueue to specific ReadyQueue | `MPMC_OK`/`MPMC_FULL` |
| `ready_dequeue(type, mode, item)` | Dequeue from specific ReadyQueue | `MPMC_OK`/`MPMC_EMPTY` |
| `complete_enqueue(item)` | Enqueue to CompleteQueue | `MPMC_OK`/`MPMC_FULL` |
| `complete_dequeue(item)` | Dequeue from CompleteQueue | `MPMC_OK`/`MPMC_EMPTY` |

## Agent Context Update

See CLAUDE.md section updated via `<!-- SPECKIT START -->` / `<!-- SPECKIT END -->` markers.