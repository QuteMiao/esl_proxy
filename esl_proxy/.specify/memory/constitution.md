<!--
Sync Impact Report:
Version change: 3.1.0 → 3.2.0 (MINOR - new principle added)
Modified principles:
  - None (new principle added only)
Added sections:
  - XI. Concise Naming (new Core Principle - variable/function naming should be concise without unnecessary prefixes)
Templates requiring updates:
  - ⚠ .specify/templates/plan-template.md (Constitution Check table needs new principle entry)
  - ⚠ .specify/templates/spec-template.md (check if any naming references need updates)
  - ⚠ .specify/templates/tasks-template.md (check task categorization)
  - ⚠ .specify/templates/commands/*.md (verify no outdated references)
Removed sections: none
Deferred items: none
-->

# DAG Engine Constitution

## Core Principles

### I. Modern C11

Code MUST use C11 standard (`-std=c11`) exclusively. Pre-C11 constructs are PROHIBITED unless no equivalent exists. Use `_Generic`, atomics (`<stdatomic.h>`), alignment specifiers, and inline functions. Unsafe practices (unbounded `strcpy`, unvalidated pointer arithmetic, integer overflow without checks) are PROHIBITED. All public interfaces MUST have documented contracts.

Rationale: C11 provides memory model guarantees for concurrency and type-generic macros that eliminate entire classes of bugs.

### II. Callback-Based Async Architecture

All public APIs MUST support asynchronous operation. Synchronous blocking calls in hot paths are PROHIBITED. Task execution, I/O, and synchronization must use callback-based async patterns. Function pointers and userdata contexts replace C++ lambdas and std::function. Completion callbacks MUST be invoked exactly once per operation.

Rationale: Async-first ensures the engine can handle millions of concurrent tasks without thread exhaustion. Callbacks are the C idiom for async operation.

### III. DAG-Based Task Scheduling

All tasks MUST be organized as a Directed Acyclic Graph (DAG). Cycles are defects. Task dependencies MUST be explicit and validated at graph construction time. The scheduler MUST respect dependency order and execute independent tasks in parallel. The scheduler MUST use the Work-Stealing algorithm to balance load across worker threads.

Rationale: DAG structure enables maximum parallelism while guaranteeing correctness of dependency ordering. Work-Stealing ensures efficient load balancing.

### IV. Zero-Copy Task Data Flow

Data MUST flow between tasks without copies where possible. Use buffer descriptors (pointer + size pairs), shared memory regions, or in-place transformation. Any copy in a hot path MUST be justified with benchmarks. Data fusion (combining adjacent transformations) is ENCOURAGED.

Rationale: Copies between tasks destroy cache locality and multiply memory bandwidth requirements.

### V. Lock-Free Concurrency

All concurrent DAG operations MUST use C11 atomics or lock-free data structures. Mutexes, spinlocks, or semaphores are PROHIBITED in hot paths. Use atomic compare-and-swap, lock-free SPSC queues for task distribution. Reference counting via atomic operations.

Rationale: Locks introduce non-deterministic latency; lock-free ensures bounded, predictable scheduling latency.

### VI. No Blocking in Hot Paths

Synchronous I/O, blocking waits, or any operation that suspends the current thread indefinitely is PROHIBITED in task execution paths. All wait operations MUST be async with registered callbacks or continuation enqueue. Timeouts MUST be bounded and explicit.

Rationale: Blocking in a parallel scheduler causes thread starvation and defeats the purpose of async execution.

### VII. Deterministic Scheduling

The scheduler MUST produce deterministic results given the same DAG and inputs. Scheduling order for independent tasks need not be deterministic, but task inputs, outputs, and side effects MUST be. Hidden global state (time, random, environment variables) that affects results is PROHIBITED.

Rationale: Determinism is essential for reproducible testing and debugging production issues.

### VIII. Testability & Reproducibility

Every DAG node, edge, and scheduling decision MUST be independently testable. Dependency injection (function pointers + userdata) MUST be used to support mock scheduler policies for unit testing. Integration tests MUST use deterministic DAGs with known expected outputs. Chaos testing (random task delays, failures) is ENCOURAGED.

Rationale: Complex schedulers are impossible to debug without comprehensive, deterministic tests.

### IX. Header-Only Library

This project MUST be a header-only C library. No binary dependencies or separate compilation units required. All implementation MUST be in headers with `static inline` or `static _Thread_local` storage. Any third-party header-only library requires documented justification and project lead approval.

Rationale: Header-only design eliminates linking complexity, enables maximum inlining optimization, and ensures zero-overhead abstractions in C.

### X. Trust the Caller

All inputs are assumed correct. No exception handling, no data validation, no edge case testing at runtime. Callers are responsible for providing valid data. If invalid data is provided, behavior is undefined. This simplifies the codebase and eliminates defensive programming overhead.

Rationale: Validating every input adds overhead and complexity. In a well-designed system, the caller guarantees validity.

### XI. Concise Naming

Variable and function names MUST be concise and avoid unnecessary prefixes. Names should be descriptive within their scope context. Redundant prefixes that duplicate information already conveyed by context or type are PROHIBITED.

Examples of PROHIBITED naming:
- `dag_task_descriptor_t` for a type (use `task_desc` or `task_t`)
- `dag_executor_get_id` for a function in dag_executor module (use `exec_id` or `get_id` if scope is clear)
- `g_global_counter` where global scope is obvious (use `counter`)

Examples of ACCEPTABLE naming:
- `ring_put()` - clear from context that ring is the module
- `task_id` - clear within task context
- `exec` - concise, clear within executor context

Rationale: Concise naming reduces visual noise and improves readability. Unnecessary prefixes add verbosity without information. Module context provides disambiguation.

## Development Standards

### Code Quality

- All code MUST compile with `-Wall -Werror -Wextra -pedantic` on GCC/Clang
- Static analysis (clang-tidy, sparse) MUST pass with zero warnings
- Sanitizers (ASan, MSan, TSan, UBSan) MUST pass in CI
- No undefined behavior; all code must be thread-safe by construction
- Use `restrict` pointers to enable compiler optimizations

### Scheduler Architecture

- The scheduler MUST implement Work-Stealing: idle workers steal tasks from busy workers' deques
- Task deques MUST be lock-free SPSC or use fine-grained locking
- Task submission and theft operations MUST be wait-free or bounded-lock
- Work-stealing steal rate MUST be configurable
- NUMA-aware task placement is ENCOURAGED for large-scale systems

### Performance Validation

- Each PR MUST include microbenchmarks for any scheduler change
- Latency percentiles (p50, p95, p99) MUST be reported for scheduling operations
- Memory allocation in hot paths MUST be pre-allocated or pool-based
- Regression >10% in any measured metric blocks merge

### Dependencies

- HEADER-ONLY LIBRARY: No binary dependencies
- STANDARD C LIBRARY ONLY (no external dependencies)
- `<stdatomic.h>` for C11 atomics
- `<stdint.h>`, `<stdbool.h>`, `<stddef.h>` for standard types
- Third-party libraries PROHIBITED without documented justification and project lead approval

### Naming Conventions

- Use concise names within module context
- Avoid redundant prefixes that duplicate type or scope
- Types should use short suffixed identifiers (e.g., `_t` for typedef structs)
- Functions should use verb-noun patterns within their module scope
- Variables should use noun phrases descriptive of their purpose

## Governance

This constitution supersedes all other development practices. Amendments require:

1. A written proposal with rationale and benchmark evidence
2. Approval from project lead
3. Migration plan for existing code
4. Version increment following semantic versioning (MAJOR for principle removals, MINOR for additions, PATCH for clarifications)

All PRs MUST verify compliance with these principles before merge.

**Version**: 3.2.0 | **Ratified**: 2026-05-22 | **Last Amended**: 2026-05-26