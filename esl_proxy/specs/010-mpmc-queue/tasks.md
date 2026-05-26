# Tasks: MPMC Queue

**Input**: Design documents from `/specs/010-mpmc-queue/`

**Prerequisites**: plan.md (required), spec.md (required for user stories)

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Project Initialization)

**Purpose**: Create project structure and basic configuration

- [X] T001 [P] Create include/dag/mpmc_queue.h with header guard DAG_MPMC_QUEUE_H, mpmc_status_t enum, mpmc_queue_t struct
- [X] T002 [P] Create include/dag/mpmc_queue.c for global queue definitions
- [X] T003 [P] Create include/dag/ready_queue.h with 2D queue matrix API
- [X] T004 [P] Create include/dag/ready_queue.c for global ReadyQueue matrix
- [X] T005 [P] Create include/dag/complete_queue.h with global CompleteQueue API
- [X] T006 [P] Create include/dag/complete_queue.c for global CompleteQueue definition

---

## Phase 2: Foundational (Core MPMC Queue Infrastructure)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

- [X] T007 Implement mpmc_init() in mpmc_queue.h - initialize queue with capacity and elem_size
- [X] T008 [P] Implement mpmc_idx() function using pos % capacity
- [X] T009 Implement mpmc_enqueue() with atomic head/tail, circular buffer copy
- [X] T010 Implement mpmc_dequeue() with atomic head/tail, circular buffer copy
- [X] T011 Implement mpmc_size() returning tail - head approximate size
- [X] T012 [P] Define global mpmc_queue_t g_ready_queues[3][4] in ready_queue.c
- [X] T013 [P] Define global mpmc_queue_t g_complete_queue in complete_queue.c
- [X] T014 Implement ready_queue_get(type, mode) inline accessor
- [X] T015 Implement complete_enqueue() and complete_dequeue() inline accessors

**Checkpoint**: Core MPMC queue infrastructure ready

---

## Phase 3: User Story 1 - Task Dispatch via MPMC Queue (Priority: P1)

**Goal**: Basic enqueue/dequeue operations

**Independent Test**: Verify multiple producers/consumers can enqueue/dequeue concurrently without loss

- [X] T016 [US1] mpmc_enqueue() implementation verified - stores item in circular buffer
- [X] T017 [US1] mpmc_dequeue() implementation verified - retrieves item from circular buffer

---

## Phase 4: User Story 2 - Bounded Queue with Backpressure (Priority: P1)

**Goal**: Queue capacity limits and backpressure behavior

**Independent Test**: Verify enqueue returns MPMC_FULL when queue is at capacity

- [X] T018 [US2] mpmc_enqueue() returns MPMC_FULL when tail - head >= capacity
- [X] T019 [US2] mpmc_dequeue() creates available capacity after removal

---

## Phase 5: User Story 3 - FIFO Ordering (Priority: P2)

**Goal**: FIFO ordering for sequential enqueue/dequeue

**Independent Test**: Verify items dequeue in same order as enqueued

- [X] T020 [US3] Sequential enqueue/dequeue maintains FIFO order

---

## Phase 6: User Story 4 - Non-Blocking Dequeue Option (Priority: P2)

**Goal**: Non-blocking dequeue operation

**Independent Test**: Verify dequeue returns immediately with empty status when queue is empty

- [X] T021 [US4] mpmc_dequeue() returns MPMC_EMPTY immediately when queue is empty

---

## Phase 7: User Story 5 - Memory-Efficient Circular Implementation (Priority: P3)

**Goal**: Circular buffer with wraparound behavior

**Independent Test**: Verify buffer wraparound and memory reuse

- [X] T022 [US5] mpmc_idx() correctly wraps index using pos % capacity

---

## Phase 8: User Story 6 - Batch Enqueue (Priority: P2)

**Goal**: Batch enqueue of multiple items

**Independent Test**: Verify batch of 10 items can be enqueued in single call

- [X] T023 [US6] mpmc_enqueue_batch() implementation verified - loop copying items
- [X] T024 [US6] mpmc_enqueue_batch() returns actual count enqueued
- [X] T025 [US6] Partial batch when count exceeds available capacity

---

## Phase 9: User Story 7 - Batch Dequeue (Priority: P2)

**Goal**: Batch dequeue of multiple items

**Independent Test**: Verify batch of 10 items can be dequeued in single call

- [X] T026 [US7] mpmc_dequeue_batch() implementation verified - loop copying items
- [X] T027 [US7] mpmc_dequeue_batch() returns actual count dequeued
- [X] T028 [US7] Partial batch when fewer items than requested available

---

## Phase 10: User Story 8 - Batch Size Limits and Partial Results (Priority: P2)

**Goal**: Accurate partial batch handling

**Independent Test**: Verify batch API returns accurate count when fewer items than requested

- [X] T029 [US8] mpmc_enqueue_batch() handles partial batch correctly
- [X] T030 [US8] mpmc_dequeue_batch() handles partial batch correctly

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
- [X] T042 [P] C11 atomics usage (_Atomic, atomic_load/store)
- [ ] T043 Update checklist status and commit changes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: Completed
- **Foundational (Phase 2)**: Completed - unblocks all user stories
- **User Stories (Phase 3-14)**: All completed
- **Polish (Phase 15)**: 1 task remaining (T043 commit)

### Summary

All implementation tasks completed. Only T043 (commit changes) remains.

---

## Implementation Strategy

### Completed Milestones

1. ✓ Phase 1: Setup - 6 files created
2. ✓ Phase 2: Foundational - All core MPMC functions implemented
3. ✓ Phase 3-4: US1-2 - Core enqueue/dequeue + bounded behavior
4. ✓ Phase 5-8: US3-8 - FIFO, non-blocking, batch operations
5. ✓ Phase 9-10: US9-10 - 2D ReadyQueue matrix
6. ✓ Phase 11-12: US11-12 - CompleteQueue
7. ✓ Phase 13-14: Polish - Compilation verified

### Remaining Work

- T043: Commit changes to git

---

## Notes

- Implementation complete - all queue operations functional
- All queue operations are O(1)
- Lock-free operations use C11 atomics only
- No mutexes in hot paths
- Header-only library design