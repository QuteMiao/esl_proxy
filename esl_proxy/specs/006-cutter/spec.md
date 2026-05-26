# Feature Specification: Cutter - Dependency Resolution

**Feature Branch**: `006-cutter`

**Created**: 2026-05-22

**Status**: Draft

**Input**: User description: "cutter通过共享内存的方式获取dispatch捕获的已完成任务，读取orchestator构造的图，解依赖"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Completed Task Collection via Shared Memory (Priority: P1)

A system operator relies on the Cutter to collect completed task results from the Dispatch via shared memory. The Cutter reads the task execution results (output data, completion status) that the Dispatch has captured, enabling downstream processing.

**Why this priority**: Completed task collection is the Cutter's primary function - it must receive task results from the Dispatch pipeline.

**Independent Test**: Can be tested by having Dispatch write completed tasks to shared memory and verifying Cutter reads them correctly.

**Acceptance Scenarios**:

1. **Given** a Dispatch captures a completed task with output data, **When** the Cutter reads from shared memory, **Then** it retrieves the complete task result
2. **Given** multiple tasks complete around the same time, **When** the Cutter reads from shared memory, **Then** all completed tasks are retrieved in order
3. **Given** shared memory contains completed task results, **When** the Cutter reads, **Then** the data reflects the actual task outputs without corruption

---

### User Story 2 - Graph Reading from Orchestrator (Priority: P1)

A system operator uses the Cutter to read the task dependency graph constructed by the Orchestrator. The Cutter accesses the shared memory region where the Orchestrator wrote the DAG structure, enabling dependency analysis.

**Why this priority**: The Cutter must understand the task graph to determine which tasks become ready when dependencies complete.

**Independent Test**: Can be tested by having Orchestrator write a graph to shared memory and verifying Cutter reads the complete structure.

**Acceptance Scenarios**:

1. **Given** the Orchestrator constructs and writes a DAG to shared memory, **When** the Cutter reads the graph, **Then** it retrieves the complete task nodes and edges
2. **Given** the Orchestrator updates the graph (adding new tasks), **When** the Cutter reads, **Then** it sees the updated graph structure
3. **Given** the Cutter reads the graph, **When** it analyzes dependencies, **Then** all precedence constraints are correctly captured

---

### User Story 3 - Dependency Resolution (Priority: P1)

A system operator relies on the Cutter to analyze completed tasks against the graph to identify newly ready tasks. When a task completes, the Cutter checks which dependent tasks now have all their predecessors satisfied, making them ready for execution.

**Why this priority**: Dependency resolution is the core logic of the Cutter - determining what tasks are now runnable.

**Independent Test**: Can be tested by completing Task A and verifying that Task B (which depends only on A) is identified as ready.

**Acceptance Scenarios**:

1. **Given** Task B depends on Task A (among others), **When** Task A completes but Task B still has incomplete predecessors, **Then** Task B is NOT identified as ready
2. **Given** Task B depends only on Task A, **When** Task A completes, **Then** Task B is identified as ready for execution
3. **Given** multiple tasks become ready simultaneously, **When** the Cutter resolves dependencies, **Then** all newly ready tasks are identified together

---

### User Story 4 - Ready Task Notification (Priority: P1)

A system operator depends on the Cutter to notify downstream components (e.g., Orchestrator or Dispatch) about newly ready tasks. The Cutter writes ready task information to shared memory or signals availability, enabling the pipeline to continue.

**Why this priority**: Ready task notification closes the loop - completed tasks trigger new task execution.

**Independent Test**: Can be tested by having Cutter write ready task info and verifying downstream components receive it.

**Acceptance Scenarios**:

1. **Given** the Cutter identifies Task B as ready, **When** it writes the ready notification, **Then** the notification is accessible to the Orchestrator or Dispatch
2. **Given** multiple tasks become ready, **When** the Cutter writes notifications, **Then** all ready tasks are included in a single batch notification

---

### User Story 5 - Cutter Synchronization (Priority: P2)

A system operator needs the Cutter to synchronize access to shared memory with the Dispatch (writing) and Orchestrator (writing). The Cutter must not read partial or inconsistent data during concurrent access.

**Why this priority**: Proper synchronization ensures the Cutter reads consistent graph and task completion data.

**Independent Test**: Can be tested by simulating concurrent read/write and verifying the Cutter handles it correctly.

**Acceptance Scenarios**:

1. **Given** the Dispatch is writing completed task data, **When** the Cutter attempts to read, **Then** proper synchronization ensures consistent data is read
2. **Given** the Orchestrator updates the graph while the Cutter reads, **When** the Cutter synchronization operates, **Then** no corrupted graph data is processed

---

### User Story 6 - Cutter Lifecycle (Priority: P2)

A system operator creates a Cutter instance, uses it for dependency resolution, and shuts it down cleanly. The Cutter properly releases shared memory attachments on shutdown.

**Why this priority**: Proper lifecycle management prevents resource leaks.

**Independent Test**: Can be tested by creating and destroying Cutter and verifying clean resource release.

**Acceptance Scenarios**:

1. **Given** a Cutter is created and attached to shared memory, **When** it is destroyed, **Then** all shared memory attachments are released
2. **Given** a Cutter is shutting down, **When** dependency resolution is in progress, **Then** shutdown completes cleanly without leaks

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The Cutter MUST read completed task results from shared memory written by the Dispatch
- **FR-002**: The Cutter MUST read the task dependency graph from shared memory written by the Orchestrator
- **FR-003**: The Cutter MUST analyze completed tasks against the graph to identify newly ready tasks
- **FR-004**: When a task's all predecessors are complete, the Cutter MUST identify that task as ready
- **FR-005**: The Cutter MUST write ready task notifications to shared memory for downstream consumption
- **FR-006**: The Cutter MUST synchronize access to shared memory with Dispatch and Orchestrator
- **FR-007**: The Cutter MUST properly detach from shared memory on shutdown

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Cutter reads completed task data from shared memory within 1 microsecond of availability
- **SC-002**: Dependency resolution identifies ready tasks within 10 microseconds of task completion
- **SC-003**: Ready task notifications are written to shared memory within 5 microseconds of resolution
- **SC-004**: Cutter synchronization causes no data corruption under concurrent access
- **SC-005**: Cutter clean shutdown completes within 1 millisecond with no shared memory leaks
- **SC-006**: The Cutter correctly identifies ready tasks for graphs with up to 10,000 tasks and 50,000 edges

---

## Assumptions

- Shared memory regions are pre-established among Orchestrator, Dispatch, and Cutter via configuration
- The Orchestrator, Dispatch, and Cutter agree on shared memory data formats
- Synchronization uses atomic operations or similar lock-free primitives
- Task completion in the graph means the task's output data is available and consistent
- The Cutter is trusted to provide valid inputs per the Trust the Caller principle
- Dependency resolution is performed on complete task results only (no partial results)