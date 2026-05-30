# Shared Memory Contract: Completed Task Queue

**Feature**: 006-cutter | **Contract**: Cutter ← Dispatch (completed tasks)

## Overview

The Cutter reads completed task information from a shared memory region written by the Dispatch. This region contains the completed-queue ring buffer and per-task state.

## Memory Region

**Key**: `shmem://cutter-complete/<pipeline_id>` (configured at startup)

## Data Format

```c
// Layout (header-only, defined in include/shmem_layout.h)
typedef struct {
    queue_t queue;                      // Ring buffer: completed task IDs
    _Atomic task_state g_state_buf[];   // Per-task state (RING_SIZE entries)
    uint16_t g_task_id_buf[];           // Task ID by ring position (RING_SIZE)
} completed_region_t;
```

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `queue.cnt` | `uint64_t` | Number of completed tasks in queue |
| `queue.head` | `uint64_t` | Head pointer (consumer avanzza) |
| `queue.tail` | `uint64_t` | Tail pointer (producer avanzza) |
| `queue.tasks[RING_SIZE]` | `uint16_t` | Ring buffer of completed `task_id_t` values |
| `g_state_buf[i].state` | `_Atomic uint16_t` | Task state: EMPTY=0, PENDING=1, COMPLETED=2 |
| `g_state_buf[i].task_id` | `uint16_t` | Task ID for this slot |
| `g_state_buf[i].successor_cnt` | `uint32_t` | Remaining predecessors for this task |
| `g_task_id_buf[i]` | `uint16_t` | Maps ring position `i` to `task_id_t` |

### Ring Buffer Behavior

- **Capacity**: `RING_SIZE = 4096`
- **Head/Tail**: Wrap via `& RING_MASK` (bitwise AND with 4095)
- **Read cursor**: `queue.tail` — Cutter reads from this position
- **Write cursor**: `queue.head` — set by Dispatch after writing completed task

## Read Protocol (Cutter)

1. Load `queue.tail` with `memory_order_acquire`
2. Load `queue.cnt` with `memory_order_acquire`
3. If `cnt == 0`, no completed tasks — return empty
4. Compute `start = queue.tail & RING_MASK`, `end = (queue.tail + cnt) & RING_MASK`
5. Iterate `i` from `start` to `end` (wrapping at RING_SIZE), reading `queue.tasks[i]`
6. For each `task_id`, verify `g_state_buf[task_id].state == COMPLETED` with `memory_order_acquire`
7. Advance logical read cursor (no atomic write — Cutter does not own the queue)

## Synchronization

- **Lock-free**: All fields use C11 atomics or natural word tearing for aligned `uint64_t`
- **No mutexes/spinlocks**: Synchronization via `memory_order_acquire` loads
- **Memory ordering**: `acquire` on all reads to ensure visibility of Dispatch's writes

## Producer Contract (Dispatch)

- Dispatch writes `task_id` to `queue.tasks[head & RING_MASK]`
- Dispatch increments `queue.head`
- Dispatch sets `g_state_buf[task_id].state = COMPLETED` with `memory_order_release`
- All writes use `memory_order_release` to publish to consumers

## Constraints

- **Trust the Caller**: Cutter assumes Dispatch writes valid `task_id` values
- **No validation**: Cutter does not validate `task_id` range (assumes < RING_SIZE)
- **No corruption**: Atomic state transitions prevent torn reads
