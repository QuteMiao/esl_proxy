# Data Model: Memory Pool

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-26

## Entities

### Memory Pool

Pre-allocated region of memory for task intermediate data.

| Field | Type | Description |
|-------|------|-------------|
| `total_size` | `size_t` | Total pool size in bytes |
| `allocated` | `size_t` | Currently allocated bytes |
| `free` | `size_t` | Available bytes (computed: total - allocated) |
| `slots` | `mem_slot_t*` | Array of memory slots |
| `slot_count` | `size_t` | Number of slots |
| `slot_size` | `size_t` | Size of each slot |

**State**: N/A (static allocation)

### Memory Slot

Individual allocatable unit within the pool.

| Field | Type | Description |
|-------|------|-------------|
| `addr` | `void*` | Slot memory address |
| `size` | `size_t` | Slot capacity |
| `in_use` | `_Atomic bool` | Whether slot is allocated |
| `owner_task_id` | `uint32_t` | Task ID that owns this slot |
| `when2free_task_id` | `uint32_t` | Threshold for when2free release (0 = none) |

### Buffer Handle

Reference to allocated buffer returned to caller.

| Field | Type | Description |
|-------|------|-------------|
| `addr` | `void*` | Buffer address within pool |
| `size` | `size_t` | Buffer size |
| `slot_id` | `size_t` | Index of owning slot |

### when2free Registry

Registration entry for automatic memory release.

| Field | Type | Description |
|-------|------|-------------|
| `addr` | `void*` | Buffer address to free |
| `task_id` | `uint32_t` | Release threshold (free when min_uncompleted > task_id) |
| `active` | `bool` | Whether registration is active |

### Minimum Uncompleted Tracker

Tracks minimum uncompleted TaskID via Task State Ring Buffer.

| Field | Type | Description |
|-------|------|-------------|
| `min_uncompleted` | `_Atomic uint32_t` | Current minimum uncompleted TaskID |
| `sentinel` | `uint32_t` | Value indicating no uncompleted tasks (e.g., UINT32_MAX) |

## Relationships

```
Memory Pool
├── slots[] (N slots)
│   └── each slot tracks owner_task_id + when2free_task_id
├── when2free_registry[] (M registered buffers)
│   └── each entry: addr + task_id threshold
└── min_uncompleted_tracker
    └── queries Task State Ring Buffer for state
```

## Validation Rules

1. `slot.in_use == true` → slot is allocated, cannot be allocated again
2. `slot.in_use == false` → slot is free, can be allocated
3. when2free release only when `min_uncompleted > addr.task_id`
4. SPSC: only Orchestrator (producer) calls `alloc()`, only Worker (consumer) calls `free()`

## State Transitions

### Slot State
```
FREE → ALLOCATED (via alloc)
ALLOCATED → FREE (via free or when2free threshold reached)
```

### when2free Entry
```
ACTIVE → FREED (when min_uncompleted > task_id)
ACTIVE → EXPLICIT_FREE (when Orchestrator calls free explicitly)
```

### TaskID Tracker
```
Running → min_uncompleted updates when task state changes to COMPLETED
```