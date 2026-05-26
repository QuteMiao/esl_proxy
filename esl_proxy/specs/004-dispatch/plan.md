# Implementation Plan: Dispatch

**Branch**: `008-task` | **Date**: 2026-05-25 | **Spec**: [link](spec.md)

**Input**: Dispatch component with taskID (2 bytes), successor storage (count + IDs), 3 taskIDs per node.

## Summary

Dispatch distributes tasks via shared memory, manages Executor pools per worker thread (60 CUBE + 60 VECTOR), and implements Work-Stealing for load balancing. Task data uses 16-bit TaskID with compact successor storage.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: Standard C library only

**Storage**: Ring Buffers in shared memory (4096 slots, O(1) access)

**Testing**: Unit tests via dependency injection; chaos testing

**Target Platform**: Cross-platform (Linux/macOS)

**Project Type**: Header-only C library for DAG scheduling

**Performance Goals**:
- Task dispatch latency: <10 μs
- Shared memory access: <1 μs
- Work-Stealing redistribution: <100 μs

**Constraints**:
- No mutexes/spinlocks in hot paths
- All inputs assumed valid (Trust the Caller)
- Ring Buffer size fixed at 4096 (power of 2)
- Header-only only — all implementation in headers, no .c files

**Scale/Scope**:
- Up to 10,000 tasks in DAG
- 60 CUBE + 60 VECTOR Executors per worker thread
- TaskID: 16-bit (2 bytes), max value 65535
- Successor storage: count (1 byte) + up to 3 successor IDs (2 bytes each)
- Node size: 8 bytes (3 × 2B taskIDs + 1B successor count + padding)

## Constitution Check

*GATE: Must pass before Phase 0 research.*

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
| Header-Only Library | All implementation in headers; `static inline` functions |
| Trust the Caller | No validation; undefined on invalid input |

## Project Structure

```text
specs/004-dispatch/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
└── checklists/
    └── requirements.md
```

### Source Code (include/dag/)

```text
include/dag/
├── dag.h                    # Core DAG types
├── dag_task.h              # Task descriptor (2B taskID)
├── dag_task_types.h        # CUBE/VECTOR/MIX
├── dag_org_modes.h        # SINGLE/GROUP/SPMD_SYNC/SPMD_ASYNC
├── dag_ringbuffer.h        # Ring Buffer implementation
├── dag_ringbuffer_ring.h   # 4 ring buffers: basic, successors, io, state
├── dag_executor.h          # Executor with 2-slot cache
├── dag_dispatch.h          # Dispatch with Work-Stealing
└── dag_spmd.h               # SPMD barrier synchronization
```

**Header-Only Enforcement**: All source files are headers (`.h`). No `.c` implementation files.

## Phase 0: Research

1. **TaskID 16-bit Packing**: TaskID fits in 2 bytes, allows 65535 unique task IDs
2. **Successor Compact Storage**: Successor node stores count (1 byte) + list of successor IDs
3. **Node Capacity**: Single slot stores 3 taskIDs (6 bytes) + successor count (1 byte) = 7 bytes minimum

## Phase 1: Design

### Task ID Structure

```c
typedef uint16_t dag_task_id_t;  // 2 bytes, max 65535
```

### Successor Node Structure

```c
struct dag_successor_node {
    uint8_t   successor_cnt;     // 1 byte: number of successors (max 3)
    dag_task_id_t successors[3]; // 3 × 2 bytes = 6 bytes
};
// Total: 7 bytes minimum, aligned to 8 bytes
```

### Compact Storage Layout

| Field | Size | Range |
|-------|------|-------|
| task_id | 2 bytes (uint16_t) | 0 - 65535 |
| successor_cnt | 1 byte (uint8_t) | 0 - 3 |
| successors[] | 3 × 2 bytes | 3 × (0 - 65535) |

### Key Design Decisions

1. **16-bit TaskID**: Compact representation saves memory in Ring Buffers
2. **Embedded Successor Count**: No need for separate lookup
3. **Fixed 3-Successor Max**: Sufficient for most DAG workloads; simplifies implementation
4. **Ring Buffer Indexing**: `task_id & 0x0FFF` provides O(1) access with 4096-slot Ring Buffer

---

**Status**: Plan complete. Ready for `/speckit-tasks`.