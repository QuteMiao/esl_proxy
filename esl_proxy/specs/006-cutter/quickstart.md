# Quickstart: Cutter

**Feature**: 006-cutter | **Date**: 2026-05-30

## Overview

The Cutter is a dependency resolution stage in the DAG engine pipeline. It:
1. Reads completed task results from shared memory (Dispatch source)
2. Reads the task dependency graph from shared memory (Orchestrator source)
3. Resolves which tasks are now ready (all predecessors complete)
4. Writes ready-task notifications to shared memory (Dispatch sink)

## Files

```
include/
├── cutter.h        # Public API
└── cutter_impl.h   # Internal implementation

include/shmem_layout.h  # Shared memory region layouts

specs/006-cutter/
├── contracts/
│   ├── shmem-completed.md  # Cutter ← Dispatch (completed tasks)
│   ├── shmem-dag.md        # Cutter ← Orchestrator (DAG graph)
│   └── shmem-ready.md      # Cutter → Dispatch (ready tasks)
└── data-model.md   # Data structures and entities
```

## 30-Second Summary

```
Cutter reads completed tasks from shmem
       ↓
Cutter walks successor list of each completed task
       ↓
For each successor: decrement predecessor count atomically
       ↓
When predecessor count → 0: task is ready → enqueue to ready_queue
       ↓
Cutter increments seq_num to signal Dispatch
```

## Public API

### Initialize

```c
#include <cutter.h>

cutter_cfg_t cfg = {
    .shmem_completed = /* pointer to completed-queue region */,
    .shmem_dag       = /* pointer to DAG region */,
    .shmem_ready     = /* pointer to ready-queue region */,
    .notify_fn       = my_ready_callback,
    .notify_userdata = my_context,
};

cutter_ctx_t *ctx = cutter_init(&cfg);
```

### Process Completed Tasks

```c
// Called by the pipeline when the Dispatch signals completed tasks available
// (e.g., via seq_num change on completed-queue region)
void cutter_on_complete_avail(void) {
    uint16_t completed[RING_SIZE];
    uint32_t n = cutter_read_complete(ctx, completed, RING_SIZE);

    for (uint32_t i = 0; i < n; i++) {
        cutter_resolve(ctx, completed[i]);  // Incremental dependency resolution
    }

    cutter_write_ready(ctx);  // Write all collected ready tasks to shmem
}
```

### Shutdown

```c
cutter_shutdown(ctx);  // Releases shared memory handles, clean shutdown < 1ms
```

## Shared Memory Regions

| Region | Direction | Source | Sink |
|--------|-----------|--------|------|
| `shmem://cutter-complete/<id>` | Cutter reads | Dispatch | completed queue |
| `shmem://cutter-dag/<id>` | Cutter reads | Orchestrator | DAG graph |
| `shmem://cutter-ready/<id>` | Cutter writes | Dispatch | ready queue |

## Key Types

```c
typedef void (*cutternotify_fn)(uint16_t *task_ids, uint32_t count, void *userdata);

typedef struct {
    void *shmem_completed;
    void *shmem_dag;
    void *shmem_ready;
    cutternotify_fn notify_fn;
    void *notify_userdata;
} cutter_cfg_t;

typedef struct cutter_ctx cutter_ctx_t;
```

## Dependency Resolution Algorithm

**Incremental**: When task `T` completes:
1. Walk `T`'s successor list (from DAG shared memory)
2. For each successor `S`: atomically decrement `g_state_buf[S].successor_cnt`
3. If decremented value is 0: enqueue `S` to ready batch
4. After processing all completions: write ready batch to `shmem://cutter-ready` region

**Bounded**: O(edges_on_completed_task) per completion. Worst case O(edges) when processing a large batch.

## Constitution Compliance

| Principle | How Cutter Complies |
|-----------|---------------------|
| Modern C11 | `-std=c11`, `_Atomic`, `restrict` pointers throughout |
| Callback-Based Async | `cutternotify_fn` function pointer + userdata for ready notification |
| Lock-Free Concurrency | All shared memory via C11 atomics, no mutexes/spinlocks |
| No Blocking in Hot Paths | No sync I/O; resolution is pure in-memory computation |
| Header-Only | All implementation in headers (`cutter.h`, `cutter_impl.h`, `shmem_layout.h`) |
| Trust the Caller | No input validation; assumes valid shared memory regions and task IDs |
