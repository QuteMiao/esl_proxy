# Quickstart: Memory Pool Integration

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-27

## Basic Usage

### 1. Initialize Memory Pool

```c
// Configure pool at system init
mem_pool_config_t config = {
    .pool_size = 64 * 1024 * 1024,  // 64MB
    .slot_size = 4096,               // 4KB slots
};
mem_pool_init(&config);
```

### 2. Orchestrator Allocates Buffer (Producer)

```c
// Orchestrator allocates buffer via ring buffer tail
size_t slot_index = mem_pool_alloc(1024);
if (slot_index == INVALID_SLOT) {
    // Handle pool exhaustion
}

// Register for automatic release
mem_pool_when2free(slot_index, target_task_id);
```

### 3. Worker Uses Buffer (Consumer)

```c
// Worker uses buffer
void *buffer = mem_pool_addr(slot_index);

// After task completion, Manager thread frees via ring buffer head
mem_pool_free(slot_index);
```

### 4. Manager Thread (when2free Release)

```c
// Manager thread runs continuously
while (running) {
    uint32_t min_id = task_state_ring_buffer_min_uncompleted();
    mem_pool_process_when2free(min_id);
    // Poll interval: typically 1-10μs
}
```

## Ring Buffer Head/Tail Flow

```
Producer (Orchestrator)          Consumer (Manager/Worker)
      |                                  |
      v                                  v
   tail = 0                           head = 0
      |                                  |
      |-- alloc --> slot[0] -- free --> |
      |                                  |
   tail = 1                           head = 1
      |                                  |
      |-- alloc --> slot[1] -- free --> |
      |                                  |
   tail = 2                           head = 2
```

## Test Scenarios

### Scenario 1: Basic Alloc/Free via Ring Buffer

```
1. Init pool (1MB, 256 slots of 4KB)
2. tail = 0, head = 0
3. alloc() → returns slot 0, tail = 1
4. free(slot 0) → head = 1
5. Verify ring buffer state: head=1, tail=1 (balanced)
```

### Scenario 2: Ring Buffer Wraparound

```
1. Pool with 4 slots
2. alloc × 4 → tail: 0→1→2→3→4 (wrap to 0)
3. Verify tail % 4 == 0
4. free × 4 → head: 0→1→2→3→4 (wrap to 0)
5. Verify head % 4 == 0
```

### Scenario 3: when2free Automatic Release

```
1. Init pool
2. alloc() → slot 0, register when2free(slot=0, task_id=5)
3. Task 3 completes → min_uncompleted = 4
4. Task 4 completes → min_uncompleted = 5 (slot NOT freed, 5 is threshold)
5. Task 5 completes → min_uncompleted = 6 → slot freed via head++
```

### Scenario 4: SPSC Mode Enforcement

```
1. Init pool in SPSC mode
2. Producer (Orchestrator) calls alloc() → tail++ → slot allocated
3. Consumer (Worker/Manager) calls free() → head++ → slot freed
4. Verify: only producer modifies tail, only consumer modifies head
```

### Scenario 5: Pool Exhaustion

```
1. Init pool (1KB total, 2 slots of 500 bytes)
2. tail = 0: alloc → slot 0, tail = 1
3. tail = 1: alloc → slot 1, tail = 2
4. tail = 2 % 2 = 0 == head? Pool full
5. free(slot 0) → head = 1
6. alloc → slot 1, tail = 0
```