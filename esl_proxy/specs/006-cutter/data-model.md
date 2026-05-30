# Data Model: Cutter

**Feature**: 006-cutter | **Branch**: 010-mpmc-queue | **Date**: 2026-05-30

## Entities

### Cutter Context

```c
typedef struct cutter_ctx cutter_ctx_t;

struct cutter_ctx {
    void *shmem_completed;    // Pointer to completed-queue shared memory region
    void *shmem_dag;          // Pointer to DAG shared memory region
    void *shmem_ready;        // Pointer to ready-task notification shared memory region
    cutternotify_fn notify_fn; // Callback invoked when ready tasks are identified
    void *notify_userdata;    // Userdata passed to notify_fn
};
```

**Fields**:
- `shmem_completed`: Handle to the shared memory region containing `completed_queue` (written by Dispatch). Type: pointer to `queue_t`.
- `shmem_dag`: Handle to the shared memory region containing DAG structures (written by Orchestrator). Type: pointer to DAG metadata region.
- `shmem_ready`: Handle to the shared memory region for ready-task notifications (read by Dispatch). Type: pointer to `queue_t`.
- `notify_fn`: Callback function pointer called when ready tasks are resolved. Signature: `void cutternotify_fn(uint16_t *task_ids, uint32_t count, void *userdata)`.
- `notify_userdata`: Opaque pointer passed through to `notify_fn`.

**Lifetime**: Initialized via `cutter_init()`, destroyed via `cutter_shutdown()`. All fields set exactly once at init time.

---

### Ready Task Notification Callback

```c
typedef void (*cutternotify_fn)(uint16_t *task_ids, uint32_t count, void *userdata);
```

**Signature**:
- `task_ids`: Array of `task_id_t` values for tasks now ready (zero-copy pointer into shared memory)
- `count`: Number of task IDs in `task_ids`
- `userdata`: The `notify_userdata` field from `cutter_ctx_t`

**Contract**: Callback is invoked exactly once per resolution cycle. Caller must not modify or retain the `task_ids` pointer after the callback returns.

---

### Shared Memory Region Descriptors

#### Completed Task Queue Region (`shmem://cutter-complete`)

```c
// Layout (exact memory layout defined in shmem_layout.h)
typedef struct {
    queue_t queue;                      // Ring buffer of completed task IDs
    _Atomic task_state g_state_buf[];   // Per-task atomic state (RING_SIZE entries)
    uint16_t g_task_id_buf[];           // Task ID mapping (RING_SIZE entries)
} completed_region_t;
```

- **Written by**: Dispatch
- **Read by**: Cutter
- **Synchronization**: Cutter reads `queue.tail` to determine completed task IDs; `g_state_buf` provides atomic state lookup

#### DAG Graph Region (`shmem://cutter-dag`)

```c
// Layout (exact memory layout defined in shmem_layout.h)
typedef struct {
    uint32_t node_count;                // Total task count in graph
    uint32_t edge_count;                // Total edge count in graph
    struct task_desc nodes[];           // Task descriptors (node_count entries)
    successorList successors[];          // Per-task successor adjacency (node_count entries)
} dag_region_t;
```

- **Written by**: Orchestrator
- **Read by**: Cutter (read-only during resolution)
- **Synchronization**: Orchestrator writes atomically; Cutter reads with acquire semantics

#### Ready Task Notification Region (`shmem://cutter-ready`)

```c
// Layout (exact memory layout defined in shmem_layout.h)
typedef struct {
    queue_t ready_queue;                // Ring buffer of ready task IDs
    _Atomic uint64_t seq_num;           // Sequence number for consumer wakeup
} ready_region_t;
```

- **Written by**: Cutter
- **Read by**: Dispatch
- **Synchronization**: Cutter enqueues with release semantics; Dispatch dequeues with acquire semantics

---

## Relationships

```
Dispatch ──writes──> completed_queue (shmem://cutter-complete) ──reads──> Cutter
Orchestrator ──writes──> dag_region (shmem://cutter-dag) ──reads──> Cutter
Cutter ──writes──> ready_queue (shmem://cutter-ready) ──reads──> Dispatch
```

---

## Validation Rules

- `cutter_init()`: All three shared memory pointers must reference valid, pre-established regions
- `cutter_resolve()`: Called only after at least one completed task is confirmed in `completed_queue`
- `cutter_shutdown()`: All shared memory handles released; no leaks

---

## State Transitions

### Task State (g_state_buf)

```
EMPTY (0) ──Dispatch──> PENDING (1) ──Executor──> COMPLETED (2)
```

- Cutter monitors `COMPLETED` state to trigger dependency resolution
- Cutter does NOT modify `g_state_buf` (read-only)

### Ready Queue Slot State

```
EMPTY (0) ──Cutter writes──> FILL (1) ──Dispatch reads──> EMPTY (0)
```

---

## Constants

```c
#define CUTTER_MAX_READY_BATCH  256   // Max ready tasks batched per notification
#define CUTTER_SHUTDOWN_TIMEOUT_MS 1 // Max shutdown time (ms)
```
