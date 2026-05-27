# Memory Pool Contracts

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-27

## Public API Contracts

### mem_pool_init

```c
int mem_pool_init(mem_pool_config_t *config);
```

**Contract**:
- Pre-allocate `config->pool_size` bytes using standard allocator
- Initialize ring buffer: head=0, tail=0
- Compute slot_count = pool_size / slot_size
- Return 0 on success, -1 on allocation failure
- Initial state: all slots FREE, when2free registry empty

**Caller guarantees**: `config` is non-NULL, `config->pool_size > 0`, `config->slot_size > 0`

---

### mem_pool_alloc

```c
size_t mem_pool_alloc(size_t size);
```

**Contract**:
- Calculate required slot count: (size + slot_size - 1) / slot_size
- Check if `tail - head < slot_count` (available slots)
- Allocate contiguous slots starting at `tail % slot_count`
- Increment `tail` by slot_count (atomic)
- Return slot index on success, `INVALID_SLOT` if pool full

**Caller guarantees**: Called from Orchestrator (producer role) only; SPSC mode

**Performance**: < 1μs under normal conditions

---

### mem_pool_addr

```c
void* mem_pool_addr(size_t slot_index);
```

**Contract**:
- Return address: `base_addr + slot_index * slot_size`
- No validation (Trust the Caller)

**Caller guarantees**: slot_index is valid

---

### mem_pool_free

```c
int mem_pool_free(size_t slot_index);
```

**Contract**:
- Mark slot as FREE
- Increment `head` by 1 (atomic)
- Return 0 on success, -1 if already freed

**Caller guarantees**: Called from Worker/Manager (consumer role) only; slot was allocated

**Performance**: < 1μs under normal conditions

---

### mem_pool_when2free

```c
int mem_pool_when2free(size_t slot_index, uint32_t task_id);
```

**Contract**:
- Register slot with when2free threshold
- Buffer freed when min_uncompleted > task_id
- Return 0 on success, -1 if registry full

**Caller guarantees**: Called from Orchestrator only; slot was allocated

---

### mem_pool_process_when2free

```c
void mem_pool_process_when2free(uint32_t min_uncompleted);
```

**Contract**:
- Scan when2free registry
- Free any slot where `min_uncompleted > entry.task_id`
- Increment `head` for each freed slot

**Called by**: Manager thread (typically in polling loop)

**Performance**: < 1μs for typical registry sizes (< 1000 entries)

---

## Ring Buffer State Machine (SPSC)

| State | Producer (tail) | Consumer (head) |
|-------|----------------|-----------------|
| FREE | - | - |
| ALLOCATING | tail++ → slot | - |
| ALLOCATED | slot in use | - |
| FREEING | - | head++ → slot |
| FREE | slot available | - |

### Wraparound
```
tail = tail % slot_count
head = head % slot_count
```

## Internal Contracts

### Slot Index Derivation

```
addr(slot_index) = base_addr + (slot_index * slot_size)
```

### SPSC Access Control

| Role | Producer (Orchestrator) | Consumer (Worker/Manager) |
|------|------------------------|--------------------------|
| mem_pool_alloc | MUST | MUST NOT |
| mem_pool_free | MUST NOT | MUST |
| mem_pool_when2free | MUST | MUST NOT |
| mem_pool_process_when2free | (Manager thread context) |

---

## Error Handling

Per Constitution Principle X (Trust the Caller):
- Invalid input → undefined behavior (caller guarantees validity)
- Allocation failure → return INVALID_SLOT (not pool panic)
- Double-free → undefined behavior (caller guarantees single free)
- Wrong SPSC role → undefined behavior (caller guarantees correct role)