# Shared Memory Contract: Ready Task Notification

**Feature**: 006-cutter | **Contract**: Cutter → Dispatch (ready tasks)

## Overview

The Cutter writes ready-task notifications to a shared memory region consumed by the Dispatch. Ready tasks are those whose all predecessor tasks have completed, making them eligible for dispatch to Executors.

## Memory Region

**Key**: `shmem://cutter-ready/<dispatch_id>` (one region per Dispatch instance; configured at startup)

## Data Format

```c
// Layout (header-only, defined in include/shmem_layout.h)
typedef struct {
    queue_t ready_queue;                // Ring buffer: ready task IDs
    _Atomic uint64_t seq_num;           // Sequence number for consumer wakeup signal
} ready_region_t;
```

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `ready_queue.cnt` | `uint64_t` | Number of ready tasks in queue |
| `ready_queue.head` | `uint64_t` | Head pointer (Cutter write cursor) |
| `ready_queue.tail` | `uint64_t` | Tail pointer (Dispatch read cursor) |
| `ready_queue.tasks[RING_SIZE]` | `uint16_t` | Ring buffer of ready `task_id_t` values |
| `seq_num` | `_Atomic uint64_t` | monotonic sequence number; incremented on each batch write |

### Ring Buffer Behavior

- **Capacity**: `RING_SIZE = 4096`
- **Head/Tail**: Wrap via `& RING_MASK` (bitwise AND with 4095)
- **Write cursor**: `ready_queue.head` — set by Cutter after writing ready task IDs
- **Read cursor**: `ready_queue.tail` — set by Dispatch after reading ready task IDs

## Write Protocol (Cutter)

1. Collect up to `CUTTER_MAX_READY_BATCH` ready task IDs
2. For each `task_id`:
   a. Compute `pos = ready_queue.head & RING_MASK`
   b. Write `task_id` to `ready_queue.tasks[pos]`
   c. Increment `ready_queue.head` with `memory_order_release`
   d. Increment `ready_queue.cnt` with `memory_order_release`
3. Increment `seq_num` with `memory_order_release` to signal new batch to Dispatch

## Read Protocol (Dispatch)

1. Load `seq_num` with `memory_order_acquire` — if unchanged, no new ready tasks
2. Load `ready_queue.cnt` with `memory_order_acquire`
3. If `cnt > 0`:
   a. Compute `start = ready_queue.tail & RING_MASK`
   b. Read `ready_queue.tasks[start]`
   c. Increment `ready_queue.tail` with `memory_order_release`
   d. Decrement `ready_queue.cnt` with `memory_order_release`
4. Process ready tasks

## Synchronization

- **Lock-free**: Ring buffer counters use C11 atomics
- **Sequence number**: Provides a lightweight "is there new work?" check without touching the queue counters
- **Memory ordering**: `release` on Cutter writes, `acquire` on Dispatch reads

## Batch Notification

Per spec requirement: "Given multiple tasks become ready, When the Cutter writes notifications, Then all ready tasks are included in a single batch notification."

Implementation: Cutter collects all currently-ready tasks in a single pass before writing to shared memory. All IDs in the batch are written before the `seq_num` increment that signals Dispatch.

## Constraints

- **Trust the Caller**: Cutter assumes Dispatch reads and clears the queue before overflow occurs
- **No validation**: Cutter does not check if `ready_queue.cnt` is already at capacity before writing
- **Single-writer**: Only the Cutter writes to this region; only the owning Dispatch reads from it
- **Bounded batch**: Max `CUTTER_MAX_READY_BATCH = 256` tasks per notification to avoid overflow
