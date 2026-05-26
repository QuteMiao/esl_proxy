# Feature Specification: Memory Pool

**Feature Branch**: `007-memory-pool`

**Created**: 2026-05-22

**Status**: Draft

**Input**: User description: "memory预分配内存，支持task执行期间中间数据内存的分配和释放 + memory对外提供内存分配和释放接口 + memory对orchestrator提供内存分配和释放接口 + memory对orchestrator提供when2free(addr, taskID)接口，在所有小于taskID的任务已经执行完时自动释放对应的内存"

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

A system operator relies on the Orchestrator to register memory buffers with a when2free policy. The Orchestrator calls when2free(taskID) to indicate that a specific memory buffer should be automatically freed when all tasks with smaller task IDs have completed execution. This ensures memory is reclaimed precisely when it is no longer needed by any dependent task.

**Why this priority**: Automatic memory release via when2free eliminates manual memory management overhead for the Orchestrator and prevents memory leaks by ensuring buffers are freed exactly when all consumers have finished.

**Independent Test**: Can be tested by allocating a buffer, calling when2free(taskID) with a threshold, running tasks with IDs below the threshold to completion, and verifying the buffer is freed automatically.

**Acceptance Scenarios**:

1. **Given** the Orchestrator registers a buffer with when2free(taskID=T), **When** all tasks with ID < T complete execution, **Then** the registered buffer is automatically freed back to the pool
2. **Given** a buffer is registered with when2free(taskID=T), **When** task T-1 has not yet completed, **Then** the buffer is not freed
3. **Given** the Orchestrator registers multiple buffers with different when2free thresholds, **When** each threshold condition is met, **Then** each corresponding buffer is freed exactly once
4. **Given** a buffer is registered with when2free(taskID=T), **When** no tasks with ID < T ever execute, **Then** the buffer is freed only when the Orchestrator explicitly frees it or when the pool is destroyed

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST pre-allocate a memory pool of configurable size before task execution
- **FR-002**: Tasks MUST be able to allocate intermediate data memory from the pool without system calls
- **FR-003**: Tasks MUST be able to free allocated memory back to the pool for reuse
- **FR-004**: Memory pool allocation and deallocation MUST be thread-safe for concurrent task access
- **FR-005**: The system MUST handle pool exhaustion gracefully (allocation failure signal rather than crash)
- **FR-006**: The system MUST support querying pool metadata (total size, allocated, available)
- **FR-007**: Allocated buffers MUST support zero-copy sharing between producer and consumer tasks
- **FR-008**: The system MUST prevent double-free errors (freeing already-freed buffers)
- **FR-009**: The system SHOULD support pool resize (growing the pre-allocated region) at runtime
- **FR-010**: The memory pool MUST expose allocation and deallocation interfaces to tasks for obtaining and releasing intermediate data buffers
- **FR-011**: The memory pool MUST expose allocation and deallocation interfaces to the Orchestrator for constructing task graphs and managing task input/output data
- **FR-012**: The memory pool MUST expose a when2free(addr, taskID) interface to the Orchestrator that registers a buffer address for automatic release when all tasks with IDs smaller than the specified threshold have completed

### Key Entities *(include if feature involves data)*

- **Memory Pool**: A pre-allocated region of memory from which task intermediate data is allocated. Attributes: total size, allocated bytes, free bytes.
- **Buffer Handle**: A reference to an allocated buffer within the pool. Used by tasks to read/write data and to free the buffer.
- **Allocation Request**: A task's request for a buffer of a specific size. Contains: requested size, returned buffer handle or failure status.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Task allocation from the memory pool completes in under 1 microsecond
- **SC-002**: Task deallocation returns memory to the pool in under 1 microsecond
- **SC-003**: Zero-copy buffer passing between tasks introduces no additional copy overhead
- **SC-004**: The memory pool handles at least 10,000 concurrent allocations without corruption
- **SC-005**: Pool metadata queries return accurate values reflecting current utilization

## Assumptions

- Memory pool size is configured at system initialization based on workload analysis
- Tasks are trusted to call free exactly once per allocation (Trust the Caller principle applies to lifecycle management)
- The memory pool is private to a single Executor or Dispatch-Executor pair (not shared across processes)
- Fragmentation is managed through a suitable allocation strategy (e.g., slab or pool-based allocator)
- Task intermediate data does not persist beyond task completion (no durability requirements)