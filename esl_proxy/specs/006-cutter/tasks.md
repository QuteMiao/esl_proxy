# Tasks: Cutter - Dependency Resolution

**Input**: Design documents from `/specs/006-cutter/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Not explicitly requested in spec; tests included per Constitution Principle VIII (Testability & Reproducibility) — dependency injection enables unit testing without coupling to shared memory.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

**Header-only C library**: `include/` at repository root. No `src/` compilation units. Tests in `tests/`.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Header file scaffolding and shared memory layout constants

- [X] T001 [P] Create `include/shmem_layout.h` with shared memory region descriptors and constants
- [X] T002 [P] Create `include/cutter.h` public API header with function declarations and `cutter_cfg_t`
- [X] T003 [P] Create `include/cutter_impl.h` internal implementation header with `cutter_ctx`, callback types, and internal function declarations
- [X] T004 Create `tests/` directory structure (`tests/unit/`, `tests/integration/`)

**Checkpoint**: All three header files exist and form a coherent API surface

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core types and constants that ALL user stories depend on. No user story work can begin until this phase is complete.

**CRITICAL**: No user story work can begin until this phase is complete

- [X] T005 Define `completed_region_t`, `dag_region_t`, `ready_region_t` layout structs in `include/shmem_layout.h`
- [ ] T006 Define `queue_t` and `atomic_read_queue_tail()` helper for reading queue cursor in `include/shmem_layout.h`
- [ ] T007 Define `cutternotif_fn` callback type and `cutter_cfg_t` config struct in `include/cutter.h`
- [ ] T008 Define `cutter_ctx` internal struct fields (shmem handles, notify_fn, notify_userdata) in `include/cutter_impl.h`
- [ ] T009 Implement `cutter_init()` in `include/cutter_impl.h` — validates handles, sets up ctx
- [ ] T010 Implement `cutter_shutdown()` in `include/cutter_impl.h` — releases handles, clean shutdown < 1ms per SC-005

**Checkpoint**: Cutter can be initialized and shut down. All subsequent user story phases use these foundations.

---

## Phase 3: User Story 1 - Completed Task Collection via Shared Memory (Priority: P1)

**Goal**: Cutter reads completed task results from shared memory written by Dispatch

**Independent Test**: Dispatch writes task IDs to `completed_queue`; Cutter reads them back — data matches without corruption

### Implementation

- [ ] T011 [P] [US1] Implement `cutter_read_complete()` in `include/cutter_impl.h` — read batch of completed task IDs from `completed_region_t.queue`
- [ ] T012 [P] [US1] Implement `atomic_queue_count()` helper in `include/shmem_layout.h` — read `queue.cnt` with `memory_order_acquire`
- [ ] T013 [P] [US1] Implement ring-buffer iteration in `cutter_read_complete()` to walk `[tail, tail+cnt)` positions
- [ ] T014 [US1] Wire `cutter_read_complete()` to `shmem_completed` region pointer in ctx

**Checkpoint**: `cutter_read_complete(ctx, buf, 256)` returns completed task IDs matching what Dispatch wrote

---

## Phase 4: User Story 2 - Graph Reading from Orchestrator (Priority: P1)

**Goal**: Cutter reads the task dependency graph from shared memory written by Orchestrator

**Independent Test**: Orchestrator writes DAG to shared memory; Cutter reads node_count, edge_count, and successor lists — matches written data

### Implementation

- [ ] T015 [P] [US2] Implement `cutter_read_dag_header()` in `include/cutter_impl.h` — read `node_count`, `edge_count` from `dag_region_t`
- [ ] T016 [P] [US2] Implement `cutter_walk_successors()` in `include/cutter_impl.h` — iterate `successorList` adjacency for a given task
- [ ] T017 [US2] Build local `remaining_pred[]` array from DAG edges — in-degree per task — stored as `static _Thread_local` in `cutter_impl.h`
- [ ] T018 [US2] Wire DAG region read to `shmem_dag` pointer in ctx; confirm `node_count <= 10000` and `edge_count <= 50000` per SC-006 bounds

**Checkpoint**: Cutter can read the full DAG structure from shared memory; `remaining_pred[task_id]` correctly reflects in-degree for all tasks

---

## Phase 5: User Story 3 - Dependency Resolution (Priority: P1)

**Goal**: Cutter analyzes completed tasks against the graph to identify newly ready tasks

**Independent Test**: Task A completes; Task B depends only on A — Cutter identifies B as ready (successor_cnt reaches 0)

### Implementation

- [ ] T019 [P] [US3] Implement `cutter_resolve()` in `include/cutter_impl.h` — for given completed `task_id`, walk successor list and atomically decrement `g_state_buf[successor].successor_cnt`
- [ ] T020 [P] [US3] Implement `atomic_fetch_sub_acq_rel()` helper for lock-free decrement of successor count
- [ ] T021 [US3] Implement ready-batch collection: when `successor_cnt` reaches 0, add to `ready_batch[]` array (max CUTTER_MAX_READY_BATCH=256)
- [ ] T022 [US3] Implement batch fallback scan: if `remaining_pred` array shows tasks with cnt==0 not yet in ready batch, collect them (for graph updates)
- [ ] T023 [US3] Verify SC-001 (<1μs read), SC-002 (<10μs resolution) targets via inline timing in `_DEBUG` builds

**Checkpoint**: Calling `cutter_resolve(ctx, completed_task_id)` correctly identifies all tasks that just became ready; ready_batch is populated

---

## Phase 6: User Story 4 - Ready Task Notification (Priority: P1)

**Goal**: Cutter writes ready-task notifications to shared memory for Dispatch consumption

**Independent Test**: Cutter writes ready task IDs to shared memory; Dispatch reads them — all IDs match, seq_num incremented

### Implementation

- [ ] T024 [P] [US4] Implement `cutter_write_ready()` in `include/cutter_impl.h` — write ready_batch to `ready_region_t.ready_queue`
- [ ] T025 [P] [US4] Implement `atomic_queue_enqueue()` in `include/shmem_layout.h` — write task_id to `queue.tasks[pos]`, increment `queue.head` with `memory_order_release`
- [ ] T026 [US4] Implement `seq_num` increment with `memory_order_release` after batch write to signal Dispatch
- [ ] T027 [US4] Implement `cutter_write_ready()` with batching: write all collected ready IDs before incrementing `seq_num` (per contract: single batch notification)
- [ ] T028 [US4] Verify SC-003 (<5μs write) target via inline timing in `_DEBUG` builds

**Checkpoint**: After `cutter_resolve()` + `cutter_write_ready()`, Dispatch sees new ready task IDs in shared memory with incremented `seq_num`

---

## Phase 7: User Story 5 - Cutter Synchronization (Priority: P2)

**Goal**: Cutter synchronizes access to shared memory with Dispatch (writer) and Orchestrator (writer) without mutexes/spinlocks

**Independent Test**: Dispatch writes while Cutter reads; Orchestrator updates DAG while Cutter reads — Cutter reads consistent data with no corruption

### Implementation

- [ ] T029 [P] [US5] Add `memory_order_acquire` to all shared memory reads in `include/shmem_layout.h` helpers
- [ ] T030 [P] [US5] Add `memory_order_release` to all shared memory writes in `include/shmem_layout.h` helpers
- [ ] T031 [US5] Verify lock-free contract: no `pthread_mutex`, `sem_wait`, or spinlock in any hot-path function
- [ ] T032 [US5] Verify SC-004 (no data corruption under concurrent access) by adding chaos-test scaffolding in `tests/integration/cutter_pipeline_test.c`

**Checkpoint**: Cutter uses only C11 atomics for all shared memory synchronization; no blocking primitives anywhere in header

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, cleanup, and performance verification

- [ ] T033 [P] Compile all headers with `-Wall -Werror -Wextra -pedantic` on GCC/Clang to verify zero warnings
- [ ] T034 [P] Add `#include <stdatomic.h>` and `#include <stdint.h>` guard in `include/cutter.h` to ensure C11 availability
- [ ] T035 [P] Add `restrict` qualifiers to all pointer parameters in public API functions per Constitution IX
- [ ] T036 [P] Verify all public functions are `static inline` in headers per Constitution IX (Header-Only Library)
- [ ] T037 [P] Create `tests/unit/cutter_dep_test.c` with mock DAG and mock queue to unit-test dependency resolution in isolation
- [ ] T038 [P] Create `tests/unit/cutter_shmem_test.c` with mock shared memory regions to unit-test read/write without real shmem
- [ ] T039 [P] Create `tests/integration/cutter_pipeline_test.c` with mock Orchestrator/Dispatch to integration-test full pipeline
- [ ] T040 Verify SC-006 (10k tasks / 50k edges support) by adding stress-test DAG generation scaffold

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately; all three header files can be created in parallel
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories; T005–T010 are sequential (each builds on the last layout definition)
- **User Stories (Phase 3–7)**: All depend on Foundational completion
  - US1 and US2 can start in parallel (different shmem regions: completed vs dag)
  - US3 depends on both US1 (to know what completed) and US2 (to walk successors)
  - US4 depends on US3 (needs resolved ready tasks to write)
  - US5 can run in parallel with any story (synchronization touches all regions)
- **Polish (Phase 8)**: Depends on all user stories complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) — reads completed queue
- **User Story 2 (P1)**: Can start after Foundational (Phase 2) — reads DAG graph
- **User Story 3 (P1)**: Depends on US1 + US2 — needs both completed tasks and successor graph
- **User Story 4 (P1)**: Depends on US3 — writes ready tasks discovered by resolution
- **User Story 5 (P2)**: Can run in parallel with any story — synchronization touches all regions

### Within Each User Story

- Header definitions before implementation
- Private helpers ([P]) before the main function
- Main function last in the phase
- Core implementation before validation tasks

---

## Parallel Opportunities

- Phase 1: T001, T002, T003, T004 all run in parallel (different files, no cross-dependencies)
- Phase 2: T005, T006, T007 can start in parallel (different header sections); T008, T009, T010 are sequential
- Phase 3: T011, T012, T013, T014 all run in parallel (sub-functions of same phase)
- Phase 4: T015 and T016 run in parallel (read vs walk)
- Phase 5: T019, T020 run in parallel (resolve function + atomic helper)
- Phase 6: T024, T025 run in parallel (write function + enqueue helper)
- Phase 7: T029, T030 run in parallel (read-barrier vs write-barrier additions)
- Phase 8: T033–T040 all run in parallel (different files, no conflicts)

---

## Implementation Strategy

### MVP First (User Story 1 only — Completed Task Collection)

1. Complete Phase 1: Setup (T001–T004)
2. Complete Phase 2: Foundational (T005–T010)
3. Complete Phase 3: User Story 1 (T011–T014)
4. **STOP and VALIDATE**: Dispatch writes completed tasks → Cutter reads them back
5. Deploy/demo MVP

### Incremental Delivery

1. Phase 1 + Phase 2 → Foundation ready
2. Add User Story 1 → Test independently → Deploy (MVP)
3. Add User Story 2 → Test independently → Deploy
4. Add User Story 3 → Test independently → Deploy
5. Add User Story 4 → Test independently → Deploy
6. Add User Story 5 (synchronization) → Test independently → Deploy
7. Polish Phase 8 → Final validation

### Parallel Team Strategy

With multiple developers after Phase 2:

- Developer A: User Story 1 (completed task collection)
- Developer B: User Story 2 (graph reading) — parallel with A
- Developer C: User Story 3 (dependency resolution) — after A+B complete
- Developer D: User Story 4 (ready notification) — after C complete

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story independently completable and testable
- Header-only library: no `src/` compilation units — all implementation in `include/`
- C11 atomics only — no mutexes/spinlocks in hot paths per Constitution
- Dependency injection via function pointers enables unit testing without real shared memory
- Verify compile with `-Wall -Werror -Wextra -pedantic` before each story checkpoint
