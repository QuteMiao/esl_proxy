# Feature Specification: Dispatch

**Feature Branch**: `004-dispatch`

**Created**: 2026-05-22

**Status**: Draft

**Input**: User description: "dispatch负责任务下发通过共享内存获取orchestrator输出 + 多个dispatch分别管理多个不同的executor，通过work-stealing机制实现负载均衡 + dispatch通过共享内存从orchestrator和cutter获取可以下发的任务 + dispatch通过共享内存将下发的任务写给executor，包括taskID和index + executor仅返回1bit给dispatch告诉它在槽位A中的任务已执行完成 + Dispatch包含多个线程，每个线程管理60个CUBE Executor和60个Vector Executor"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Task Distribution via Shared Memory (Priority: P1)

A system operator uses the Dispatch component to distribute tasks from the Orchestrator to worker nodes. The Dispatch reads Orchestrator's output (task graph topology and ready tasks) through shared memory, enabling zero-copy task distribution between processes or nodes.

**Why this priority**: Shared memory communication is essential for high-performance, low-latency task distribution without data copying overhead.

**Independent Test**: Can be tested by having Dispatch read task information from shared memory and verifying the data matches what the Orchestrator wrote.

**Acceptance Scenarios**:

1. **Given** an Orchestrator has computed task topology and ready tasks, **When** the Dispatch accesses shared memory, **Then** it retrieves the complete task graph structure
2. **Given** the Dispatch retrieves task information from shared memory, **When** it distributes tasks to workers, **Then** the task data is transferred without additional copying
3. **Given** shared memory contains a valid task graph, **When** the Dispatch reads it, **Then** the data reflects the current state of the Orchestrator's output

---

### User Story 1b - Task Dispatch to Executor via Shared Memory (Priority: P1)

A system operator relies on the Dispatch to write task information to the Executor through shared memory. The Dispatch writes the taskID and index to shared memory, enabling the Executor to retrieve and execute the task without additional coordination overhead.

**Why this priority**: Writing task metadata to shared memory is how the Dispatch communicates executable tasks to the Executor - this is the core dispatch protocol.

**Independent Test**: Can be tested by having Dispatch write taskID and index to shared memory and verifying the Executor reads the correct values.

**Acceptance Scenarios**:

1. **Given** the Dispatch selects a task for distribution, **When** it writes to shared memory, **Then** the taskID and index are stored in the shared memory region accessible to the Executor
2. **Given** the Dispatch writes task information to shared memory, **When** the Executor reads, **Then** it retrieves the correct taskID and index matching what was written
3. **Given** the Dispatch writes to shared memory for Executor E1, **When** Executor E2 reads from the same region, **Then** it does not see E1's task data (proper isolation)

---

### User Story 1c - Executor Completion Notification via Shared Memory (Priority: P1)

A system operator relies on the Executor to notify the Dispatch about task completion through shared memory using a minimal 1-bit signal. After executing a task from Slot A, the Executor sets an atomic bit in shared memory, indicating the task has completed. The Dispatch polls this bit to determine when the Slot A task is done.

**Why this priority**: Executor completion notification closes the feedback loop - the Dispatch must receive a signal when tasks complete to remove them from the queue and enable the Cutter to resolve dependencies for newly ready tasks.

**Independent Test**: Can be tested by having Executor set the completion bit in shared memory and verifying the Dispatch reads the correct value.

**Acceptance Scenarios**:

1. **Given** the Executor completes a task from Slot A, **When** it sets the completion bit, **Then** the bit is stored in the shared memory region accessible to the Dispatch
2. **Given** the Executor sets the completion bit to 1, **When** the Dispatch reads, **Then** it retrieves the value 1 indicating task completion
3. **Given** the Dispatch reads the completion bit as 1, **When** it processes the notification, **Then** it knows the task in Slot A is complete and can submit the next task
4. **Given** the Executor has not yet completed the task, **When** the Dispatch reads the completion bit, **Then** it retrieves the value 0 indicating task in progress

---

### User Story 2 - Dual Source Task Acquisition (Priority: P1)

A system operator relies on the Dispatch to obtain executable tasks from two sources: the Orchestrator (for initial ready tasks) and the Cutter (for newly ready tasks after dependency resolution). The Dispatch reads from both shared memory regions to build its distribution queue.

**Why this priority**: The Dispatch must service both the initial task graph from Orchestrator and the dependency-resolved ready tasks from Cutter - this is the complete task acquisition pipeline.

**Independent Test**: Can be tested by having both Orchestrator and Cutter write ready tasks to shared memory and verifying Dispatch reads from both.

**Acceptance Scenarios**:

1. **Given** the Orchestrator has ready tasks in shared memory, **When** the Dispatch reads, **Then** it retrieves tasks from the Orchestrator's region
2. **Given** the Cutter has resolved dependencies and has ready tasks, **When** the Dispatch reads, **Then** it retrieves tasks from the Cutter's region
3. **Given** both Orchestrator and Cutter have ready tasks, **When** the Dispatch acquires tasks, **Then** it combines tasks from both sources into the distribution queue
4. **Given** the Dispatch reads from both sources, **When** it distributes tasks, **Then** there is no duplication of tasks in the queue

---

### User Story 3 - Orchestrator Output Integration (Priority: P1)

A system operator relies on the Dispatch to receive real-time updates from the Orchestrator. The Orchestrator writes task execution results and ready-tasks information to shared memory, and the Dispatch reads this output to determine the next set of tasks to distribute.

**Why this priority**: Seamless integration between Orchestrator and Dispatch via shared memory enables the core pipeline of the DAG engine.

**Independent Test**: Can be tested by writing data to shared memory and having Dispatch read and verify it.

**Acceptance Scenarios**:

1. **Given** the Orchestrator produces output and writes it to shared memory, **When** the Dispatch reads from the same shared memory region, **Then** it receives the correct Orchestrator output
2. **Given** the Orchestrator updates shared memory with new ready tasks, **When** the Dispatch polls or receives notification, **Then** it can immediately begin distributing the new tasks

---

### User Story 4 - Multiple Dispatch Management (Priority: P1)

A system operator configures multiple Dispatch instances, each managing its own set of Executors. Each Dispatch operates independently, reading task information from shared memory and distributing tasks to its assigned Executors.

**Why this priority**: Multiple Dispatch instances enable distributed task processing and scale-out parallelism.

**Independent Test**: Can be tested by creating multiple Dispatch instances and verifying each manages its own set of Executors.

**Acceptance Scenarios**:

1. **Given** multiple Dispatch instances are created, **When** each Dispatch is assigned a distinct set of Executors, **Then** each Dispatch manages only its assigned Executors
2. **Given** Dispatch A has Executors {E1, E2} and Dispatch B has Executors {E3, E4}, **When** tasks are submitted, **Then** Dispatch A distributes only to E1/E2, Dispatch B only to E3/E4
3. **Given** a Dispatch manages N Executors, **When** it distributes tasks, **Then** tasks are evenly spread across its N Executors

---

### User Story 4b - Dispatch Worker Thread Executor Assignment (Priority: P1)

A system operator relies on each worker thread within a Dispatch to manage a fixed pool of Executors. Each thread is assigned 60 CUBE-capable Executors and 60 VECTOR-capable Executors, allowing the thread to dispatch CUBE, VECTOR, and MIX tasks to the appropriate Executors.

**Why this priority**: Fixed Executor assignment per thread enables predictable dispatch capacity and allows the system to handle the specific workload mix (CUBE, VECTOR, MIX) efficiently.

**Independent Test**: Can be tested by verifying that each Dispatch worker thread has exactly 60 CUBE Executors and 60 VECTOR Executors under its management.

**Acceptance Scenarios**:

1. **Given** a Dispatch has multiple worker threads, **When** each thread is initialized, **Then** each thread manages exactly 60 CUBE Executors and 60 VECTOR Executors
2. **Given** a worker thread has 60 CUBE Executors and 60 VECTOR Executors available, **When** a CUBE task is dispatched, **Then** the thread routes the task to one of its 60 CUBE Executors
3. **Given** a worker thread has 60 CUBE Executors and 60 VECTOR Executors available, **When** a VECTOR task is dispatched, **Then** the thread routes the task to one of its 60 VECTOR Executors
4. **Given** a worker thread has 60 CUBE Executors and 60 VECTOR Executors available, **When** a MIX task is dispatched, **Then** the thread finds an Executor with both CUBE and VECTOR capabilities idle and dispatches to it
5. **Given** a MIX task is dispatched to a dual-capable Executor, **When** both CUBE and VECTOR sub-tasks complete, **Then** the MIX task transitions to COMPLETED state
6. **Given** a MIX task is dispatched to a dual-capable Executor, **When** either sub-task reports failure, **Then** the MIX task transitions to FAILED state

---

### User Story 6 - Work-Stealing Load Balancing (Priority: P1)

A system operator relies on the Work-Stealing mechanism to balance load across Executors managed by different Dispatches. When one Dispatch's Executors are busy while another's are idle, idle Executors steal work from busy ones.

**Why this priority**: Work-stealing ensures efficient utilization of all Executors across Dispatches and prevents idle waste.

**Independent Test**: Can be tested by creating a scenario where one Dispatch has all Executors busy and another has idle Executors, then verifying work-stealing occurs.

**Acceptance Scenarios**:

1. **Given** Dispatch A has all Executors busy and Dispatch B has idle Executors, **When** Work-Stealing is triggered, **Then** Dispatch B's Executors steal tasks from Dispatch A's queue
2. **Given** Work-Stealing is enabled, **When** an idle Executor requests work, **Then** it steals from the busiest Dispatch's queue
3. **Given** multiple Dispatches participate in Work-Stealing, **When** tasks are stolen, **Then** the stealing respects the task dependency constraints

---

### User Story 7 - Shared Memory Synchronization (Priority: P2)

A system operator needs the Dispatch and Orchestrator to coordinate access to shared memory safely. Both components must synchronize to avoid reading stale data or writing to the same region simultaneously.

**Why this priority**: Proper synchronization ensures data consistency without corruption or lost updates.

**Independent Test**: Can be tested by simulating concurrent access and verifying proper synchronization behavior.

**Acceptance Scenarios**:

1. **Given** the Orchestrator is writing to shared memory, **When** the Dispatch attempts to read, **Then** proper synchronization ensures the Dispatch either waits or reads consistent data
2. **Given** both components need to access shared memory, **When** they coordinate via synchronization primitives, **Then** no data corruption occurs

---

### User Story 7 - Shared Memory Synchronization (Priority: P2)

A system operator needs the Dispatch and Orchestrator to coordinate access to shared memory safely. Both components must synchronize to avoid reading stale data or writing to the same region simultaneously.

**Why this priority**: Proper synchronization ensures data consistency without corruption or lost updates.

**Independent Test**: Can be tested by simulating concurrent access and verifying proper synchronization behavior.

**Acceptance Scenarios**:

1. **Given** the Orchestrator is writing to shared memory, **When** the Dispatch attempts to read, **Then** proper synchronization ensures the Dispatch either waits or reads consistent data
2. **Given** both components need to access shared memory, **When** they coordinate via synchronization primitives, **Then** no data corruption occurs

---

### User Story 8 - Distributed Task Distribution (Priority: P2)

A system operator can scale the system by adding more Dispatch-Executor pairs. The Work-Stealing mechanism automatically balances load across all available Executors regardless of which Dispatch manages them.

**Why this priority**: Elastic scalability enables the system to handle varying workloads efficiently.

**Independent Test**: Can be tested by adding new Dispatch-Executor pairs and verifying Work-Stealing redistributes load.

**Acceptance Scenarios**:

1. **Given** a system with N Dispatch-Executor pairs, **When** a new Dispatch-Executor pair is added, **Then** Work-Stealing includes the new Executors in the steal pool
2. **Given** a new Dispatch joins the Work-Stealing pool, **When** load balancing occurs, **Then** tasks are redistributed across all N+1 Dispatches
3. **Given** a Dispatch is removed from the system, **When** remaining Dispatches balance load, **Then** tasks are redistributed to remaining Executors

---

### User Story 8 - Distributed Task Distribution (Priority: P2)

A system operator can scale the system by adding more Dispatch-Executor pairs. The Work-Stealing mechanism automatically balances load across all available Executors regardless of which Dispatch manages them.

**Why this priority**: Elastic scalability enables the system to handle varying workloads efficiently.

**Independent Test**: Can be tested by adding new Dispatch-Executor pairs and verifying Work-Stealing redistributes load.

**Acceptance Scenarios**:

1. **Given** a system with N Dispatch-Executor pairs, **When** a new Dispatch-Executor pair is added, **Then** Work-Stealing includes the new Executors in the steal pool
2. **Given** a new Dispatch joins the Work-Stealing pool, **When** load balancing occurs, **Then** tasks are redistributed across all N+1 Dispatches
3. **Given** a Dispatch is removed from the system, **When** remaining Dispatches balance load, **Then** tasks are redistributed to remaining Executors

---

### User Story 9 - Dispatch Lifecycle (Priority: P2)

A system operator creates a Dispatch instance connected to shared memory, uses it for task distribution, and shuts it down cleanly. The Dispatch properly detaches from shared memory on shutdown.

**Why this priority**: Proper lifecycle management ensures no resource leaks and clean system shutdown.

**Independent Test**: Can be tested by creating and destroying Dispatch and verifying clean resource release.

**Acceptance Scenarios**:

1. **Given** a Dispatch is created and attached to shared memory, **When** it is destroyed, **Then** all shared memory attachments are released
2. **Given** a Dispatch is shutting down while tasks are in flight, **When** shutdown completes, **Then** no shared memory leaks occur

---

### User Story 9 - Dispatch Lifecycle (Priority: P2)

A system operator creates a Dispatch instance connected to shared memory, uses it for task distribution, and shuts it down cleanly. The Dispatch properly detaches from shared memory on shutdown.

**Why this priority**: Proper lifecycle management ensures no resource leaks and clean system shutdown.

**Independent Test**: Can be tested by creating and destroying Dispatch and verifying clean resource release.

**Acceptance Scenarios**:

1. **Given** a Dispatch is created and attached to shared memory, **When** it is destroyed, **Then** all shared memory attachments are released
2. **Given** a Dispatch is shutting down while tasks are in flight, **When** shutdown completes, **Then** no shared memory leaks occur

---

### User Story 10 - Task Affinity (Priority: P3)

A system operator can optionally configure task affinity rules. Tasks that require specific Executors (due to memory locality, device affinity, or other constraints) are routed to the appropriate Dispatch.

**Why this priority**: Affinity awareness can improve performance for NUMA or accelerator-based workloads.

**Independent Test**: Can be tested by configuring affinity rules and verifying tasks are routed to the correct Dispatch.

**Acceptance Scenarios**:

1. **Given** a task has affinity constraints (e.g., must run on a specific Executor), **When** the task is submitted, **Then** it is routed to the Dispatch managing that Executor
2. **Given** a task has no affinity constraints, **When** Work-Stealing is active, **Then** the task can be stolen by any idle Executor

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The Dispatch MUST read task graph topology from shared memory written by the Orchestrator
- **FR-002**: The Dispatch MUST read ready-tasks information from shared memory
- **FR-003**: The Dispatch MUST distribute tasks to workers without additional data copies beyond what is necessary
- **FR-004**: Multiple Dispatch instances MUST be created, each managing its own set of Executors
- **FR-005**: Each Dispatch MUST distribute tasks only to its assigned Executors under normal load
- **FR-006**: The system MUST implement Work-Stealing so idle Executors can steal from other Dispatches
- **FR-007**: Work-Stealing MUST respect task dependency constraints
- **FR-008**: Load balancing decisions MUST be based on queue depth and Executor availability
- **FR-009**: The Dispatch and Orchestrator MUST synchronize access to shared memory
- **FR-010**: The system MUST support adding and removing Dispatch-Executor pairs dynamically
- **FR-011**: Optional task affinity rules MUST route tasks to specific Dispatches when configured
- **FR-012**: The Dispatch MUST properly detach from shared memory on shutdown
- **FR-013**: The Dispatch MUST read ready tasks from both Orchestrator and Cutter shared memory regions without duplication
- **FR-014**: The Dispatch MUST write taskID and index to shared memory for the Executor to retrieve
- **FR-015**: The Dispatch MUST read a 1-bit completion signal from shared memory to determine when the Executor has completed the task in Slot A
- **FR-016**: Each worker thread within a Dispatch MUST manage exactly 60 CUBE-capable Executors and 60 VECTOR-capable Executors
- **FR-017**: The Dispatch MUST find an idle Executor with both CUBE and VECTOR capabilities before dispatching a MIX task
- **FR-018**: The Dispatch MUST wait if no dual-capable Executor is currently idle when a MIX task is ready to dispatch
- **FR-019**: A MIX task executing on a dual-capable Executor MUST track CUBE and VECTOR sub-task completion independently
- **FR-020**: A MIX task MUST NOT transition to COMPLETED until both its CUBE and VECTOR sub-tasks have completed successfully
- **FR-021**: A MIX task MUST transition to FAILED if either its CUBE or VECTOR sub-task reports failure

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Dispatch reads Orchestrator output from shared memory within 1 microsecond of the data being available
- **SC-002**: Task distribution latency from shared memory read to worker notification is under 10 microseconds
- **SC-003**: Shared memory access is properly synchronized with no data corruption under concurrent access
- **SC-004**: When Dispatch A has 10 pending tasks and Dispatch B has 0, Work-Stealing redistributes tasks achieving <= 5 tasks per Executor on Dispatch B
- **SC-005**: Task redistribution via Work-Stealing completes within 100 microseconds
- **SC-006**: A new Dispatch-Executor pair is included in Work-Stealing within 10 milliseconds of being added
- **SC-007**: Dispatch clean shutdown completes within 1 millisecond with no shared memory leaks
- **SC-008**: Zero-copy distribution is achieved for task data read from shared memory

---

## Assumptions

- Shared memory region is pre-established between Orchestrator and Dispatch (via configuration or startup handshake)
- The Orchestrator writes data in a format that the Dispatch understands (agreed-upon data structure)
- Synchronization uses atomic operations or similar lock-free primitives
- Shared memory persistence is handled by the operating system (no explicit durability requirements)
- Each Dispatch has a unique identifier visible to the Work-Stealing mechanism
- Task dependencies are preserved during Work-Stealing (a task can only be stolen if its predecessors are satisfied)
- The system assumes "Trust the Caller" - affinity constraints are provided correctly
- Work-Stealing steal rate is configurable per Dispatch or globally