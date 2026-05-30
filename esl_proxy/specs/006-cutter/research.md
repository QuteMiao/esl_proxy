# Research: Cutter - Dependency Resolution

**Feature**: 006-cutter | **Branch**: 010-mpmc-queue | **Date**: 2026-05-30

## Research Questions

1. **Completed task queue shared memory format** — what structure does Dispatch write and Cutter reads?
2. **DAG graph shared memory format** — what structure does Orchestrator write and Cutter reads?
3. **Ready-task notification shared memory format** — what structure does Cutter write and Dispatch reads?
4. **Synchronization strategy** — how does Cutter coordinate with Dispatch (writer) and Orchestrator (writer) without mutexes/spinlocks?
5. **Dependency resolution algorithm** — incremental (per-task) or batch (periodic scan)?

---

## RQ-1: Completed Task Queue Format

### Decision

The completed task queue is the `completed_queue` field inside `ctrl_t` (dispatch.h), typed as `queue_t`:

```c
typedef struct queue {
    uint64_t cnt;
    uint64_t head;
    uint64_t tail;
    uint16_t tasks[RING_SIZE];
} queue_t;
```

plus the global state buffer:
```c
_Atomic task_state g_state_buf[RING_SIZE];
uint16_t g_task_id_buf[RING_SIZE];
```

where `task_state` is:
```c
typedef struct {
    uint16_t state;       // EMPTY=0, PENDING=1, COMPLETED=2
    uint16_t task_id;
    uint32_t successor_cnt;
} task_state;
```

### Rationale

`queue_t` is a simple ring-buffer with head/tail/cnt counters. `g_state_buf` provides atomic state for each task-slot. `g_task_id_buf` maps ring position to task ID. This is the existing structure used by Dispatch; Cutter reads from the same memory region.

**Cutter's view**: Cutter reads completed `task_id` values from the `completed_queue.tasks` array at positions `[tail, tail+cnt)`. State for each completed task is read from `g_state_buf[task_id]` to verify `state == COMPLETED`.

### Alternatives Considered

- **MPMC queue** (`mpmc_queue.h`): Already implemented but used for ready-queue not completed-queue. Could be reused if we unify the two. Decision: reuse `queue_t`/`g_state_buf` as-is for compatibility with Dispatch.
- **Separate completion slot per Executor**: Dispatch spec FR-015 mentions "1-bit completion signal" from Executor. The MPMC queue's `SLOT_COMPLETE` state maps to this. However, the global `g_state_buf` provides richer completion info (successor count tracking).

### Status: RESOLVED — uses existing `queue_t` + `g_state_buf` format

---

## RQ-2: DAG Graph Format

### Decision

The DAG is stored as:
- `successorList` adjacency list per task node (from `task.h`):
  ```c
  struct successorList {
      uint16_t successor[3];
      struct successorList* next;
  };
  ```
- Global task descriptors array `struct task_desc g_tasks[RING_SIZE]` (implicit in design; DAG metadata lives in task descriptors)
- `g_state_buf` tracks per-task state and successor count

### Rationale

The `successorList` with embedded 3-successor nodes and linked-list overflow is the existing structure. The Orchestrator constructs the DAG by building successor lists; the Cutter reads them to traverse the graph backward (from completed task to its dependents).

**Cutter's algorithm**: When a task `T` completes, Cutter walks `T`'s successor list. For each successor `S`, Cutter decrements `g_state_buf[S].successor_cnt`. When `successor_cnt` reaches 0, `S` is ready.

### Alternatives Considered

- **CSR (Compressed Sparse Row) format**: More efficient for large static graphs but requires additional index array. Decision: stick with `successorList` for incremental updates (decrement on completion is O(1) per edge).
- **Reverse adjacency (predecessor list)**: Would enable O(1) ready-task check without decrement, but requires Orchestrator to build reverse edges on graph construction. Decision: use successor-list + decrement (standard DAG resolution).

### Status: RESOLVED — uses existing `successorList` + `g_state_buf` format

---

## RQ-3: Ready-Task Notification Format

### Decision

**NEEDS CLARIFICATION** — The ready-task notification format that Cutter writes for downstream Dispatch consumption is not yet defined in the spec or codebase. Key questions:

- Is it a single `queue_t` ready-queue (mirroring `completed_queue`)?
- Is it per-DDispatch notification via per-Dispatch shared memory region?
- Does the notification include full `task_desc` or just `task_id`?

The Dispatch spec (FR-013) says: "The Dispatch MUST read ready tasks from both Orchestrator and Cutter shared memory regions without duplication."

### Open Question

What is the exact shared memory region and format for Cutter→Dispatch ready-task notification?

**Proposed approach** (pending confirmation): Mirror the `queue_t` format used by Orchestrator's ready-queue, placing a `queue_t ready_queue` in a shared-memory region `shmem://cutter-ready/<dispatch_id>`. Cutter batch-enqueues `task_id` values; Dispatch batch-dequeues.

### Status: NEEDS CLARIFICATION — tracked as open item

---

## RQ-4: Synchronization Strategy

### Decision

**Lock-free via C11 atomics** — no mutexes/spinlocks in hot paths.

Synchronization uses two mechanisms:

1. **Atomic ring-buffer counters** (`queue.cnt`, `queue.head`, `queue.tail`): `uint64_t` fields accessed with `memory_order_acquire` on read, `memory_order_release` on write. Not fully lock-free for multi-producer scenarios but sufficient for single-Cutter/single-Dispatch.

2. **Atomic task state machine** (`g_state_buf`): `_Atomic task_state` with transitions EMPTY→PENDING→COMPLETED. CAS-based updates for state transitions.

3. **MBPMC queue slot states** (if MPMC queue is used for ready notification): `SLOT_EMPTY`/`SLOT_FILL`/`SLOT_COMPLETE` state machine with single-CAS transitions — wait-free for both producer and consumer.

### Rationale

The Constitution prohibits mutexes/spinlocks in hot paths. C11 atomics provide the necessary synchronization primitives. The existing codebase uses `_Atomic` on `task_state` and `slot_state_t` — Cutter follows the same pattern.

### Alternatives Considered

- **Memory barriers only**: Could use `memory_order_seq_cst` for total ordering but adds unnecessary synchronization overhead. Decision: use `acquire`/`release` semantics where possible.
- **Sequential consistency**: Simpler reasoning but slower. Decision: per-access bar使用权 semantics for performance.

### Status: RESOLVED — C11 atomics with acquire/release semantics

---

## RQ-5: Dependency Resolution Algorithm

### Decision

**Incremental resolution per completed task** — on each completed task `T`, walk `T`'s successor list, atomically decrement each successor's remaining-predecessor count, and enqueue any successor whose count reaches 0.

```c
// Pseudocode
void cutter_on_task_complete(uint16_t task_id) {
    successorList *succ = g_successor_buf[task_id];  // TO BE CLARIFIED: where is successor list stored?
    while (succ) {
        for (int i = 0; i < 3 && succ->successor[i] != 0; i++) {
            uint16_t sid = succ->successor[i];
            // Atomically decrement remaining predecessor count
            uint32_t prev = atomic_fetch_sub(&g_state_buf[sid].successor_cnt, 1);
            if (prev == 1) {  // Just reached 0 — task is now ready
                enqueue_ready(sid);
            }
        }
        succ = succ->next;
    }
}
```

**Batch mode** (for graph restructuring): Periodic full scan of `g_state_buf` to find tasks with `successor_cnt == 0` not already in ready queue. Used when recovering from inconsistencies or processing graph updates.

### Rationale

Incremental resolution is O(edges_on_completed_task) — typically small. For 50k edges, worst case processes all edges only when all tasks complete, which is acceptable. No need to scan the full graph on each completion.

### Status: RESOLVED — incremental with batch fallback

---

## Open Clarifications

| # | Item | Question | Impact |
|---|------|----------|--------|
| 1 | Ready-task notification format | What shared memory region and data format does Cutter use to notify Dispatch of ready tasks? | Cutter→Dispatch contract |
| 2 | Successor list storage | Where does the Cutter read successor lists from? Are they in shared memory written by Orchestrator, or in a private structure? | DAG traversal |
| 3 | Shared memory region configuration | How are shared memory region addresses communicated between Orchestrator/Dispatch/Cutter? | System configuration |

---

## Summary

| Question | Decision | Status |
|----------|----------|--------|
| RQ-1: Completed task format | `queue_t` + `g_state_buf` | RESOLVED |
| RQ-2: DAG graph format | `successorList` + `task_desc` | RESOLVED (successor storage location needs clarification) |
| RQ-3: Ready-task notification | **NEEDS CLARIFICATION** — queue format TBD | OPEN |
| RQ-4: Synchronization | C11 atomics, acquire/release | RESOLVED |
| RQ-5: Dep resolution algorithm | Incremental per-task + batch fallback | RESOLVED |
