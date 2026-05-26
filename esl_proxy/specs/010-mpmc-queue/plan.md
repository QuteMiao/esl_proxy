# Implementation Plan: MPMC Queue (BlkRing Non-Block)

**Branch**: `010-mpmc-queue` | **Date**: 2026-05-26 | **Spec**: [link](spec.md)

**Input**: Lock-free MPMC queue using blkring (block ring) non-blocking implementation

## Summary

A bounded multi-producer-multi-consumer (MPMC) queue using C11 atomics with blkring (block ring buffer) non-blocking design. The blkring approach uses atomic operations for slot state management instead of traditional head/tail indices, enabling true non-blocking behavior with bounded capacity. 2D ReadyQueue matrix (task_type × org_mode) for task dispatch. Global CompleteQueue for recording task completions. Single header file for all queue types.

**Implementation**: BlkRing non-blocking with atomic slot state tracking

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: Standard C library only (`<stdint.h>`, `<stdatomic.h>`, `<stdbool.h>`, `<stddef.h>`, `<stdlib.h>`, `<string.h>`)

**Storage**: BlkRing circular buffer in memory with fixed capacity

**Testing**: Unit tests via dependency injection

**Target Platform**: Cross-platform (Linux/macOS)

**Project Type**: Header-only C library for DAG scheduling

**Performance Goals**:
- O(1) enqueue and dequeue
- Support 4+ producers and 4+ consumers concurrently
- Batch operations process 10+ items per call
- True non-blocking (no compare-and-swap retry loops)

**Constraints**:
- BlkRing non-blocking design with atomic slot state
- C11 atomics only (no mutexes in hot path)
- All inputs assumed valid (Trust the Caller)
- Naming per Constitution XI (no redundant prefixes)
- Header-only library design with single .c for global definitions
- 1 header file + 1 c file total
- Default capacity: 1024 per queue

**Scale/Scope**:
- Queue capacity: 100-10000 (configurable, default 1024)
- 12 ReadyQueues (3 task types × 4 org modes)
- 1 CompleteQueue
- 12 user stories covering MPMC + ReadyQueue + CompleteQueue

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

**Rationale**: This is a high-performance async DAG engine in C with Work-Stealing scheduler. Header-only C design ensures maximum inlining and zero linking overhead. BlkRing provides non-blocking guarantee without CAS retry loops.

## Project Structure

### Source Code (include/dag/)

```text
include/dag/
├── mpmc_queue.h     # All queue APIs (MPMC, ReadyQueue matrix, CompleteQueue) - BlkRing non-block
└── mpmc_queue.c    # Global queue instance definitions only
```

**BlkRing Non-Block Design**:
- Each slot has atomic state (EMPTY/FILL/COMPLETE)
- Enqueue writes to slot and atomically updates state to FILL
- Dequeue reads slot state and atomically marks COMPLETE then EMPTY
- No compare-and-swap retry loops - single atomic operation per state transition
- Producer and consumer indices track slots for O(1) access

**Default Capacities**:
- ReadyQueue: 1024 per queue (12 queues = ~12KB total buffer)
- CompleteQueue: 1024

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| BlkRing complexity | True non-blocking without CAS retry | Simple atomic head/tail has CAS retry under contention |
