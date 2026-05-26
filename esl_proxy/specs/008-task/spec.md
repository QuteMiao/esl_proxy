# Feature Specification: Task

**Feature Branch**: `008-task`

**Created**: 2026-05-25

**Status**: Draft

**Input**: User description: "008-task仅包含task描述信息，移除执行相关信息"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Task Description Only (Priority: P1)

A system operator creates a task descriptor containing only task description information: task ID, task type, organization mode, kernel pointer, index, instance count, and priority. Execution-related information (state, completion status, executor assignment) is stored separately by the DAG engine and is not part of the task descriptor.

**Why this priority**: Separating task description from execution state follows separation of concerns - the descriptor describes WHAT to execute, while execution state tracks HOW it is executing.

**Independent Test**: Can be tested by creating a task descriptor and verifying it contains only description fields and no execution state fields.

**Acceptance Scenarios**:

1. **Given** a task descriptor is created, **When** it contains task ID, task type, and organization mode, **Then** it does not contain execution state fields (state, completion status)
2. **Given** a task descriptor is created, **When** it is passed to the Orchestrator, **Then** the Orchestrator stores description data separately from execution state

---

### User Story 2 - Task Type Definition (Priority: P1)

A system operator defines tasks with three distinct types: CUBE, VECTOR, and MIX. Task type is stored in the task descriptor as a description field, not an execution state.

**Why this priority**: Task type is a fundamental description of how a task should execute - the Orchestrator stores it in the task descriptor for the Dispatch to read.

**Independent Test**: Can be tested by submitting tasks of each type and verifying the system correctly identifies and routes each type.

**Acceptance Scenarios**:

1. **Given** a task is submitted with type CUBE in its descriptor, **When** the system processes the task, **Then** it is recognized as a CUBE task with standard execution semantics
2. **Given** a task is submitted with type VECTOR in its descriptor, **When** the system processes the task, **Then** it is recognized as a VECTOR task with standard execution semantics
3. **Given** a task is submitted with type MIX in its descriptor, **When** the system processes the task, **Then** it is recognized as a MIX task requiring joint CUBE and VECTOR execution

---

### User Story 3 - Task Organization Modes (Priority: P1)

A system operator organizes tasks using one of four organization modes: Single, Group, SPMD Synchronous, or SPMD Asynchronous. Organization mode is a description field in the task descriptor, not execution state.

**Why this priority**: Organization mode is a fundamental description of how task instances are created and synchronized - stored in the task descriptor for the Dispatch to read.

**Independent Test**: Can be tested by submitting tasks with each organization mode and verifying the system handles instantiation, execution, and completion according to the specified mode semantics.

**Acceptance Scenarios**:

1. **Given** a task is submitted with organization mode Single in its descriptor, **When** the system dispatches the task, **Then** a single instance of the task is created and executed independently
2. **Given** a task is submitted with organization mode Group in its descriptor, **When** the system dispatches the task, **Then** multiple instances are created as described in the descriptor
3. **Given** a task is submitted with organization mode SPMD Synchronous in its descriptor, **When** the system dispatches the task, **Then** multiple instances are created for lockstep execution as described
4. **Given** a task is submitted with organization mode SPMD Asynchronous in its descriptor, **When** the system dispatches the task, **Then** multiple independent instances are created as described

---

### User Story 4 - SPMD INDEX in Task Descriptor (Priority: P1)

A system operator submits SPMD tasks where the task descriptor contains the KERNEL pointer and a base INDEX value. Each instance's unique INDEX is derived from the base INDEX plus instance number, written to Executor registers during dispatch.

**Why this priority**: INDEX is a task description parameter - the base INDEX is stored in the task descriptor, and the Dispatch derives per-instance INDEX values during dispatch.

**Independent Test**: Can be tested by dispatching an SPMD task with N instances and verifying each Executor receives the same KERNEL but a unique INDEX.

**Acceptance Scenarios**:

1. **Given** an SPMD task descriptor contains KERNEL and base INDEX, **When** the Dispatch dispatches the task, **Then** the same KERNEL is dispatched to all N Executors
2. **Given** the KERNEL is dispatched to N Executors, **When** each Executor receives its dispatch, **Then** it receives a unique INDEX value derived from base INDEX plus instance number
3. **Given** an Executor receives a dispatch with KERNEL and INDEX, **When** it executes, **Then** it uses the INDEX to select its data partition while running the shared KERNEL

---

### User Story 5 - Task Descriptor Reuse (Priority: P1)

A system operator reuses the same task descriptor for multiple task submissions. The task descriptor contains static description information, while execution state is managed per-task-instance by the DAG engine.

**Why this priority**: Task descriptors should be reusable - the description of a task (its type, kernel, priority) does not change between submissions, only the execution state varies per instance.

**Independent Test**: Can be tested by submitting the same task descriptor twice and verifying both instances execute correctly with independent state tracking.

**Acceptance Scenarios**:

1. **Given** a task descriptor with type CUBE and priority 5 is created, **When** it is submitted twice, **Then** both instances execute as CUBE tasks with priority 5
2. **Given** task descriptors are reusable, **When** the same descriptor is used for sequential submissions, **Then** execution state is tracked independently per instance

---

### User Story 6 - Clear Separation of Task Description and Execution State (Priority: P1)

A system operator relies on the DAG engine to maintain execution state separately from task description. The Orchestrator manages task descriptors (description), while the Dispatch/Executor manage execution state (state transitions, completion).

**Why this priority**: Clear separation allows independent evolution of the task description model and execution state model without coupling changes.

**Independent Test**: Can be tested by verifying that task descriptors do not reference execution state and execution state does not duplicate task description fields.

**Acceptance Scenarios**:

1. **Given** a task descriptor exists, **When** execution completes, **Then** the task descriptor remains unchanged and reusable
2. **Given** execution state tracks task progress, **When** a task completes, **Then** the execution state is updated but the original descriptor is unchanged

---

### Edge Cases

- What happens when a task descriptor contains NULL kernel pointer?
- What happens when an executor receives a task descriptor with an unknown task type?
- What happens when successor count exceeds 3? (Extension entry used via next pointer)
- What happens when the same task descriptor is submitted concurrently?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: A task descriptor MUST contain only task description information (task ID, type, organization mode, kernel, index, instance_count, priority, userdata)
- **FR-002**: A task descriptor MUST NOT contain execution state information (state, completion status, executor assignment, Ring Buffer indices)
- **FR-003**: Task type MUST be stored as a description field (CUBE, VECTOR, MIX)
- **FR-004**: Organization mode MUST be stored as a description field (Single, Group, SPMD Synchronous, SPMD Asynchronous)
- **FR-005**: The INDEX for SPMD tasks MUST be stored as a base value in the task descriptor; per-instance INDEX is derived during dispatch
- **FR-006**: The same task descriptor instance MAY be reused for multiple task submissions
- **FR-007**: Task descriptor MUST NOT contain pointers or references to internal execution state
- **FR-008**: The Dispatch MUST route tasks based on task type from the descriptor
- **FR-009**: The Dispatch MUST derive per-instance INDEX values (base INDEX + instance number) during SPMD dispatch
- **FR-010**: Task dependencies MUST be resolved correctly based on task ID in the descriptor; Dependency Information Ring Buffer MUST store successor count, successor node list, predecessor count; successor storage via base entry (3 inline successors) plus extension entries via 2-byte next pointer
- **FR-011**: Task runtime information Ring Buffer MUST store input data address, output data address, and kernel address for task execution

### Key Entities *(include if feature involves data)*

- **Task Descriptor**: A data structure containing only task description fields. Contains: task ID, task type, organization mode, kernel pointer, base INDEX, instance count, priority, userdata.
- **Task Description Information**: Static fields that describe what to execute: task ID, type (CUBE/VECTOR/MIX), organization mode (Single/Group/SPMD_SYNC/SPMD_ASYNC), kernel pointer, base INDEX, instance count, priority, userdata.
- **Task Type**: An attribute in the task descriptor indicating its execution model. Values: CUBE, VECTOR, MIX.
- **Task Organization Mode**: An attribute in the task descriptor defining how task instances are created. Values: Single, Group, SPMD Synchronous, SPMD Asynchronous.
- **KERNEL**: The executable code pointer stored in the task descriptor. All SPMD instances share this same KERNEL pointer.
- **Base INDEX**: A value stored in the task descriptor. Per-instance INDEX is derived as (base INDEX + instance number) during dispatch.
- **Task ID**: A 2-byte identifier in the task descriptor, used for DAG dependency resolution and Ring Buffer indexing.
- **Dependency Information**: Stored in separate Dependency Information Ring Buffer (not in task descriptor). Contains: successor count, list of successor TaskIDs, predecessor count. Base entry contains 3 inline successor TaskIDs plus 2-byte next pointer to extension entry. Extension entries linked via next pointer, each storing 3 additional successor TaskIDs.
- **Runtime Information**: Stored in separate Ring Buffer. Contains: input data address, output data address, kernel address. Mutable per-task-instance.
- **Execution State**: Maintained separately by DAG engine components (Dispatcher, Executor, Cutter), not part of task descriptor. Includes: state, completion status, executor assignment.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Task descriptors contain only description fields (task ID, type, org_mode, kernel, index, instance_count, priority, userdata) - no execution state
- **SC-002**: Task descriptors are reusable across multiple submissions without modification
- **SC-003**: Task type and organization mode are stored as description fields in the descriptor
- **SC-004**: Base INDEX is stored in task descriptor; per-instance INDEX is derived during dispatch
- **SC-005**: Clear boundary between task description (Orchestrator-owned descriptor) and execution state (Dispatcher/Executor-owned)

## Assumptions

- Task description fields are populated by the Orchestrator at task creation time
- Execution state is managed by Dispatch/Executor/Cutter components, not in the task descriptor
- Task descriptors are created before the DAG is constructed and remain stable during execution
- Successor information stored via base entry (3 inline successors) + extension entries (2-byte next pointer); Ring Buffer storage managed by other DAG components
- Task ID is a 2-byte value used for Ring Buffer indexing via TASKID & RING_SIZE
- Runtime information includes input data address, output data address, and kernel address