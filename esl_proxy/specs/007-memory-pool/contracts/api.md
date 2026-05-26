# Memory Pool Contracts

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-26

## Public API Contracts

### mem_pool_init

```c
int mem_pool_init(mem_pool_config_t *config);
```

**Contract**:
- Pre-allocate `config->pool_size` bytes using standard allocator
- Initialize slot array with `config->slot_size` slot size
- Return 0 on success, -1 on allocation failure
- Initial state: all slots FREE, when2free registry empty

**Caller guarantees**: `config` is non-NULL, `config->pool_size > 0`, `config->slot_size > 0`

---

### mem_pool_alloc

```c
void* mem_pool_alloc(size_t size);
```

**Contract**:
- Find first FREE slot with sufficient size
- Mark slot ALLOCATED, set owner_task_id = current_task_id
- Return slot address on success, NULL if no suitable slot

**Caller guarantees**: Called from Orchestrator (producer role) only; SPSC mode

**Performance**: < 1μs under normal conditions

---

### mem_pool_free

```c
int mem_pool_free(void *addr);
```

**Contract**:
- Find slot by address
- Mark slot FREE, clear owner_task_id
- Return 0 on success, -1 if slot not found or not in use

**Caller guarantees**: Called from Worker (consumer role) only; addr was returned by mem_pool_alloc

**Performance**: < 1μs under normal conditions

---

### mem_pool_when2free

```c
int mem_pool_when2free(void *addr, uint32_t task_id);
```

**Contract**:
- Register buffer address with task_id threshold
- Buffer will be automatically freed when min_uncompleted > task_id
- Return 0 on success, -1 if registration failed (e.g., no space in registry)

**Caller guarantees**: Called from Orchestrator only; addr was returned by mem_pool_alloc; task_id is valid

---

### mem_pool_process_when2free

```c
void mem_pool_process_when2free(uint32_t min_uncompleted);
```

**Contract**:
- Scan when2free registry
- Free any entry where `min_uncompleted > entry.task_id`
- Mark entry inactive after freeing

**Called by**: Manager thread (typically in polling loop)

**Performance**: < 1μs for typical registry sizes (< 1000 entries)

---

### task_state_ring_buffer_min_uncompleted

```c
uint32_t task_state_ring_buffer_min_uncompleted(void);
```

**Contract**:
- Query Task State Ring Buffer for minimum uncompleted TaskID
- Return sentinel value (e.g., UINT32_MAX) if no uncompleted tasks
- Treat "running" state as uncompleted

**Performance**: < 1μs

---

## Internal Contracts

### Slot State Machine

```
FREE --[alloc]--> ALLOCATED
ALLOCATED --[free/when2free]--> FREE
```

### SPSC Access Control

| Role | Producer (Orchestrator) | Consumer (Worker) |
|------|------------------------|------------------|
| mem_pool_alloc | MUST | MUST NOT |
| mem_pool_free | MUST NOT | MUST |
| mem_pool_when2free | MUST | MUST NOT |
| mem_pool_process_when2free | (Manager thread context) |

---

## Error Handling

Per Constitution Principle X (Trust the Caller):
- Invalid input → undefined behavior (caller guarantees validity)
- Allocation failure → return NULL (not pool panic)
- Double-free → undefined behavior (caller guarantees single free)
- Wrong SPSC role → undefined behavior (caller guarantees correct role)