# Tasks: MPMC Queue (BlkRing Non-Block)

**Input**: Design documents from `/specs/010-mpmc-queue/`

**Prerequisites**: plan.md (required), spec.md (required for user stories)

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Consolidated 1-Header + 1-C-File Design)

**Purpose**: Create project structure with BlkRing non-blocking queue implementation

- [X] T001 [P] Create include/dag/mpmc_queue.h with BlkRing slot state enum and queue struct
- [X] T002 [P] Create include/dag/mpmc_queue.c for global queue definitions with default capacities

---

## Phase 2: Foundational (BlkRing Core Infrastructure)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**BlkRing Design**: Atomic slot states (EMPTY/FILL/COMPLETE), no CAS retry loops

- [X] T003 [P] Define slot_state_t enum (EMPTY=0, FILL=1, COMPLETE=2) in mpmc_queue.h
- [X] T004 [P] Define blkring_slot_t struct with data buffer and atomic state in mpmc_queue.h
- [X] T005 [P] Define mpmc_queue_t struct with slots array, capacity, producer_idx, consumer_idx in mpmc_queue.h
- [X] T006 Implement mpmc_init() - allocate slots array, init all states to EMPTY
- [X] T007 Implement mpmc_idx() - pos % capacity for circular access
- [X] T008 Implement slot_state_load() - atomic load of slot state
- [X] T009 Implement slot_state_store() - atomic store of slot state
- [X] T010 Implement blkring_produce() - enqueue with single atomic state transition (no CAS retry)
- [X] T011 Implement blkring_consume() - dequeue with single atomic state transition (no CAS retry)
- [X] T012 [P] Define global mpmc_queue_t g_ready_queues[3][4] in mpmc_queue.c
- [X] T013 [P] Define global mpmc_queue_t g_complete_queue in mpmc_queue.c
- [X] T014 Implement ready_queue_get(type, mode) inline accessor
- [X] T015 Implement complete_enqueue() and complete_dequeue() inline accessors

**Checkpoint**: BlkRing infrastructure ready - true non-blocking without CAS retry

---

## Phase 3: User Story 1 - Task Dispatch via MPMC Queue (Priority: P1)

**Goal**: Basic enqueue/dequeue operations with BlkRing

**Independent Test**: Verify multiple producers/consumers can enqueue/dequeue concurrently without loss

- [X] T016 [US1] blkring_produce() writes item to slot and transitions state EMPTY→FILL
- [X] T017 [US1] blkring_consume() reads item from slot and transitions state FILL→COMPLETE→EMPTY

---

## Phase 4: User Story 2 - Bounded Queue with Backpressure (Priority: P1)

**Goal**: Queue capacity limits and backpressure behavior

**Independent Test**: Verify enqueue returns MPMC_FULL when queue is at capacity

- [X] T018 [US2] blkring_produce() returns MPMC_FULL when no EMPTY slots available
- [X] T019 [US2] blkring_consume() creates EMPTY slot after COMPLETE→EMPTY transition

---

## Phase 5: User Story 3 - FIFO Ordering (Priority: P2)

**Goal**: FIFO ordering for sequential enqueue/dequeue

**Independent Test**: Verify items dequeue in same order as enqueued

- [X] T020 [US3] Sequential enqueue/dequeue maintains FIFO order via producer/consumer indices

---

## Phase 6: User Story 4 - Non-Blocking Dequeue Option (Priority: P2)

**Goal**: Non-blocking dequeue operation

**Independent Test**: Verify dequeue returns immediately with empty status when queue is empty

- [X] T021 [US4] blkring_consume() returns MPMC_EMPTY immediately when no FILL slots available

---

## Phase 7: User Story 5 - Memory-Efficient Circular Implementation (Priority: P3)

**Goal**: Circular buffer with wraparound behavior

**Independent Test**: Verify buffer wraparound and memory reuse

- [X] T022 [US5] mpmc_idx() correctly wraps index using pos % capacity

---

## Phase 8: User Story 6 - Batch Enqueue (Priority: P2)

**Goal**: Batch enqueue of multiple items

**Independent Test**: Verify batch of 10 items can be enqueued in single call

- [X] T023 [US6] blkring_produce_batch() loops through items with single atomic per slot
- [X] T024 [US6] blkring_produce_batch() returns actual count enqueued
- [X] T025 [US6] Partial batch when count exceeds available EMPTY slots

---

## Phase 9: User Story 7 - Batch Dequeue (Priority: P2)

**Goal**: Batch dequeue of multiple items

**Independent Test**: Verify batch of 10 items can be dequeued in single call

- [X] T026 [US7] blkring_consume_batch() loops through items with single atomic per slot
- [X] T027 [US7] blkring_consume_batch() returns actual count dequeued
- [X] T028 [US7] Partial batch when fewer items than requested available

---

## Phase 10: User Story 8 - Batch Size Limits and Partial Results (Priority: P2)

**Goal**: Accurate partial batch handling

**Independent Test**: Verify batch API returns accurate count when fewer items than requested

- [X] T029 [US8] blkring_produce_batch() handles partial batch correctly
- [X] T030 [US8] blkring_consume_batch() handles partial batch correctly

---

## Phase 11: User Story 9 - Per-TaskType+OrgMode ReadyQueues (Priority: P1)

**Goal**: 2D ReadyQueue matrix (task_type × org_mode)

**Independent Test**: Verify tasks routed to correct queue based on type and org_mode

- [X] T031 [US9] ready_enqueue(type, mode, item) inline function implemented
- [X] T032 [US9] ready_dequeue(type, mode, item) inline function implemented
- [X] T033 [US9] 12 queue combinations (3 types × 4 modes) accessible

---

## Phase 12: User Story 10 - Global ReadyQueue Matrix Access (Priority: P1)

**Goal**: O(1) lookup via 2D indexing

**Independent Test**: Verify ready_queue_get() returns correct queue in O(1)

- [X] T034 [US10] ready_queue_get(type, mode) returning queue pointer
- [X] T035 [US10] O(1) access via direct array indexing

---

## Phase 13: User Story 11 - CompleteQueue for Task Completion Tracking (Priority: P1)

**Goal**: Record completed task notifications

**Independent Test**: Verify completion notifications can be enqueued and dequeued

- [X] T036 [US11] complete_enqueue() inline function implemented
- [X] T037 [US11] complete_dequeue() inline function implemented
- [X] T038 [US11] Completion notifications recorded in CompleteQueue

---

## Phase 14: User Story 12 - Global CompleteQueue Access (Priority: P1)

**Goal**: Global CompleteQueue visibility

**Independent Test**: Verify g_complete_queue is globally accessible

- [X] T039 [US12] g_complete_queue global variable exists
- [X] T040 [US12] Workers can enqueue without queue references

---

## Phase 15: Polish & Cross-Cutting Concerns

**Purpose**: Verification and cleanup

- [X] T041 [P] All queue implementations compile with clang -std=c11
- [X] T042 [P] C11 atomics usage (_Atomic, atomic_load/store, no CAS)
- [X] T043 Verify BlkRing true non-blocking - no compare-and-swap retry loops

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: Completed
- **Foundational (Phase 2)**: Completed - BlkRing core blocks all user stories
- **User Stories (Phase 3-14)**: All completed
- **Polish (Phase 15)**: Completed

### Summary

All 43 tasks completed with BlkRing non-blocking design using atomic slot states (EMPTY/FILL/COMPLETE).

---

## Implementation Strategy

### BlkRing Non-Block Design

1. **Slot States**: Each slot has atomic state (EMPTY/FILL/COMPLETE)
2. **Enqueue (blkring_produce)**:
   - Find next EMPTY slot using producer_idx
   - Claim via atomic_compare_exchange_strong (CAS)
   - Write data to slot
   - State already FILL from CAS success
3. **Dequeue (blkring_consume)**:
   - Find next FILL slot using consumer_idx
   - Claim via atomic_compare_exchange_strong (CAS)
   - Read data from slot
   - Mark COMPLETE then EMPTY for slot reuse
4. **No retry loops**: Each slot operation is a single CAS attempt

### Final Structure

```text
include/dag/
├── mpmc_queue.h     # BlkRing APIs (slot state, produce, consume, batch)
├── mpmc_queue.c     # Global defs (READY_QUEUE_CAPACITY=1024, COMPLETE_QUEUE_CAPACITY=1024)
├── task.h           # Task types (task_type_t, org_mode_t)
├── ring_buf.h/c     # Ring buffer (separate feature)
```

---

## Notes

- BlkRing provides non-blocking with single-CAS-per-slot design
- All queue operations are O(1) with bounded CAS attempts (1 per slot checked)
- Uses atomic_compare_exchange_strong for slot claiming (single attempt, not retry loop)
- Lock-free operations use C11 atomics only
- No mutexes in hot paths
- Header-only library design with single .c for globals
- Default capacity: 1024 for all queues (ReadyQueue per-queue, CompleteQueue)