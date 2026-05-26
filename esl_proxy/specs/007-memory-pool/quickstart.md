# Quickstart: Memory Pool Integration

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-26

## Basic Usage

### 1. Initialize Memory Pool

```c
// Configure pool at system init
mem_pool_config_t config = {
    .pool_size = 64 * 1024 * 1024,  // 64MB
    .slot_size = 4096,               // 4KB slots
    .enable_monitoring = true
};
mem_pool_init(&config);
```

### 2. Orchestrator Allocates Buffer (Producer)

```c
// Orchestrator allocates buffer for task input
void *buffer = mem_pool_alloc(1024);
if (!buffer) {
    // Handle pool exhaustion
}

// Register for automatic release
mem_pool_when2free(buffer, target_task_id);
```

### 3. Worker Uses Buffer (Consumer)

```c
// Worker uses buffer, then frees it
mem_pool_free(buffer);
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

## Test Scenarios

### Scenario 1: Basic Alloc/Free

```
1. Init pool (1MB, 256 slots of 4KB)
2. Alloc 100 bytes → succeeds
3. Free 100 bytes → slot returned to pool
4. Verify pool.allocated decreased correctly
```

### Scenario 2: when2free Automatic Release

```
1. Init pool
2. Alloc buffer B1
3. Register when2free(B1, task_id=5)
4. Task 3 completes → min_uncompleted = 4
5. Task 4 completes → min_uncompleted = 5 (B1 NOT freed, 5 is threshold)
6. Task 5 completes → min_uncompleted = 6 → B1 freed automatically
```

### Scenario 3: SPSC Mode Enforcement

```
1. Init pool in SPSC mode
2. Orchestrator (producer) allocates → succeeds
3. Worker (consumer) frees → succeeds
4. Worker attempts to allocate → rejected or ignored (wrong role)
5. Orchestrator attempts to free → rejected or ignored (wrong role)
```

### Scenario 4: Pool Exhaustion

```
1. Init pool (1KB total, 2 slots of 500 bytes)
2. Alloc 500 bytes → slot 1 allocated
3. Alloc 500 bytes → slot 2 allocated
4. Alloc 500 bytes → fails gracefully (pool full)
5. Free slot 1 → slot returned to pool
6. Alloc 500 bytes → succeeds
```

### Scenario 5: Minimum Uncompleted Tracking

```
1. Task State Ring Buffer: [1=PENDING, 2=RUNNING, 3=COMPLETED, 4=PENDING, 5=COMPLETED]
2. Query min_uncompleted → returns 1 (first PENDING)
3. Task 1 completes → ring buffer updates
4. Query min_uncompleted → returns 2 (still RUNNING, not complete)
5. Task 2 completes → returns 4 (first PENDING/RUNNING after 2)
```