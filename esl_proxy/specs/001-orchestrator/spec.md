# Feature Specification: Orchestrator

**Feature Branch**: `001-orchestrator`

**Created**: 2026-05-22

**Status**: Draft

**Input**: User description: "目标：设计一个名为 Graph 的类，用户可以通过它直观地编排任务依赖关系。任务声明：用户通过传递输入数据地址、输出数据地址、常量、Kernel地址来创建 Task。拓扑编排：支持 A.precede(B)（A 在 B 之前执行）或 B.succeed(A)（B 在 A 之后执行）的操作符或方法。"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Task Graph Construction (Priority: P1)

A data engineer needs to define a pipeline of computation tasks where each task depends on the outputs of previous tasks. They create a Graph, add Tasks representing each computational step, and wire them together using precedence constraints.

**Why this priority**: This is the core use case - users must be able to build a DAG of tasks with dependencies.

**Independent Test**: Can be tested by creating a simple 3-task graph, verifying that execution respects the dependency order.

**Acceptance Scenarios**:

1. **Given** an empty Graph, **When** the user adds Task A and Task B, **Then** both tasks exist in the graph with no dependency between them
2. **Given** Task A and Task B exist in a Graph, **When** the user calls `A.precede(B)`, **Then** B will execute after A completes
3. **Given** Task A and Task B exist in a Graph, **When** the user calls `B.succeed(A)`, **Then** the same dependency is established as `A.precede(B)`

---

### User Story 2 - Task Data Binding (Priority: P1)

A data engineer needs to specify what data each task reads from and writes to. They provide memory addresses for input/output data, constants, and the kernel (computation function) when creating each Task.

**Why this priority**: Task execution requires knowing the data sources and destinations - this is fundamental to task operation.

**Independent Test**: Can be tested by creating Tasks with data bindings and verifying the Graph captures these bindings correctly.

**Acceptance Scenarios**:

1. **Given** a user creates a Task with input address X, output address Y, input output address W, constant Z, duration M, subTaskCnt N , taskType V and kernel address K, **When** the Task is added to a Graph, **Then** the Graph stores these bindings
2. **Given** a Task with input bound to output of another Task, **When** the Graph executes, **Then** data flows from the producing Task to the consuming Task

---

### User Story 3 - Parallel Execution Planning (Priority: P2)

A data engineer wants to understand how their task graph will be executed. The Graph should expose information about execution order - specifically, which tasks can run in parallel.

**Why this priority**: Users need to verify the scheduler will exploit parallelism as expected.

**Independent Test**: Can be tested by creating a diamond dependency pattern and verifying tasks on different branches can execute concurrently.

**Acceptance Scenarios**:

1. **Given** a Graph with a diamond pattern (A→B, A→C, B→D, C→D), **When** the user queries parallel execution groups, **Then** {A} is in the first group, {B, C} in the second, and {D} in the third
2. **Given** a Graph is valid (no cycles), **When** the user requests topological order, **Then** the returned order respects all precedence constraints

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Users MUST be able to create a Graph instance
- **FR-002**: Users MUST be able to create a Task by providing input addresses, output addresses，input output addresses, constants, and kernel address
- **FR-003**: Users MUST be able to add Tasks to a Graph
- **FR-004**: Users MUST be able to express precedence via `A.precede(B)` method
- **FR-005**: Users MUST be able to express precedence via `B.succeed(A)` method
- **FR-006**: The Graph MUST provide topological ordering of Tasks
- **FR-007**: The Graph MUST identify independent Tasks that can execute in parallel
- **FR-008**: The Graph MUST validate that all Task references are resolvable before execution

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can construct a 100-task DAG with mixed precedence constraints in under 5 minutes
- **SC-002**: Cycle detection completes in O(V+E) time where V=tasks, E=edges
- **SC-003**: Independent task groups are correctly identified for parallel execution
- **SC-004**: Users receive clear error messages when cycle is detected, identifying the specific Tasks involved

---

## Assumptions

- Users have a valid kernel implementation (function pointer/address) for each Task
- Memory addresses provided are valid and accessible during Graph execution
- The Graph is intended for single-threaded construction but multi-threaded execution
- No automatic data flow inference - users explicitly specify input/output bindings
