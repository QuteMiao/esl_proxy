# Tasks: Task

**Input**: Design documents from `/specs/008-task/`

**Prerequisites**: plan.md (required), spec.md (required for user stories)

**Tests**: Tests NOT requested in feature specification

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- Source code: `include/dag/` at repository root
- Header-only C library: all implementation in `.h` files
- Single header per feature: `task.h`

---

## Phase 1: Setup (Project Initialization)

**Purpose**: Verify directory structure for header-only library

- [X] T001 Verify include/dag/ directory exists per plan.md

---

## Phase 2: Foundational (Core Types & Constants)

**Purpose**: Core types and constants needed by all task operations

**CRITICAL**: Must complete before any user story can be tested

- [X] T002 [P] Define RING_SIZE (4096) and RING_MASK constants in include/dag/task.h
- [X] T003 [P] Define task_id_t as uint16_t for 16-bit TaskID
- [X] T004 [P] Implement ring_idx() helper for O(1) indexing via TaskID & (RING_SIZE - 1)

---

## Phase 3: User Story 1 - Task Description Only (Priority: P1) 🎯 MVP

**Goal**: Task descriptor contains only description fields - no execution state

**Independent Test**: Verify task descriptor has only description fields (id, type, mode, kernel, index, count, prio, data) and no execution state fields

### Implementation for User Story 1

- [X] T005 [P] [US1] Define struct task_desc with description fields only (id, type, mode, kernel, index, count, prio, data) in include/dag/task.h
- [X] T006 [US1] Add comment clarifying NO execution state fields in task_desc

---

## Phase 4: User Story 2 - Task Type Definition (Priority: P1)

**Goal**: Task type enum with CUBE, VECTOR, MIX values

**Independent Test**: Verify task_type_t enum has CUBE=0, VECTOR=1, MIX=2 values

### Implementation for User Story 2

- [X] T007 [P] [US2] Define task_type_t enum (TASK_TYPE_CUBE=0, TASK_TYPE_VECTOR=1, TASK_TYPE_MIX=2) in include/dag/task.h

---

## Phase 5: User Story 3 - Task Organization Modes (Priority: P1)

**Goal**: Organization mode enum with Single, Group, SPMD_SYNC, SPMD_ASYNC values

**Independent Test**: Verify org_mode_t enum has correct values

### Implementation for User Story 3

- [X] T008 [P] [US3] Define org_mode_t enum (ORG_MODE_SINGLE=0, ORG_MODE_GROUP=1, ORG_MODE_SPMD_SYNC=2, ORG_MODE_SPMD_ASYNC=3) in include/dag/task.h

---

## Phase 6: User Story 4 - SPMD INDEX in Task Descriptor (Priority: P1)

**Goal**: Base INDEX stored in task descriptor; per-instance INDEX derived during dispatch

**Independent Test**: Verify task descriptor has index field for base INDEX

### Implementation for User Story 4

- [X] T009 [P] [US4] Add inline helper function to derive per-instance INDEX: base_index + instance_number (in include/dag/task.h)

---

## Phase 7: User Story 5 - Task Descriptor Reuse (Priority: P1)

**Goal**: Same task descriptor instance can be reused for multiple submissions

**Independent Test**: Verify task descriptor structure is self-contained and stateless

### Implementation for User Story 5

- [X] T010 [P] [US5] Add comment in task.h documenting task descriptor reuse semantics (descriptor is const after creation, execution state managed separately)

---

## Phase 8: User Story 6 - Clear Separation of Task Description and Execution State (Priority: P1)

**Goal**: Task descriptor owned by Orchestrator; execution state owned by Dispatcher/Executor

**Independent Test**: Verify task descriptor has no references to execution state

### Implementation for User Story 6

- [X] T011 [P] [US6] Add header comment documenting ownership separation: task descriptor (Orchestrator) vs execution state (Dispatcher/Executor)

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Finalize header for distribution

- [X] T012 [P] Add header guard (dag/task.h)
- [X] T013 [P] Verify all types use concise naming (no dag_ prefix on types/functions)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-8)**: All depend on Foundational phase completion, can run in parallel

### User Story Dependencies

- **User Story 1-6**: All can run in parallel after Foundational phase (independent stories)

### Within Each User Story

- Core type definitions before helpers
- Header complete before polish

---

## Parallel Opportunities

- T002, T003, T004 can run in parallel (different definitions)
- T005, T007, T008 can run in parallel (different type definitions)
- T009, T010, T011 can run in parallel (different helper functions/comments)
- T012, T013 can run in parallel (polish tasks)

---

## Implementation Strategy

### MVP First (User Story 1)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1
4. **STOP and VALIDATE**: Verify task descriptor structure

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 → Test independently
3. Add User Stories 2-6 → Each adds a discrete type/helper
4. Polish → Finalize header for distribution

---

## Notes

- [P] tasks = different files or different definitions within same file
- [Story] label maps task to specific user story for traceability
- Header-only library: no .c implementation files
- Concise naming: types/functions do not use dag_ prefix
- Trust the Caller: no validation, undefined on invalid input