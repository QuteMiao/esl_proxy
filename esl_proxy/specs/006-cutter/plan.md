# Implementation Plan: Cutter - Dependency Resolution

**Branch**: `010-mpmc-queue` | **Date**: 2026-05-30 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/006-cutter/spec.md`

## Summary

The Cutter reads completed task results from shared memory (written by Dispatch), reads the task dependency graph from shared memory (written by Orchestrator), resolves dependencies to identify newly ready tasks, and writes ready-task notifications to shared memory for downstream consumption by Dispatch. The Cutter operates as a callback-driven async component within the header-only DAG engine, using C11 atomics and lock-free data structures for all shared-memory access.

## Technical Context

**Language/Version**: C11 (`-std=c11`) with `_Generic`, `<stdatomic.h>`, `restrict` pointers

**Primary Dependencies**: None (header-only library, standard C only)

**Storage**: N/A (shared memory regions pre-established via configuration)

**Testing**: Unit tests with dependency injection via function pointers; integration tests with deterministic DAGs; chaos testing (concurrent read/write)

**Target Platform**: Linux server (DARWIN for dev)

**Project Type**: Header-only C library (no binary dependencies)

**Performance Goals**: <1μs read latency, <10μs dependency resolution, <5μs ready-task notification write, support 10k tasks / 50k edges

**Constraints**: SC-005: clean shutdown <1ms; SC-004: no data corruption under concurrent access; Lock-free only (no mutexes/spinlocks in hot paths)

**Scale/Scope**: 10,000 tasks max, 50,000 edges max per graph; single Cutter instance per pipeline

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

**Rationale**: This is a high-performance async DAG engine in C with Work-Stealing scheduler. Header-only C design ensures maximum inlining and zero linking overhead. Cutter is a read/process/write pipeline stage — it reads completed tasks and a DAG from two shared memory regions, resolves dependencies, and writes ready-task notifications. All three shared-memory operations are lock-free via C11 atomics. No blocking I/O in hot paths.

## Project Structure

### Documentation (this feature)

```text
specs/006-cutter/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output (/speckit-plan command)
├── data-model.md        # Phase 1 output (/speckit-plan command)
├── quickstart.md        # Phase 1 output (/speckit-plan command)
├── contracts/           # Phase 1 output (/speckit-plan command)
│   ├── shmem-completed.md   # Shared memory format: completed task queue (Cutter reads from Dispatch)
│   ├── shmem-dag.md         # Shared memory format: DAG graph (Cutter reads from Orchestrator)
│   └── shmem-ready.md       # Shared memory format: ready-task notification (Cutter writes to Dispatch)
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)

```text
include/
├── cutter.h             # Public API: cutter_init, cutter_read_complete, cutter_resolve, cutter_write_ready, cutter_shutdown
├── cutter_impl.h       # Internal: cutternotif_t, cutternotify_fn, cutter_ctx, dependency resolution logic
└── shmem_layout.h      # Shared memory region descriptors and layout constants

src/
└── (empty — header-only library)

tests/
├── unit/
│   ├── cutter_dep_test.c   # Unit tests: dependency resolution logic
│   └── cutter_shmem_test.c  # Unit tests: shared memory read/write via dependency injection
└── integration/
    └── cutter_pipeline_test.c  # Integration: full pipeline with mock Orchestrator/Dispatch
```

**Structure Decision**: Header-only C library (`include/cutter.h`, `include/cutter_impl.h`, `include/shmem_layout.h`). No `src/` compilation units. Shared memory formats defined in `shmem_layout.h` and documented in `contracts/`. Test files in `tests/` with mock injection.

## Complexity Tracking

> No violations of the Constitution require justification at this time.
