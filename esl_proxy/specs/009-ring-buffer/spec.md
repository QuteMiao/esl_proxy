# Feature Specification: Task Ring Buffers

**Feature Branch**: `009-ring-buffer`

**Created**: 2026-05-26

**Status**: Draft

**Input**: User description: "4个ring buffer分别存储task状态，task基本信息，task后继节点信息，task运行时信息" + "ring buffer大小默认设置为4096，task通过taskID & RingBufferSize决定存储位置"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Ring Buffer Size Configuration (Priority: P1)

A system operator configures the Ring Buffer with a fixed size of 4096 entries. The size is a power of 2, enabling efficient bitwise operations for indexing.

**Why this priority**: Ring Buffer size must be fixed and power of 2 for correct and efficient indexing operation using bitwise AND.

**Independent Test**: Can be verified by computing index for various TaskID values and confirming correct wraparound behavior.

**Acceptance Scenarios**:

1. **Given** Ring Buffer size is 4096 (2^12), **When** the size is used in indexing, **Then** it enables efficient bitmask operation instead of modulo division
2. **Given** Ring Buffer has 4096 entries, **When** TaskID values exceed 4096, **Then** the index wraps around via bitwise AND

---

### User Story 2 - TaskID-Based Indexing (Priority: P1)

A system operator stores task data using TaskID as the index into the Ring Buffer. The index is computed as `TaskID & (RingBufferSize - 1)`, providing O(1) access to any task entry.

**Why this priority**: TaskID-based indexing provides constant-time access to task data without searching or hashing.

**Independent Test**: Can be tested by computing index for known TaskID values and verifying correct storage/retrieval location.

**Acceptance Scenarios**:

1. **Given** TaskID is 5000 and RingBufferSize is 4096, **When** index is computed as 5000 & 4095, **Then** index equals 904
2. **Given** TaskID is 4096 and RingBufferSize is 4096, **When** index is computed as 4096 & 4095, **Then** index equals 0 (wraps to start)
3. **Given** TaskID is 0, **When** index is computed, **Then** index equals 0

---

### User Story 3 - O(1) Access Performance (Priority: P1)

A system operator accesses task data in constant time regardless of TaskID value. The bitwise AND operation completes in a single CPU cycle, ensuring minimal latency for Ring Buffer access.

**Why this priority**: O(1) access is critical for high-performance DAG scheduling where task lookups happen on every scheduling decision.

**Independent Test**: Can be verified by measuring access time for different TaskID values and confirming consistent latency.

**Acceptance Scenarios**:

1. **Given** any valid TaskID value, **When** index is computed via bitwise AND, **Then** access time is constant (O(1))
2. **Given** Ring Buffer is used for 10,000 tasks, **When** any task is accessed, **Then** access latency remains the same regardless of task count

---

### User Story 4 - Task State Storage (Priority: P1)

A system operator stores task execution state in a dedicated Ring Buffer. The state Ring Buffer holds task state transitions (e.g., pending, running, completed, failed) indexed by TaskID. State updates are lock-free using C11 atomics.

**Why this priority**: Task state tracking is essential for DAG execution - the system must know which tasks are running, completed, or failed to make scheduling decisions.

**Independent Test**: Can be tested by writing a task state to the state Ring Buffer and reading it back to verify correctness.

**Acceptance Scenarios**:

1. **Given** a task with TaskID 42 is in pending state, **When** the state is written to the state Ring Buffer at index (42 & RING_MASK), **Then** the same state can be read back from the same index
2. **Given** multiple tasks updating state concurrently, **When** each task writes its state to the Ring Buffer, **Then** no state data is lost or corrupted due to concurrent access

---

### User Story 5 - Task Basic Information Storage (Priority: P1)

A system operator stores task basic information in a dedicated Ring Buffer. The basic info Ring Buffer holds task descriptors (id, type, mode, kernel, index, count, prio, data) indexed by TaskID.

**Why this priority**: The basic info Ring Buffer provides O(1) access to task description data via TaskID indexing.

**Independent Test**: Can be tested by writing a task descriptor to the basic info Ring Buffer and reading it back.

**Acceptance Scenarios**:

1. **Given** a task descriptor with id=1, type=CUBE, mode=SINGLE, **When** it is written to the basic info Ring Buffer at index (1 & RING_MASK), **Then** the same descriptor can be read back from the same index
2. **Given** the Ring Buffer size is 4096, **When** TaskID 5000 is used, **Then** the actual index is (5000 & 4095) = 904

---

### User Story 6 - Task Dependency Information Storage (Priority: P1)

A system operator stores task dependency information in a dedicated Ring Buffer. The Dependency Information Ring Buffer holds successor count, list of successor TaskIDs, and predecessor count for each task node.

**Why this priority**: DAG dependency resolution requires knowing which tasks depend on which. The successor information enables downstream traversal.

**Independent Test**: Can be tested by writing successor info for a task with 3 successors and reading it back.

**Acceptance Scenarios**:

1. **Given** a task with TaskID 10 has 3 successors (IDs 20, 30, 40) and 2 predecessors, **When** the dependency info is written to the Dependency Information Ring Buffer at index (10 & RING_MASK), **Then** the successor count=3, successor list [20, 30, 40], and predecessor count=2 can be read back
2. **Given** a task with no successors and no predecessors, **When** the dependency info is written, **Then** successor count=0, empty successor list, and predecessor count=0

---

### User Story 7 - Task Runtime Information Storage (Priority: P1)

A system operator stores task runtime information in a dedicated Ring Buffer. The runtime info Ring Buffer holds per-task execution context that changes during execution (e.g., current index for SPMD, instance count, worker assignment).

**Why this priority**: Runtime information is mutable state that changes during task execution. Separating runtime info from the static task descriptor allows descriptor reuse.

**Independent Test**: Can be tested by writing runtime context to the runtime Ring Buffer and reading it back after modification.

**Acceptance Scenarios**:

1. **Given** an SPMD task with base index 100 running 8 instances, **When** instance 5 updates its current index to 105, **Then** the runtime info reflects index=105 for that instance
2. **Given** runtime info is stored separately from task descriptor, **When** the task descriptor is reused for a new execution, **Then** the previous runtime info does not persist

---

### User Story 8 - Conditional State Insert (Priority: P1)

A system operator inserts a task state into the state Ring Buffer only if the target entry is currently empty. If the entry already contains a value, the insert operation fails and returns an error without modifying the existing value.

**Why this priority**: Prevents overwriting existing state data, ensuring data integrity in the Ring Buffer. Critical for preventing accidental data loss during concurrent access.

**Independent Test**: Can be tested by attempting to insert into empty and non-empty entries and verifying correct return values and behavior.

**Acceptance Scenarios**:

1. **Given** a state Ring Buffer entry at index is empty, **When** an insert operation is performed, **Then** the value is stored successfully and success is returned
2. **Given** a state Ring Buffer entry at index contains a value, **When** an insert operation is performed, **Then** the operation fails and an error is returned without modifying the existing value

---

### User Story 9 - Task State Tracking (Priority: P1)

A system operator tracks task execution state via the Task State Ring Buffer. The state enum (EMPTY, PENDING, RUNNING, COMPLETED) is defined in the task feature, and the minimum uncompleted TaskID is computed by scanning the state ring buffer.

**Why this priority**: Task state tracking enables the memory pool's when2free mechanism to know when all tasks with ID less than a threshold have completed.

**Independent Test**: Can be tested by setting task states and verifying ring_min_uncompleted() returns the correct minimum uncompleted TaskID.

**Acceptance Scenarios**:

1. **Given** tasks 1-5 are COMPLETED and tasks 6-10 are PENDING/RUNNING, **When** ring_min_uncompleted() is called, **Then** it returns 6
2. **Given** all tasks are COMPLETED or EMPTY, **When** ring_min_uncompleted() is called, **Then** it returns UINT32_MAX (sentinel)
3. **Given** a task ID, **When** task_state_set(task_id, TASK_STATE_RUNNING) is called, **Then** task_state_get(task_id) returns TASK_STATE_RUNNING


---

### User Story 10 - Task State Ring Buffer Interface (Priority: P1)

A system operator interacts with the Task State Ring Buffer through functions defined in the task feature. The interface includes task_state_get(task_id), task_state_set(task_id, state), and ring_min_uncompleted().

**Why this priority**: Having a consistent interface to the Task State Ring Buffer allows both task management and memory pool components to use the same functions without duplicating logic.

**Independent Test**: Can be tested by calling task_state_get/set and verifying the ring buffer contains the correct state values.

**Acceptance Scenarios**:

1. **Given** a task ID, **When** task_state_set(task_id, TASK_STATE_RUNNING) is called, **Then** task_state_get(task_id) returns TASK_STATE_RUNNING
2. **Given** multiple task states are set, **When** ring_min_uncompleted() is called, **Then** it returns the smallest uncompleted TaskID


---

### Edge Cases

- What happens when Ring Buffer is full and new data is written? (Wrapped overwrite behavior)
- What happens when invalid TaskID (e.g., out of range) is used for indexing?
- What happens when successor count exceeds maximum (3)? (Extension entry used via next pointer)
- What happens when TaskID is at maximum value (65535)?
- What happens when RingBufferSize is changed (not power of 2)?
- What happens when multiple concurrent insert operations target the same empty entry?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Ring Buffer size MUST default to 4096 entries
- **FR-002**: Ring Buffer size MUST be a power of 2
- **FR-003**: Task storage location MUST be computed as `TaskID & (RingBufferSize - 1)`
- **FR-004**: Index computation MUST complete in O(1) time via single bitwise AND operation
- **FR-005**: TaskID MUST be 16-bit unsigned integer (0-65535)
- **FR-006**: A Ring Buffer MUST be used for each of the 4 data categories: task state, task basic info, task successor nodes, task runtime info
- **FR-007**: All 4 Ring Buffers MUST have fixed size of 4096 entries (power of 2)
- **FR-008**: Ring Buffer indexing MUST use TASKID & (RING_SIZE - 1) for O(1) access
- **FR-009**: Task state Ring Buffer MUST store task execution state (pending, running, completed, failed)
- **FR-010**: Task basic info Ring Buffer MUST store task descriptor fields (id, type, mode, kernel, index, count, prio, data)
- **FR-011**: Dependency Information Ring Buffer MUST store successor count, list of successor TaskIDs, and predecessor count; successor storage in base entries with 3 inline successor slots; base entries use a 2-byte next pointer to reference extension entries when more than 3 successors exist
- **FR-012**: Task runtime info Ring Buffer MUST store mutable runtime context (current index, instance state, worker assignment)
- **FR-013**: Ring Buffer operations MUST be lock-free using C11 atomics for concurrent access
- **FR-014**: Ring Buffers MUST support single-producer-single-consumer (SPSC) patterns
- **FR-015**: State buffer insert operation MUST check if target entry is empty before writing
- **FR-016**: If target entry is non-empty during insert, operation MUST fail and return error code without modifying existing value
- **FR-017**: If target entry is empty during insert, operation MUST store value and return success
- **FR-018**: Insert operation MUST be atomic to prevent race conditions

### Key Entities *(include if feature involves data)*

- **Ring Buffer**: A fixed-size circular buffer providing O(1) indexed access. Size is power of 2 (4096) enabling fast modulo via bitmask.
- **RingBufferSize**: Fixed constant defining Ring Buffer entry count. Default value is 4096 (power of 2).
- **RingBufferMask**: Computed as (RingBufferSize - 1), used for bitwise AND indexing.
- **TaskID**: 16-bit identifier used for Ring Buffer indexing via TASKID & (RING_SIZE - 1)
- **Index**: Computed value determining actual storage location in Ring Buffer.
- **Task State Ring Buffer**: Stores execution state enum values (pending, running, completed, failed) indexed by TaskID
- **Task Basic Info Ring Buffer**: Stores task descriptor copies indexed by TaskID
- **Dependency Information Ring Buffer**: Stores dependency information indexed by TaskID. Contains: successor count, list of successor TaskIDs, predecessor count. Base entry contains 3 inline successor TaskIDs (2 bytes each) plus a 2-byte next pointer to extension entry. Extension entries linked via next pointer for tasks with more than 3 successors.
- **Task Runtime Info Ring Buffer**: Stores mutable runtime context per task instance indexed by TaskID

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Index computation uses single bitwise AND operation - no division or modulo
- **SC-002**: Index always falls within [0, RingBufferSize-1] range
- **SC-003**: TaskID 4096 maps to index 0 (first wraparound)
- **SC-004**: RingBufferSize 4096 enables efficient 12-bit mask operation
- **SC-005**: All 4 Ring Buffers can store and retrieve data for 4096 distinct TaskIDs
- **SC-006**: Concurrent Ring Buffer writes do not cause data corruption (verified via lock-free atomics)
- **SC-007**: Task successor storage supports up to 3 successors inline in base entry; additional successors via 2-byte next pointer to extension entries
- **SC-008**: Insert into empty state buffer entry succeeds 100% of the time
- **SC-009**: Insert into non-empty state buffer entry fails 100% of the time without modifying existing value
- **SC-010**: No race conditions occur during concurrent insert operations

## Assumptions

- Ring Buffer size (4096) is fixed and power of 2 for efficient bitmask indexing
- TASKID is 16-bit, allowing values 0-65535 with wraparound via bitmask
- Bitwise AND with (RingBufferSize-1) provides same result as modulo RingBufferSize when size is power of 2
- Successor count per node: base entry stores up to 3 inline; extension entries via 2-byte next pointer for additional successors
- Ring Buffers use wraparound behavior - new writes overwrite oldest data when full
- Lock-free operations use C11 atomics - no mutexes in hot path
- Each Ring Buffer is independent and stores different data categories
- 4096 entries is sufficient for the DAG scale (up to 10,000 tasks with TaskID reuse via wraparound)
- State buffer insert returns integer result (0 for success, negative for error)
- State values are integers (e.g., pending=0, running=1, completed=2, failed=3)
