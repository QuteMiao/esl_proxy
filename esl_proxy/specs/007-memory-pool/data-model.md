# Data Model: Memory Pool

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-27

## Entities

### Memory Pool

Pre-allocated region of memory for task intermediate data managed via ring buffer head/tail pointers.

| Field | Type | Description |
|-------|------|-------------|
| `base_addr` | `void*` | Base address of pre-allocated memory block |
| `total_size` | `size_t` | Total pool size in bytes |
| `slot_size` | `size_t` | Size of each slot |
| `slot_count` | `size_t` | Number of slots (computed: total_size / slot_size) |
| `_Atomic tail` | `size_t` | Producer head pointer for allocation (SPSC) |
| `_Atomic head` | `size_t` | Consumer head pointer for release (SPSC) |

**State**: N/A (static allocation via ring buffer)

### Memory Slot

Individual allocatable unit within the pool - addressed via base_addr + slot_size * index.

| Field | Type | Description |
|-------|------|-------------|
| `addr` | `void*` | Slot memory address (derived: base_addr + slot_size * index) |
| `in_use` | `_Atomic bool` | Whether slot is currently allocated |
| `when2free_task_id` | `uint32_t` | Threshold for when2free release (0 = none) |

### Buffer Handle

Reference to allocated buffer returned to caller.

| Field | Type | Description |
|-------|------|-------------|
| `slot_index` | `size_t` | Index of allocated slot |
| `addr` | `void*` | Buffer address within pool |
| `size` | `size_t` | Buffer size |

### when2free Registry

Registration entry for automatic memory release.

| Field | Type | Description |
|-------|------|-------------|
| `slot_index` | `size_t` | Index of registered slot |
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
Memory Pool (Ring Buffer)
├── base_addr + slot_size * tail (alloc)
├── base_addr + slot_size * head (free)
├── slots[] (N slots by index)
│   └── each slot: in_use state + when2free_task_id
├── when2free_registry[] (M registered slots)
│   └── each entry: slot_index + task_id threshold
└── min_uncompleted_tracker
    └── queries Task State Ring Buffer for state
```

## Validation Rules

1. `slot.in_use == true` → slot is allocated via ring buffer tail
2. `slot.in_use == false` → slot is in free state, can be allocated
3. when2free release only when `min_uncompleted > slot.when2free_task_id`
4. SPSC: only Orchestrator (producer) increments `tail`, only Worker/Manager (consumer) increments `head`

## State Transitions

### Slot State (via Ring Buffer)
```
FREE --[tail % slot_count]--> ALLOCATED
ALLOCATED --[head % slot_count]--> FREE
```

### Ring Buffer Pointer Wrap
```
tail: (tail + 1) % slot_count
head: (head + 1) % slot_count
```

### when2free Entry
```
ACTIVE --[min_uncompleted > task_id]--> FREED
ACTIVE --[explicit free]--> EXPLICIT_FREE
```

### TaskID Tracker
```
Running --> min_uncompleted updates when task state changes to COMPLETED
```