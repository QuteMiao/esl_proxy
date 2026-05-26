# Feature Specification: Memory Pool

**Feature Branch**: `007-memory-pool`

**Created**: 2026-05-22

**Status**: Draft

**Input**: User description: "memory预分配内存，支持task执行期间中间数据内存的分配和释放  + memory对orchestrator提供内存分配和注册释放时机的接口 + memory对orchestrator提供when2free(addr, taskID)接口，在所有小于taskID的任务已经执行完时自动释放对应的内存  + 根据task state Ring buffer中的TaskID状态更新最小未完成TaskID + 采用单生产者单消费者的模式"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Pre-allocated Memory Pool (Priority: P1)

A system operator configures a pre-allocated memory pool before task execution begins. The memory pool reserves a fixed amount of memory upfront, allowing tasks to allocate and free intermediate data during execution without triggering costly system calls or memory fragmentation.

**Why this priority**: Pre-allocating memory eliminates dynamic allocation overhead during task execution, ensuring consistent and predictable performance for latency-sensitive workloads.

**Independent Test**: Can be tested by pre-allocating a memory pool, running tasks that allocate/free intermediate data, and verifying no system malloc/free is called during execution.

**Acceptance Scenarios**:

1. **Given** a memory pool is pre-allocated with size M bytes, **When** a task requests temporary memory, **Then** the allocation is served from the pre-allocated pool within microseconds
2. **Given** the memory pool has free space, **When** a task allocates intermediate data, **Then** the allocation succeeds without any system call
3. **Given** the memory pool is fully allocated, **When** a task requests more memory, **Then** the allocation fails gracefully or triggers overflow handling
4. **Given** a task completes its intermediate data usage, **When** it frees the memory, **Then** the freed memory is returned to the pool for reuse

---

### User Story 2 - In-Task Allocation and Deallocation (Priority: P1)

A system operator relies on tasks to allocate intermediate data buffers during execution and release them when no longer needed. The memory pool manages the lifecycle of these temporary allocations, ensuring memory is reclaimed immediately after use.

**Why this priority**: Supporting allocation/deallocation within task execution enables complex multi-stage tasks to use temporary buffers without leaking memory or requiring long-lived allocations.

**Independent Test**: Can be tested by running a task that allocates, uses, and frees intermediate data multiple times within a single execution, and verifying pool utilization remains stable.

**Acceptance Scenarios**:

1. **Given** a task is executing, **When** it allocates a buffer of size N bytes, **Then** the buffer is returned immediately from the available pool
2. **Given** a task allocates multiple intermediate buffers, **When** it finishes with each buffer, **Then** each buffer is individually freed back to the pool
3. **Given** a task allocates a large buffer that exceeds pool remaining capacity, **When** the allocation is attempted, **Then** the system handles the overflow gracefully
4. **Given** multiple tasks execute concurrently using the same pool, **When** they allocate and free independently, **Then** pool allocations remain thread-safe with no corruption

---

### User Story 3 - Zero-Copy Intermediate Data (Priority: P2)

A system operator relies on the memory pool to enable zero-copy data sharing between tasks. Intermediate data buffers allocated from the pool can be passed directly to downstream tasks without copying, as both the producer and consumer reference the same physical memory.

**Why this priority**: Zero-copy intermediate data minimizes memory bandwidth usage and reduces task-to-task data transfer latency, critical for high-throughput pipeline workloads.

**Independent Test**: Can be tested by having a producer task allocate a buffer, write data, pass the buffer reference to a consumer task, and verify the consumer sees the data without any memory copy.

**Acceptance Scenarios**:

1. **Given** a producer task allocates a buffer and writes data, **When** it passes the buffer reference to a consumer task, **Then** the consumer accesses the same physical memory without copying
2. **Given** a consumer task receives a buffer reference, **When** it completes processing, **Then** it must not free the buffer (producer owns the lifecycle)
3. **Given** a buffer is passed between tasks, **When** both tasks are finished with it, **Then** exactly one task is responsible for freeing the buffer to the pool

---

### User Story 4 - Memory Pool Monitoring (Priority: P3)

A system operator monitors the memory pool's utilization to understand memory consumption patterns and plan capacity. The operator can query pool metadata such as total size, allocated bytes, and available bytes.

**Why this priority**: Visibility into pool utilization enables operators to tune pool size and detect memory exhaustion before it impacts task execution.

**Independent Test**: Can be tested by querying pool metadata and verifying the values match actual allocations and frees.

**Acceptance Scenarios**:

1. **Given** a memory pool has been pre-allocated, **When** an operator queries total pool size, **Then** the returned value equals the pre-allocated size
2. **Given** tasks have allocated memory from the pool, **When** an operator queries allocated bytes, **Then** the returned value equals the sum of all active allocations
3. **Given** the pool is near exhaustion, **When** an operator monitors utilization, **Then** the operator can make informed decisions about pool resize or task throttling

---

### User Story 5 - Automatic Memory Release via when2free (Priority: P1)

A system operator relies on the Orchestrator to register memory buffers with a when2free policy. The Orchestrator calls when2free(addr, taskID) to indicate that a specific memory buffer should be automatically freed when all tasks with smaller task IDs have completed execution. This ensures memory is reclaimed precisely when it is no longer needed by any dependent task.

**Why this priority**: Automatic memory release via when2free eliminates manual memory management overhead for the Orchestrator and prevents memory leaks by ensuring buffers are freed exactly when all consumers have finished.

**Independent Test**: Can be tested by allocating a buffer, calling when2free(addr, taskID) with a threshold, running tasks with IDs below the threshold to completion, and verifying the buffer is freed automatically.

**Acceptance Scenarios**:

1. **Given** the Orchestrator registers a buffer with when2free(addr, taskID=T), **When** all tasks with ID < T complete execution, **Then** the registered buffer is automatically freed back to the pool
2. **Given** a buffer is registered with when2free(addr, taskID=T), **When** task T-1 has not yet completed, **Then** the buffer is not freed
3. **Given** the Orchestrator registers multiple buffers with different when2free thresholds, **When** each threshold condition is met, **Then** each corresponding buffer is freed exactly once
4. **Given** a buffer is registered with when2free(addr, taskID=T), **When** no tasks with ID < T ever execute, **Then** the buffer is freed only when the Orchestrator explicitly frees it or when the pool is destroyed

---

### User Story 6 - Track and Update Minimum Uncompleted TaskID (Priority: P1)

A system operator relies on the memory pool to track and report the minimum TaskID among all uncompleted tasks. As tasks complete, the system automatically updates this tracked value, enabling efficient memory reclamation and dependency resolution.

**Why this priority**: Tracking the minimum uncompleted TaskID enables the Orchestrator to make informed decisions about when to release memory, trigger downstream task scheduling, and monitor DAG progress. Without this tracking, the system cannot efficiently determine which tasks remain outstanding.

**Independent Test**: Can be tested by creating tasks with various IDs, tracking which are complete, and verifying the system reports the minimum uncompleted TaskID correctly as tasks complete.

**Acceptance Scenarios**:

1. **Given** a DAG with tasks having IDs 1 through 10, **When** tasks 1-5 are complete and 6-10 are pending, **Then** the system reports the minimum uncompleted TaskID as 6
2. **Given** all tasks are complete, **When** the system queries minimum uncompleted TaskID, **Then** the system returns a sentinel value indicating no uncompleted tasks
3. **Given** tasks complete out of order (e.g., 3 completes before 2), **When** the system updates the minimum, **Then** the tracked value correctly reflects the new minimum uncompleted ID
4. **Given** a new task with ID=0 is submitted, **When** it begins execution, **Then** the system updates the minimum uncompleted TaskID to include the new in-progress task

---

### User Story 7 - Memory Release Based on Minimum Uncompleted TaskID (Priority: P1)

A system operator uses the minimum uncompleted TaskID to trigger memory release decisions. When the minimum uncompleted TaskID advances past a threshold, memory associated with completed tasks can be safely reclaimed.

**Why this priority**: Memory release based on minimum uncompleted TaskID enables the Orchestrator to precisely time when buffers can be freed, ensuring no dependent task still needs the data before reclamation occurs.

**Independent Test**: Can be tested by registering memory with when2free thresholds, advancing the minimum uncompleted TaskID past each threshold, and verifying memory is released at the appropriate times.

**Acceptance Scenarios**:

1. **Given** a buffer is registered with when2free(addr, taskID=5), **When** the minimum uncompleted TaskID advances to 6, **Then** the buffer is automatically freed
2. **Given** multiple buffers are registered with different thresholds, **When** the minimum uncompleted TaskID crosses each threshold, **Then** each buffer is freed exactly once in threshold order
3. **Given** the minimum uncompleted TaskID has not yet reached a buffer's threshold, **When** the system queries for releasable memory, **Then** the buffer remains allocated until its threshold is met

---

### User Story 8 - Update Minimum Uncompleted TaskID from Task State Ring Buffer (Priority: P1)

A system operator relies on the Task State Ring Buffer as an alternative source for tracking task completion status. The Task State Ring Buffer maintains the state of each task ID (pending, running, completed). The system queries the ring buffer to determine which tasks are still uncompleted and updates the minimum uncompleted TaskID accordingly.

**Why this priority**: Having multiple sources for task completion tracking (CompleteQueue and Task State Ring Buffer) provides resilience and allows the memory pool to operate even if the CompleteQueue is temporarily unavailable. The ring buffer provides a direct view of task states without needing to poll a separate queue.

**Independent Test**: Can be tested by querying the Task State Ring Buffer for task states and verifying the minimum uncompleted TaskID is correctly computed based on the ring buffer contents.

**Acceptance Scenarios**:

1. **Given** the Task State Ring Buffer shows tasks 1-5 as completed and 6-10 as pending, **When** the system queries the ring buffer, **Then** the minimum uncompleted TaskID is reported as 6
2. **Given** the Task State Ring Buffer has a task in "running" state at ID=7, **When** the system computes minimum uncompleted, **Then** ID 7 is treated as uncompleted (not yet complete)
3. **Given** the Task State Ring Buffer has no pending or running tasks, **When** the system queries minimum uncompleted, **Then** the system returns a sentinel value indicating no uncompleted tasks
4. **Given** the Task State Ring Buffer is queried periodically, **When** task states change, **Then** the minimum uncompleted TaskID is updated to reflect the current state

---

### User Story 9 - Single Producer Single Consumer Mode (Priority: P1)

A system operator configures the memory pool to operate in Single Producer Single Consumer (SPSC) mode. In this mode, a single designated producer (typically the Orchestrator) is responsible for all memory allocations, and a single designated consumer (typically the Worker or Executor) is responsible for all memory deallocations. This constraint simplifies the implementation and eliminates the need for complex synchronization primitives.

**Why this priority**: SPSC mode provides a well-defined ownership model where the Orchestrator allocates buffers for task inputs and the Worker frees them after task completion. This separation of concerns prevents double-allocation and simplifies memory tracking.

**Independent Test**: Can be tested by configuring the pool in SPSC mode, having a single producer allocate buffers, having a single consumer free buffers, and verifying no allocation conflicts or synchronization issues occur.

**Acceptance Scenarios**:

1. **Given** the memory pool is configured in SPSC mode, **When** the Orchestrator allocates a buffer, **Then** the allocation succeeds from the designated producer role
2. **Given** the memory pool is in SPSC mode, **When** the Worker frees a buffer, **Then** the deallocation succeeds from the designated consumer role
3. **Given** the memory pool is in SPSC mode, **When** an incorrect producer (non-designated) attempts to allocate, **Then** the operation is rejected or handled according to policy
4. **Given** the memory pool is in SPSC mode, **When** an incorrect consumer (non-designated) attempts to free, **Then** the operation is rejected or handled according to policy

---

### User Story 10 - Manager Thread for Automatic Memory Release (Priority: P1)

A system operator relies on a dedicated Manager thread to monitor the minimum uncompleted TaskID and trigger automatic memory release for when2free-registered buffers. The Manager thread continuously monitors the Task State Ring Buffer, updates the minimum uncompleted TaskID, and frees any buffers whose when2free threshold has been reached.

**Why this priority**: The Manager thread decouples memory release from task execution, ensuring buffers are freed precisely when all dependent tasks have completed without requiring explicit coordination between the Orchestrator and Worker during task execution.

**Independent Test**: Can be tested by registering buffers with when2free, having the Manager thread monitor the Task State Ring Buffer, and verifying buffers are freed exactly when the minimum uncompleted TaskID crosses each buffer's threshold.

**Acceptance Scenarios**:

1. **Given** the Manager thread is running, **When** a buffer is registered with when2free(addr, taskID=T), **Then** the Manager thread monitors the buffer for automatic release
2. **Given** the Manager thread detects the minimum uncompleted TaskID has advanced past T, **When** the threshold condition is met, **Then** the Manager thread frees the buffer automatically
3. **Given** multiple buffers are registered with different when2free thresholds, **When** the Manager thread processes them, **Then** each buffer is freed exactly once in threshold order
4. **Given** the Manager thread runs continuously, **When** task states change in the Task State Ring Buffer, **Then** the Manager thread updates the minimum uncompleted TaskID and releases eligible buffers within microseconds

---

### Edge Cases

- What happens when a task is cancelled or fails? Does its ID still count as "uncompleted"?
- How does the system handle task IDs that are not sequential (gaps in ID space)?
- What happens when a new task is submitted with an ID lower than the current minimum uncompleted?
- How does the system behave when all tasks complete simultaneously?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST pre-allocate a memory pool of configurable size before task execution
- **FR-002**: Tasks MUST be able to allocate intermediate data memory from the pool without system calls
- **FR-003**: Tasks MUST be able to free allocated memory back to the pool for reuse
- **FR-004**: The memory pool MUST support single-producer-single-consumer access patterns (one producer, one consumer)
- **FR-005**: The system MUST handle pool exhaustion gracefully (allocation failure signal rather than crash)
- **FR-006**: The system MUST support querying pool metadata (total size, allocated, available)
- **FR-007**: Allocated buffers MUST support zero-copy sharing between producer and consumer tasks
- **FR-008**: The system MUST prevent double-free errors (freeing already-freed buffers)
- **FR-009**: The system SHOULD support pool resize (growing the pre-allocated region) at runtime
- **FR-010**: The memory pool MUST expose allocation and deallocation interfaces to tasks for obtaining and releasing intermediate data buffers
- **FR-011**: The memory pool MUST expose allocation and deallocation interfaces to the Orchestrator for constructing task graphs and managing task input/output data
- **FR-012**: The memory pool MUST expose a when2free(addr, taskID) interface to the Orchestrator that registers a buffer address for automatic release when all tasks with IDs smaller than the specified threshold have completed
- **FR-013**: The system MUST track the minimum TaskID among all uncompleted tasks
- **FR-014**: The system MUST automatically update the minimum uncompleted TaskID when tasks complete
- **FR-015**: The system MUST provide an interface to query the current minimum uncompleted TaskID
- **FR-016**: The system MUST query the Task State Ring Buffer to determine task completion status for minimum uncompleted TaskID computation
- **FR-017**: The system MUST treat tasks in "running" state as uncompleted when computing minimum uncompleted TaskID
- **FR-018**: A dedicated Manager thread MUST continuously monitor the Task State Ring Buffer, update the minimum uncompleted TaskID, and automatically free when2free-registered buffers whose threshold has been reached

### Key Entities *(include if feature involves data)*

- **Memory Pool**: A pre-allocated region of memory from which task intermediate data is allocated. Attributes: total size, allocated bytes, free bytes.
- **Buffer Handle**: A reference to an allocated buffer within the pool. Used by tasks to read/write data and to free the buffer.
- **Allocation Request**: A task's request for a buffer of a specific size. Contains: requested size, returned buffer handle or failure status.
- **Task Completion Tracker**: Component that tracks task completion status and maintains the minimum uncompleted TaskID based on Task State Ring Buffer.
- **Task State Ring Buffer**: Ring buffer that maintains the state of each task ID (pending, running, completed). The authoritative source for determining which tasks are still uncompleted.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Task allocation from the memory pool completes in under 1 microsecond
- **SC-002**: Task deallocation returns memory to the pool in under 1 microsecond
- **SC-003**: Zero-copy buffer passing between tasks introduces no additional copy overhead
- **SC-004**: The memory pool handles at least 10,000 concurrent allocations without corruption
- **SC-005**: Pool metadata queries return accurate values reflecting current utilization
- **SC-006**: Minimum uncompleted TaskID is updated within 1 microsecond of task completion
- **SC-007**: When2free registered buffers are freed within 1 microsecond of threshold being reached
- **SC-008**: Task State Ring Buffer state changes are reflected in minimum uncompleted TaskID within 1 microsecond

## Assumptions

- Memory pool size is configured at system initialization based on workload analysis
- Tasks are trusted to call free exactly once per allocation (Trust the Caller principle applies to lifecycle management)
- The memory pool is private to a single Executor or Dispatch-Executor pair (not shared across processes)
- Fragmentation is managed through a suitable allocation strategy (e.g., slab or pool-based allocator)
- Task intermediate data does not persist beyond task completion (no durability requirements)
- Task IDs are assigned sequentially and monotonically increasing within a DAG execution
- A sentinel value (e.g., UINT16_MAX or special "DONE" marker) indicates no uncompleted tasks
- The Task State Ring Buffer is the authoritative source for task state information
- Task states include: pending, running, completed (or similar states defined in the ring buffer)
- The system queries the Task State Ring Buffer to compute minimum uncompleted TaskID
- CompleteQueue is NOT used for minimum uncompleted TaskID updates - it serves other purposes (e.g., signaling task completion to other components)
- The memory pool operates in Single Producer Single Consumer (SPSC) mode, where a single designated producer (Orchestrator) allocates memory and a single designated consumer (Worker/Executor) frees memory
- A dedicated Manager thread handles when2free-based automatic memory release by monitoring minimum uncompleted TaskID