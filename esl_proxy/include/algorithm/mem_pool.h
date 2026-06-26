/*
 * mem_pool.h - Memory Pool with when2free automatic release
 *
 * Pre-allocated memory pool using FIFO-based allocation.
 * when2free registers memory for release when min_uncompleted >= taskid.
 * Release is implemented by updating FIFO head pointer to registered addr.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef DAG_MEM_POOL_H
#define DAG_MEM_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "ring_buf.h"
#include "task.h"
#include "tensor.h"

/* Alignment requirement for allocations */
#define MEM_POOL_ALIGN 64

/* Sentinel value indicating no uncompleted tasks */
#define SENTINEL_DONE UINT32_MAX

/*
 * when2free entry - addr/taskid pair for automatic release
 */
typedef struct when2free_entry {
    void *addr;
    uint32_t taskid;
} when2free_entry_t;

/*
 * when2free FIFO queue - lock-free SPSC queue for when2free entries
 */
typedef struct when2free_fifo {
    when2free_entry_t *entries;
    size_t capacity;
    _Atomic size_t head; /* consumer index */
    _Atomic size_t tail; /* producer index */
} when2free_fifo_t;

/*
 * Memory pool with FIFO-based allocation and when2free support
 */
typedef struct mem_pool {
    void *base;             /* base address of pre-allocated memory */
    size_t size;            /* total size of pool */
    _Atomic uintptr_t head; /* consumer head (for deallocation) */
    _Atomic uintptr_t tail; /* producer tail (for allocation) */
    when2free_fifo_t fifo;  /* when2free FIFO queue */
} mem_pool_t;

extern mem_pool_t g_mem_pool;

/*
 * Allocate from memory pool (FIFO tail advance)
 * Trust caller: enough space available
 */
static inline void *mem_pool_alloc(mem_pool_t *pool, size_t size)
{
    uintptr_t tail = atomic_load_explicit(&pool->tail, memory_order_relaxed);
    uintptr_t new_tail = tail + size;

    /* Check if enough space (without wrap for simplicity) */
    if (new_tail > (uintptr_t)pool->base + pool->size) {
        return NULL;
    }

    void *result = (void *)tail;
    atomic_store_explicit(&pool->tail, new_tail, memory_order_release);
    return result;
}

static inline Tensor alloc_tensors(uint32_t shape[], int dim, int bytes)
{
    size_t size = (size_t)shape[0] * (size_t)shape[1] * (size_t)dim * (size_t)bytes;
    uint64_t base = (uint64_t)(uintptr_t)mem_pool_alloc(&g_mem_pool, size);
    const uint32_t shapes[2] = {shape[0], shape[1]};
    return tensor_from_base_layout(base, shapes, 2, (dtype_t)bytes);
}

/*
 * when2free FIFO initialization
 */
static inline void when2free_fifo_init(when2free_fifo_t *fifo, when2free_entry_t *entries,
                                       size_t capacity)
{
    fifo->entries = entries;
    fifo->capacity = capacity;
    atomic_store_explicit(&fifo->head, 0, memory_order_relaxed);
    atomic_store_explicit(&fifo->tail, 0, memory_order_relaxed);
}

/*
 * when2free FIFO push (producer side - SPSC)
 */
static inline void when2free_fifo_push(when2free_fifo_t *fifo, void *addr, uint32_t taskid)
{
    size_t tail = atomic_load_explicit(&fifo->tail, memory_order_relaxed);
    size_t next_tail = (tail + 1) % fifo->capacity;
    fifo->entries[tail].addr = addr;
    fifo->entries[tail].taskid = taskid;
    atomic_store_explicit(&fifo->tail, next_tail, memory_order_release);
}

/*
 * when2free FIFO pop (consumer side - SPSC)
 * Returns 0 on success, -1 if queue is empty
 */
static inline int when2free_fifo_pop(when2free_fifo_t *fifo, void **addr, uint32_t *taskid)
{
    size_t head = atomic_load_explicit(&fifo->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&fifo->tail, memory_order_acquire);
    if (head == tail) {
        return -1; /* queue empty */
    }
    size_t next_head = (head + 1) % fifo->capacity;
    *addr = fifo->entries[head].addr;
    *taskid = fifo->entries[head].taskid;
    atomic_store_explicit(&fifo->head, next_head, memory_order_release);
    return 0;
}

/*
 * Check if when2free FIFO is empty
 */
static inline int when2free_fifo_empty(when2free_fifo_t *fifo)
{
    return atomic_load_explicit(&fifo->head, memory_order_acquire) ==
           atomic_load_explicit(&fifo->tail, memory_order_acquire);
}

/*
 * Memory pool initialization
 * Trust caller: base != NULL, size > 0
 */
static inline void mem_pool_init(mem_pool_t *pool, void *base, size_t size)
{
    pool->base = base;
    pool->size = size;
    atomic_store_explicit(&pool->head, (uintptr_t)base, memory_order_relaxed);
    atomic_store_explicit(&pool->tail, (uintptr_t)base, memory_order_relaxed);
}

/*
 * Initialize when2free FIFO in memory pool
 * Trust caller: entries != NULL, capacity > 0
 */
static inline void mem_pool_init_fifo(mem_pool_t *pool, when2free_entry_t *entries, size_t capacity)
{
    when2free_fifo_init(&pool->fifo, entries, capacity);
}

/*
 * Register when2free entry (producer side)
 * Trust caller: valid addr, taskid
 */
static inline void mem_pool_when2free(mem_pool_t *pool, void *addr, uint32_t taskid)
{
    when2free_fifo_push(&pool->fifo, addr, taskid);
}

/*
 * Query minimum uncompleted TaskID from ring buffer
 */
static inline uint32_t mem_pool_min_uncompleted(void)
{
    return ring_min_uncompleted();
}

/*
 * Process when2free queue - release memory when threshold reached
 * Called by manager thread. Updates pool head pointer to addr when min_uncompleted >= taskid.
 */
static inline void mem_pool_process_when2free(mem_pool_t *pool)
{
    void *addr;
    uint32_t taskid;
    uint32_t min_uncompleted = mem_pool_min_uncompleted();

    /* Process all eligible entries */
    while (when2free_fifo_pop(&pool->fifo, &addr, &taskid) == 0) {
        if (min_uncompleted >= taskid) {
            /* Release memory by advancing head to addr */
            atomic_store_explicit(&pool->head, (uintptr_t)addr, memory_order_release);
        }
    }
}

/*
 * Get current allocation usage (for debugging/monitoring)
 */
static inline size_t mem_pool_allocated(mem_pool_t *pool)
{
    uintptr_t head = atomic_load_explicit(&pool->head, memory_order_acquire);
    uintptr_t tail = atomic_load_explicit(&pool->tail, memory_order_relaxed);
    return (size_t)(tail - head);
}

/*
 * Get available space in pool
 */
static inline size_t mem_pool_available(mem_pool_t *pool)
{
    uintptr_t tail = atomic_load_explicit(&pool->tail, memory_order_relaxed);
    return (size_t)((uintptr_t)pool->base + pool->size - tail);
}

#endif /* DAG_MEM_POOL_H */
