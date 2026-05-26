# Implementation Plan: Memory Pool

**Branch**: `007-memory-pool` | **Date**: 2026-05-26 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/007-memory-pool/spec.md`

## Summary

A pre-allocated memory pool for DAG task execution supporting: SPSC allocation/deallocation, when2free automatic release based on minimum uncompleted TaskID tracked via Task State Ring Buffer, and a dedicated Manager thread for threshold-based memory reclamation.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: None (standard C library only)

**Storage**: N/A (in-memory pool)

**Testing**: Unity test framework, microbenchmarks for allocation latency

**Target Platform**: Linux/macOS, x86_64

**Project Type**: Header-only C library (in-memory pool)

**Performance Goals**: Allocation/deallocation < 1μs, when2free release < 1μs after threshold

**Constraints**: SPSC mode only (single producer Orchestrator, single consumer Worker), no blocking in Manager thread hot path

**Scale/Scope**: Pool sizes 1MB-1GB, thousands of concurrent allocations

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance Requirement |
|-----------|----------------------|
| Modern C11 | C11 standard (`-std=c11`) only; `_Generic`, atomics, `restrict` pointers required; unsafe practices prohibited |
| Callback-Based Async Architecture | Manager thread uses async polling pattern; when2free callbacks for allocation/release; no blocking |
| DAG-Based Task Scheduling | N/A - memory pool component, not DAG scheduler |
| Zero-Copy Task Data Flow | Buffer descriptors (pointer+size) for zero-copy sharing |
| Lock-Free Concurrency | SPSC mode with C11 atomics; atomic CAS for slot state; no mutexes |
| No Blocking in Hot Paths | Manager thread polls without blocking; atomic operations only |
| Deterministic Scheduling | N/A - memory pool component |
| Testability & Reproducibility | Unit tests for alloc/free/error paths; microbenchmarks |
| Header-Only Library | All implementation in headers with `static inline` |
| Trust the Caller | Caller provides valid addresses and TaskIDs; no validation at pool layer |

**Rationale**: Memory pool is a header-only C11 component using SPSC atomics. Manager thread runs independently with async polling. SPSC constraint simplifies lock-free design.

## Project Structure

### Documentation (this feature)

```text
specs/007-memory-pool/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
└── tasks.md             # Phase 2 output (/speckit-tasks)
```

### Source Code (repository root)

```text
include/dag/
├── mem_pool.h           # Memory pool header (main API)
├── mem_pool.c           # Global pool definitions
├── ring_buf.h           # Ring buffer (Task State Ring Buffer)
└── ring_buf.c           # Ring buffer global defs

src/                    # (if needed for tests)
```

**Structure Decision**: Header-only library under `include/dag/`. Single `mem_pool.h` with `static inline` implementations. Separate `ring_buf.h/c` for Task State Ring Buffer (existing component). Manager thread logic embedded in pool module.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Manager thread polling | Decouples when2free release from task execution | Synchronous release would block task execution paths |
| SPSC-only mode | Simplifies lock-free design to single-producer single-consumer | MPMC would require CAS retry loops |
