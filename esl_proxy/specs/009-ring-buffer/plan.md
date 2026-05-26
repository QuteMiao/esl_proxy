# Implementation Plan: Task Ring Buffers

**Branch**: `009-ring-buffer` | **Date**: 2026-05-26 | **Spec**: [link](spec.md)

**Input**: 4 globally visible Ring Buffers for task data storage. Include `dag/task.h`. Ring Buffer size 4096, O(1) indexing via TaskID & (RING_SIZE - 1). State buffer insert with non-empty check: fails if non-empty, inserts if empty.

## Summary

Four globally visible Ring Buffers provide O(1) storage for task data indexed by TaskID. Includes `dag/task.h` for task descriptor types. All operations are lock-free using C11 atomics. State buffer insert checks empty before writing using atomic CAS.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: Standard C library only (`<stdint.h>`, `<stdatomic.h>`)

**Storage**: 4 Ring Buffers in memory (state, basic info, dependency, runtime)

**Testing**: Unit tests via dependency injection

**Target Platform**: Cross-platform (Linux/macOS)

**Project Type**: Header-only C library for DAG scheduling

**Performance Goals**:
- Ring Buffer access: O(1) via TASKID & (RING_SIZE - 1)
- Compact storage: 16-bit TaskID
- Lock-free operations: C11 atomics only
- Atomic conditional insert: C11 compare-and-swap

**Constraints**:
- No mutexes/spinlocks in hot paths
- All inputs assumed valid (Trust the Caller)
- Ring Buffer size fixed at 4096 (power of 2)
- Header-only library design with .c file for global definitions only
- Naming without `dag` prefix per Constitution XI
- 4 globally visible ring buffers
- State insert: non-empty returns error, empty inserts successfully

**Scale/Scope**:
- Up to 10,000 tasks in DAG
- TASKID: 16-bit (2 bytes)

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
| Header-Only Library | All implementation in headers; `static inline` functions for API; global state in single .c |
| Trust the Caller | No validation; undefined on invalid input |
| Concise Naming | No redundant prefixes; no `dag` prefix; names within context |

## Project Structure

### Source Code (include/dag/)

```text
include/dag/
├── ring_buf.h     # Ring Buffer API (static inline functions)
└── ring_buf.c     # Global ring buffer definitions
```

**Header-Only Enforcement**: All API in headers with `static inline`. Only .c file is for global variable definitions.

**Naming Convention**: File names use `dag_` prefix for header guard only. Type names and function names do not use `dag_` prefix.

## Phase 1: Design

### ring_buf.h - Ring Buffer API

```c
#ifndef DAG_RING_BUF_H
#define DAG_RING_BUF_H

#include <stdint.h>
#include <stdatomic.h>
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
 * O(1) index computation via bitwise AND
 * Requires RING_SIZE to be power of 2
 */
static inline uint32_t ring_idx(uint16_t id) {
    return id & RING_MASK;
}

/*
 * Conditional state insert - atomic empty check + insert
 * Returns: 0 on success, negative on error (non-empty or race)
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
 * 4 globally visible ring buffers - direct access via variable name
 * Usage: g_state_buf[ring_idx(task_id)]
 */
extern _Atomic uint32_t g_state_buf[RING_SIZE];
extern _Atomic task_desc_t g_basic_buf[RING_SIZE];
extern _Atomic dep_base_t g_dep_buf[RING_SIZE];
extern _Atomic void *g_runtime_buf[RING_SIZE];

#endif
```

### ring_buf.c - Global Definitions

```c
#include "ring_buf.h"

_Atomic uint32_t g_state_buf[RING_SIZE];
_Atomic task_desc_t g_basic_buf[RING_SIZE];
_Atomic dep_base_t g_dep_buf[RING_SIZE];
_Atomic void *g_runtime_buf[RING_SIZE];
```

### Key Design Decisions

1. **Single Header API**: All Ring Buffer accessors in `ring_buf.h` as static inline functions
2. **4 Global Buffers**: Defined in `ring_buf.c` as `g_<name>` pattern
3. **Conditional Insert**: Uses `atomic_compare_exchange_strong` to atomically check if empty (expected=0) and insert
4. **Ring Buffer Size 4096**: Power of 2 for efficient bitmask indexing
5. **No dag Prefix on Types**: `ring_idx`, `ring_cat_t`, `state_put_if_empty` — dag_ only for header guard
6. **Includes task.h**: Uses `task_desc_t`, `dep_base_t` from `dag/task.h`

---

**Status**: Plan complete. Ready for `/speckit-tasks`.