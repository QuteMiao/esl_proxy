<!--
Sync Impact Report:
Version change: 2.0.0 → 2.1.0 (MINOR - technical constraints added)
Modified principles:
  - III. DAG-Based Task Scheduling (added Work-Stealing specification)
  - IX. Minimal Dependencies (expanded to header-only requirement)
Added sections:
  - Scheduler Architecture (new Development Standards section)
Templates requiring updates:
  - ✅ .specify/templates/plan-template.md (Constitution Check table updated)
Removed sections: none
Deferred items: none
-->

# DAG Engine Constitution

## Core Principles

### I. Modern C++17/20

Code MUST use C++17/20 features exclusively. Pre-C++17 constructs are PROHIBITED unless no equivalent exists. Use std::variant, std::optional, std::string_view, ranges, concepts, and coroutines where applicable. Raw new/delete is PROHIBITED; use smart pointers and allocators. The C++17 memory model MUST be used for all concurrent operations.

Rationale: Modern C++ provides zero-overhead abstractions that eliminate entire classes of bugs.

### II. Async-First Architecture

All public APIs MUST be asynchronous by default. Synchronous blocking calls in hot paths are PROHIBITED. Task execution, I/O, and synchronization must be async. Coroutines (C++20) are the preferred async model when available.

Rationale: Async-first ensures the engine can handle millions of concurrent tasks without thread exhaustion.

### III. DAG-Based Task Scheduling

All tasks MUST be organized as a Directed Acyclic Graph (DAG). Cycles are defects. Task dependencies MUST be explicit and validated at graph construction time. The scheduler MUST respect dependency order and execute independent tasks in parallel. The scheduler MUST use the Work-Stealing algorithm to balance load across worker threads.

Rationale: DAG structure enables maximum parallelism while guaranteeing correctness of dependency ordering. Work-Stealing ensures efficient load balancing.

### IV. Zero-Copy Task Data Flow

Data MUST flow between tasks without copies where possible. Use std::span, shared_ptr, or move semantics. Any copy in a hot path MUST be justified with benchmarks. Data fusion (combining adjacent transformations) is ENCOURAGED.

Rationale: Copies between tasks destroy cache locality and multiply memory bandwidth requirements.

### V. Lock-Free Concurrency

All concurrent DAG operations MUST use lock-free data structures. Mutexes, spinlocks, or semaphores are PROHIBITED in hot paths. Use atomic operations, compare-and-swap, and lock-free queues. Thread-safe reference counting via std::atomic_ref or std::shared_ptr atomic operations.

Rationale: Locks introduce non-deterministic latency; lock-free ensures bounded, predictable scheduling latency.

### VI. No Blocking in Hot Paths

Synchronous I/O, blocking waits, or any operation that suspends the current thread indefinitely is PROHIBITED in task execution paths. All wait operations MUST be async and register callbacks or continuations. Timeouts MUST be bounded and explicit.

Rationale: Blocking in a parallel scheduler causes thread starvation and defeats the purpose of async execution.

### VII. Deterministic Scheduling

The scheduler MUST produce deterministic results given the same DAG and inputs. Scheduling order for independent tasks need not be deterministic, but task inputs, outputs, and side effects MUST be. Hidden global state (time, random, environment) that affects results is PROHIBITED.

Rationale: Determinism is essential for reproducible testing and debugging production issues.

### VIII. Testability & Reproducibility

Every DAG node, edge, and scheduling decision MUST be independently testable. Mock scheduler policies MUST be supported for unit testing. Integration tests MUST use deterministic DAGs with known expected outputs. Chaos testing (random task delays, failures) is ENCOURAGED.

Rationale: Complex schedulers are impossible to debug without comprehensive, deterministic tests.

### IX. Header-Only Library

This project MUST be a header-only library. No heavy third-party runtimes are permitted. Any third-party header-only library requires documented justification and project lead approval. Build systems MUST be minimal (CMake, single header, or module-based).

Rationale: Header-only design eliminates linking complexity, reduces binary size, and enables maximum optimization through inlining.

## Development Standards

### Code Quality

- All code MUST compile with `-Wall -Werror -Wextra -Wpedantic` on GCC/Clang
- Static analysis (clang-tidy) MUST pass with zero warnings
- Sanitizers (ASan, MSan, TSan, UBSan) MUST pass in CI
- No undefined behavior; all code must be thread-safe by construction

### Scheduler Architecture

- The scheduler MUST implement Work-Stealing: idle workers steal tasks from busy workers' queues
- Task queues MUST be lock-free or use fine-grained locking (per-queue locks allowed)
- Task submission and theft operations MUST be wait-free or bounded-lock
- Work-stealing stealing rate MUST be configurable
- NUMA-aware task placement is ENCOURAGED for large-scale systems

### Performance Validation

- Each PR MUST include microbenchmarks for any scheduler change
- Latency percentiles (p50, p95, p99) MUST be reported for scheduling operations
- Memory allocation in hot paths MUST be pre-allocated or pool-based
- Regression >10% in any measured metric blocks merge

### Dependencies

- HEADER-ONLY LIBRARY: No binary dependencies
- STANDARD C++ LIBRARY ONLY (no external dependencies)
- std::thread, std::async, std::future for threading
- std::coroutine (C++20) for async/await when available
- Third-party libraries PROHIBITED without documented justification and project lead approval

## Governance

This constitution supersedes all other development practices. Amendments require:

1. A written proposal with rationale and benchmark evidence
2. Approval from project lead
3. Migration plan for existing code
4. Version increment following semantic versioning (MAJOR for principle removals, MINOR for additions, PATCH for clarifications)

All PRs MUST verify compliance with these principles before merge.

**Version**: 2.1.0 | **Ratified**: 2026-05-22 | **Last Amended**: 2026-05-22
