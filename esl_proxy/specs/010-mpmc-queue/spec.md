# Feature Specification: MPMC Queue

**Feature Branch**: `010-mpmc-queue`

**Created**: 2026-05-26

**Status**: Draft

**Input**: User description: "mpmc队列" and "010-mpmc-queue创建各种任务类型的全局ReadyQueue，用于任务下发"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Task Dispatch via MPMC Queue (Priority: P1)

A system operator dispatches tasks to workers through a lock-free MPMC (Multi-Producer-Multi-Consumer) queue. Multiple producer threads can enqueue tasks concurrently while multiple consumer threads dequeue tasks independently, with all operations remaining lock-free.

**Why this priority**: The MPMC queue is the primary mechanism for distributing tasks from the DAG scheduler to worker threads. Without it, tasks cannot be dispatched to workers.

**Independent Test**: Can be verified by having multiple producer threads enqueue tasks and multiple consumer threads dequeue those tasks concurrently, confirming all tasks are delivered without loss or corruption.

**Acceptance Scenarios**:

1. **Given** a queue with 1000 tasks enqueued, **When** 4 consumer threads dequeue concurrently, **Then** all 1000 tasks are successfully dequeued
2. **Given** a queue is empty, **When** a consumer attempts to dequeue, **Then** the operation returns a signal indicating queue is empty
3. **Given** a queue is full, **When** a producer attempts to enqueue, **Then** the operation returns a signal indicating queue is full

---

### User Story 2 - Bounded Queue with Backpressure (Priority: P1)

A system operator configures the MPMC queue with a fixed capacity to apply backpressure when producers outpace consumers. When the queue reaches capacity, producers block or return an error rather than unbounded memory growth.

**Why this priority**: Bounded queues prevent memory exhaustion and provide flow control between producers and consumers in the DAG scheduling pipeline.

**Independent Test**: Can be verified by attempting to enqueue more items than queue capacity and confirming appropriate backpressure behavior.

**Acceptance Scenarios**:

1. **Given** a queue with capacity 100, **When** 100 items are enqueued, **Then** the queue is full and subsequent enqueues return full status
2. **Given** a queue with capacity 100 is full, **When** a consumer dequeues an item, **Then** the queue has room for one more item

---

### User Story 3 - FIFO Ordering (Priority: P2)

A system operator expects tasks to be delivered to consumers in First-In-First-Out order. Tasks enqueued first are dequeued first, ensuring deterministic task ordering within the DAG execution flow.

**Why this priority**: FIFO ordering is critical for DAG scheduling where task dependencies require respecting the order in which tasks were enqueued.

**Independent Test**: Can be verified by enqueuing tasks with sequential identifiers and confirming they dequeue in the same order.

**Acceptance Scenarios**:

1. **Given** tasks with IDs 1, 2, 3, 4, 5 are enqueued in order, **When** 5 consumers each dequeue one task, **Then** they receive tasks in order 1, 2, 3, 4, 5
2. **Given** tasks with IDs 1, 2, 3 are enqueued concurrently by multiple producers, **When** tasks are dequeued, **Then** the relative ordering of concurrent enqueues is non-deterministic but each individual task is delivered exactly once

---

### User Story 4 - Non-Blocking Dequeue Option (Priority: P2)

A system operator can attempt to dequeue a task without blocking. If the queue is empty, the dequeue operation returns immediately with an empty status rather than waiting.

**Why this priority**: Non-blocking dequeue is essential for worker threads that need to check for work without being stuck in a wait state, enabling efficient polling patterns.

**Independent Test**: Can be verified by attempting to dequeue from an empty queue and confirming immediate return with empty status.

**Acceptance Scenarios**:

1. **Given** an empty queue, **When** a non-blocking dequeue is attempted, **Then** the operation returns immediately with empty status
2. **Given** a queue with items, **When** a non-blocking dequeue is attempted, **Then** an item is returned without waiting

---

### User Story 5 - Memory-Efficient Circular Implementation (Priority: P3)

A system operator leverages a circular buffer implementation for the MPMC queue that reuses memory slots. The queue size remains fixed regardless of enqueue/dequeue operations, enabling predictable memory footprint.

**Why this priority**: Circular buffer implementation provides O(1) enqueue and dequeue with minimal memory overhead, critical for high-throughput DAG scheduling.

**Independent Test**: Can be verified by monitoring memory usage during sustained enqueue/dequeue operations and confirming stable memory footprint.

**Acceptance Scenarios**:

1. **Given** a circular buffer queue with capacity 100, **When** 10,000 items are enqueued and dequeued over time, **Then** memory usage remains constant at approximately 100 slots
2. **Given** a circular buffer queue, **When** the producer wraps around to the beginning of the buffer, **Then** it can overwrite slots that have been fully consumed

---

### User Story 6 - Batch Enqueue (Priority: P2)

A system operator enqueues multiple items in a single batch operation, reducing per-item overhead and improving throughput when dispatching groups of related tasks to the queue.

**Why this priority**: Batch enqueue reduces syscall and atomic operation overhead when dispatching multiple tasks, critical for high-throughput DAG scheduling where groups of dependent tasks are often enqueued together.

**Independent Test**: Can be verified by enqueuing a batch of 10 items in a single operation and confirming all 10 items are in the queue with correct order.

**Acceptance Scenarios**:

1. **Given** a queue with capacity 100 and 90 items, **When** a batch of 10 items is enqueued, **Then** all 10 items are successfully added and queue is full
2. **Given** a queue with capacity 100 and 95 items, **When** a batch of 10 items is enqueued, **Then** the operation succeeds for 5 items and returns partial batch status for the remaining 5
3. **Given** a batch of items [1, 2, 3, 4, 5] is enqueued together, **When** items are dequeued, **Then** items maintain relative order [1, 2, 3, 4, 5]

---

### User Story 7 - Batch Dequeue (Priority: P2)

A system operator dequeues multiple items in a single batch operation, allowing workers to fetch multiple waiting tasks at once and reduce per-item coordination overhead.

**Why this priority**: Batch dequeue enables workers to process multiple tasks with a single queue interaction, reducing contention and improving cache locality for sequential task processing.

**Independent Test**: Can be verified by enqueuing 100 items and dequeuing a batch of 10, confirming 10 items are returned and queue depth decreases by 10.

**Acceptance Scenarios**:

1. **Given** a queue with 100 items, **When** a batch dequeue of 10 is attempted, **Then** 10 items are returned and queue has 90 remaining
2. **Given** a queue with 5 items, **When** a batch dequeue of 10 is attempted, **Then** 5 items are returned and queue is empty
3. **Given** a queue with 5 items, **When** a batch dequeue of 10 is attempted in non-blocking mode, **Then** 5 items are returned immediately

---

### User Story 8 - Batch Size Limits and Partial Results (Priority: P2)

A system operator configures maximum batch sizes and handles partial batch results when fewer items than requested are available. The batch API returns the actual number of items processed.

**Why this priority**: Partial batch handling is essential for bounded queues where the number of available items may be less than the requested batch size, enabling graceful degradation without blocking.

**Independent Test**: Can be verified by requesting more items than available and confirming partial results are returned with accurate count.

**Acceptance Scenarios**:

1. **Given** a queue with 3 items, **When** a batch dequeue of 10 is requested, **Then** exactly 3 items are returned with batch count of 3
2. **Given** a queue with 3 items, **When** a batch enqueue of 10 is attempted with 7 items needing to wait, **Then** 3 items are enqueued with batch count of 3

---

### User Story 9 - Per-TaskType ReadyQueues (Priority: P1)

A system operator maintains separate ReadyQueues for each task type (CUBE, VECTOR, MIX). The DAG scheduler enqueues tasks into the appropriate type-specific queue based on the task descriptor type field. Workers consume from their type-specific queue, enabling type-aware task distribution.

**Why this priority**: Different task types may require different worker resources or scheduling policies. Separate queues per task type enable efficient type-aware task distribution and load balancing.

**Independent Test**: Can be verified by enqueueing tasks of different types into their respective queues and confirming workers receive tasks matching their type.

**Acceptance Scenarios**:

1. **Given** task type CUBE with 5 tasks enqueued, **When** workers consume from CUBE queue, **Then** all 5 CUBE tasks are received
2. **Given** task type VECTOR with 3 tasks and MIX with 2 tasks, **When** workers consume from each queue, **Then** VECTOR workers receive only VECTOR tasks, MIX workers receive only MIX tasks
3. **Given** task types CUBE, VECTOR, MIX each have tasks pending, **When** scheduler enqueues new tasks, **Then** each task is placed in the queue matching its type

---

### User Story 10 - Global ReadyQueue Access (Priority: P1)

A system operator accesses global ReadyQueues by task type. The global queues are globally visible and accessible via the task type index, enabling O(1) lookup of the appropriate queue for any task.

**Why this priority**: Global visibility enables any component (scheduler, workers) to access the correct queue without passing queue references. Task type indexing provides O(1) access.

**Independent Test**: Can be verified by looking up the queue for each task type and confirming correct queue is returned.

**Acceptance Scenarios**:

1. **Given** task type CUBE, **When** the scheduler accesses the CUBE ReadyQueue, **Then** the correct queue is found via type-based indexing
2. **Given** task types CUBE, VECTOR, MIX, **When** any component needs to enqueue a task, **Then** it can directly access the appropriate queue via task type
3. **Given** the system has 3 task types, **When** looking up any ReadyQueue, **Then** lookup completes in O(1) time

---

### Edge Cases

- What happens when multiple producers enqueue concurrently at the same slot?
- What happens when the queue is empty and multiple consumers try to dequeue simultaneously?
- What happens when producers enqueue faster than consumers can handle (head-of-line blocking)?
- What happens when the queue wraps around multiple times?
- What happens when task data contains pointers that become invalid after dequeue?
- What happens when batch enqueue attempts more items than remaining capacity?
- What happens when batch dequeue requests more items than are available?
- What is the maximum batch size supported?
- What happens when a task type is added or removed from the system?
- What happens when all ReadyQueues are empty (no work available)?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The queue MUST support multiple producers enqueueing concurrently without locks
- **FR-002**: The queue MUST support multiple consumers dequeuing concurrently without locks
- **FR-003**: The queue MUST be bounded with configurable capacity
- **FR-004**: Enqueue operation MUST return success when queue has capacity
- **FR-005**: Enqueue operation MUST return full/error status when queue is at capacity
- **FR-006**: Dequeue operation MUST return an item when queue has items
- **FR-007**: Dequeue operation MUST return empty status when queue is empty
- **FR-008**: The queue MUST provide FIFO ordering for items enqueued by the same producer
- **FR-009**: Each item MUST be delivered to exactly one consumer (no duplication)
- **FR-010**: The queue MUST use a circular buffer for memory-efficient operation
- **FR-011**: The queue MUST support non-blocking dequeue operation
- **FR-012**: The queue MUST support batch enqueue of multiple items in a single operation
- **FR-013**: Batch enqueue MUST return the count of items successfully enqueued
- **FR-014**: Batch enqueue MUST fail for items that would exceed capacity (partial batch handling)
- **FR-015**: The queue MUST support batch dequeue of multiple items in a single operation
- **FR-016**: Batch dequeue MUST return the count of items actually dequeued
- **FR-017**: Batch dequeue MUST return partial results when fewer items than requested are available
- **FR-018**: A separate ReadyQueue MUST exist for each task type (CUBE, VECTOR, MIX)
- **FR-019**: ReadyQueues MUST be globally visible and accessible by task type
- **FR-020**: ReadyQueue lookup by task type MUST complete in O(1) time
- **FR-021**: Tasks MUST be enqueued into the queue matching their type field

### Key Entities *(include if feature involves data)*

- **MPMC Queue**: A bounded lock-free queue enabling multiple producers and consumers to operate concurrently. Stores task descriptors or pointers to task data.
- **Queue Capacity**: Fixed maximum number of items the queue can hold at once. Configured at queue creation time.
- **Enqueue Operation**: Add an item to the tail of the queue. Succeeds if queue is not full, returns error if queue is full.
- **Dequeue Operation**: Remove and return an item from the head of the queue. Succeeds if queue is not empty, returns empty signal if queue is empty.
- **Queue Slot**: Individual memory location within the circular buffer that holds one queue item.
- **Batch Enqueue**: Operation that adds multiple items to the queue in a single call. Returns count of items successfully enqueued.
- **Batch Dequeue**: Operation that removes multiple items from the queue in a single call. Returns count of items actually dequeued.
- **Batch Count**: Return value indicating how many items were processed in a batch operation.
- **ReadyQueue**: Per-task-type MPMC queue for holding tasks ready to be dispatched to workers.
- **TaskType**: Classification of task execution model (CUBE, VECTOR, MIX). Used to index into ReadyQueue array.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: System supports at least 4 producer threads enqueueing concurrently
- **SC-002**: System supports at least 4 consumer threads dequeuing concurrently
- **SC-003**: Enqueue and dequeue operations complete in O(1) time
- **SC-004**: No item is lost or duplicated during concurrent operations
- **SC-005**: Queue memory footprint remains constant regardless of total items processed
- **SC-006**: Non-blocking dequeue returns within 1 microsecond when queue is empty
- **SC-007**: Bounded queue correctly applies backpressure when at capacity
- **SC-008**: Batch enqueue processes at least 10 items in a single operation
- **SC-009**: Batch dequeue processes at least 10 items in a single operation
- **SC-010**: Partial batch results accurately report actual item count
- **SC-011**: System maintains at least 3 ReadyQueues (one per task type)
- **SC-012**: ReadyQueue access by task type completes in O(1) time
- **SC-013**: Tasks are correctly routed to type-specific queues

## Assumptions

- MPMC queue is used for inter-thread task dispatch within the DAG scheduler
- Queue capacity will be configured based on expected workload (typical values 100-10000)
- Task data stored in the queue is either value types or valid pointers that remain stable until dequeue
- The MPMC queue complements the existing ring buffer infrastructure - ring buffers store task metadata, MPMC queue dispatches tasks to workers
- Lock-free implementation uses C11 atomics only (no mutexes/spinlocks in hot path)
- Queue follows Constitution XI naming conventions (no dag_ prefix)
- Task types are fixed at compile time (CUBE, VECTOR, MIX)
- There are exactly 3 task types, so 3 global ReadyQueues are needed