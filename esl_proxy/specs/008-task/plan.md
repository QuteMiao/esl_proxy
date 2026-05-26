# Implementation Plan: Task

**Branch**: `008-task` | **Date**: 2026-05-26 | **Spec**: [link](spec.md)

**Input**: Task feature with task descriptor containing only description information. Task types CUBE/VECTOR/MIX, organization modes Single/Group/SPMD_SYNC/SPMD_ASYNC. Dependency info: successor count, successor nodes, predecessor count. Successor storage: base entry (3 inline) + extension entries via 2-byte next pointer. Runtime info: input/output data address, kernel address. Naming without `dag` prefix per Constitution XI.

## Summary

Task is the fundamental execution unit in the DAG engine. The task descriptor contains only task description information (id, type, mode, kernel, base index, count, prio, data). Dependency information stored separately includes successor count, successor nodes, and predecessor count. Runtime information stored separately includes input/output data addresses and kernel address. Naming avoids `dag` prefix per Constitution XI.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: Standard C library only

**Storage**: 4 separate Ring Buffers (managed by ring buffer component):
- Task State Ring Buffer
- Task Basic Info Ring Buffer
- Task Dependency Ring Buffer
- Task Runtime Info Ring Buffer

**Testing**: Unit tests via dependency injection

**Target Platform**: Cross-platform (Linux/macOS)

**Project Type**: Header-only C library for DAG scheduling

**Performance Goals**:
- Task access: O(1) via TASKID & RING_SIZE
- Compact storage: 16-bit TaskID

**Constraints**:
- Header-only — all implementation in headers, no .c files
- Task descriptor contains ONLY description fields, NO execution state
- Naming without `dag` prefix per Constitution XI

**Scale/Scope**:
- TASKID: 16-bit (2 bytes)
- Successor storage: base entry (3 inline) + extension via 2B next pointer

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
| Concise Naming | No redundant prefixes; no `dag` prefix; names within context |

## Project Structure

### Source Code (include/dag/) - Task Related Only

```text
include/dag/
└── task.h             # Single header: task_desc, task_type_t, org_mode_t, helpers
```

**Header-Only Enforcement**: All source files are headers (`.h`). No `.c` implementation files.

**Naming Convention**: File names use `dag_` prefix for header guard only. Type names and function names do not use `dag_` prefix.

## Phase 1: Design

### Task Descriptor Fields (Description Only)

```c
struct task_desc {
    uint16_t    id;        // 2 bytes - Task identifier
    task_type_t type;      // CUBE/VECTOR/MIX
    org_mode_t  mode;      // SINGLE/GROUP/SPMD_SYNC/SPMD_ASYNC
    void       *kernel;    // KERNEL code pointer
    uint32_t    index;     // base INDEX for SPMD
    uint32_t    count;     // number of instances
    uint32_t    prio;      // scheduling priority
    void       *data;      // user context pointer
};
// NO execution state fields
```

### Task Type Enum

```c
typedef enum {
    TASK_TYPE_CUBE   = 0,
    TASK_TYPE_VECTOR = 1,
    TASK_TYPE_MIX    = 2,
} task_type_t;
```

### Organization Mode Enum

```c
typedef enum {
    ORG_MODE_SINGLE     = 0,
    ORG_MODE_GROUP      = 1,
    ORG_MODE_SPMD_SYNC  = 2,
    ORG_MODE_SPMD_ASYNC = 3,
} org_mode_t;
```

### Dependency Information Structure

```c
// Base entry: 3 inline successors + overflow pointer
struct dep_base {
    uint16_t succ[3];   // 3 inline successor TaskIDs
    uint16_t next;      // 2B pointer to extension entry (0 = none)
    uint16_t pred_cnt;  // Predecessor count
    uint16_t succ_cnt;  // Successor count
};
```

- **Successor count**: Number of direct successor nodes
- **Successor nodes**: List of successor TaskIDs (base entry 3 inline + extension via 2B next pointer)
- **Predecessor count**: Number of direct predecessor nodes

### Runtime Information (Runtime Ring Buffer)

- **Input data address**: Pointer to input data buffer
- **Output data address**: Pointer to output data buffer
- **Kernel address**: Pointer to kernel code to execute

### Key Design Decisions

1. **Single Header**: All task types and structs in one `task.h` header
2. **Task Descriptor Contains ONLY Description**: No state, completion, executor assignment
3. **TaskID 16-bit**: Compact representation for Ring Buffer efficiency
4. **Dependency Info Separate**: Successor count, successor nodes, predecessor count stored in successor Ring Buffer
5. **Runtime Info Separate**: Input/output addresses and kernel address stored in runtime Ring Buffer
6. **No dag Prefix on Types**: `task_desc`, `task_type_t`, `org_mode_t` — dag_ only for header guard

---

**Status**: Plan complete. Ready for `/speckit-tasks`.