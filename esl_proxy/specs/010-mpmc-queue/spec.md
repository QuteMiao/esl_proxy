# Feature Specification: MPMC Queue

**Feature Branch**: `010-mpmc-queue`

**Created**: 2026-05-26

**Status**: Draft

**Input**: User description: "mpmc队列"

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

### Edge Cases

- What happens when multiple producers enqueue concurrently at the same slot?
- What happens when the queue is empty and multiple consumers try to dequeue simultaneously?
- What happens when producers enqueue faster than consumers can handle (head-of-line blocking)?
- What happens when the queue wraps around multiple times?
- What happens when task data contains pointers that become invalid after dequeue?

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

### Key Entities *(include if feature involves data)*

- **MPMC Queue**: A bounded lock-free queue enabling multiple producers and consumers to operate concurrently. Stores task descriptors or pointers to task data.
- **Queue Capacity**: Fixed maximum number of items the queue can hold at once. Configured at queue creation time.
- **Enqueue Operation**: Add an item to the tail of the queue. Succeeds if queue is not full, returns error if queue is full.
- **Dequeue Operation**: Remove and return an item from the head of the queue. Succeeds if queue is not empty, returns empty signal if queue is empty.
- **Queue Slot**: Individual memory location within the circular buffer that holds one queue item.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: System supports at least 4 producer threads enqueueing concurrently
- **SC-002**: System supports at least 4 consumer threads dequeuing concurrently
- **SC-003**: Enqueue and dequeue operations complete in O(1) time
- **SC-004**: No item is lost or duplicated during concurrent operations
- **SC-005**: Queue memory footprint remains constant regardless of total items processed
- **SC-006**: Non-blocking dequeue returns within 1 microsecond when queue is empty
- **SC-007**: Bounded queue correctly applies backpressure when at capacity

## Assumptions

- MPMC queue is used for inter-thread task dispatch within the DAG scheduler
- Queue capacity will be configured based on expected workload (typical values 100-10000)
- Task data stored in the queue is either value types or valid pointers that remain stable until dequeue
- The MPMC queue complements the existing ring buffer infrastructure - ring buffers store task metadata, MPMC queue dispatches tasks to workers
- Lock-free implementation uses C11 atomics only (no mutexes/spinlocks in hot path)
- Queue follows Constitution XI naming conventions (no dag_ prefix)