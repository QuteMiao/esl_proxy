/*
 * dispatch.h - Task Dispatch with Shared Memory and Work-Stealing
 *
 * Distributes tasks to Executors via shared memory with work-stealing
 * load balancing across multiple Dispatch instances.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef SCHEDULER_DISPATCH_H
#define SCHEDULER_DISPATCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#include "scheduler/conf.h"
#include "common/queue.h"
#include "common/task.h"

/*
 * Multi-producer / multi-reader completed queue.
 *
 * All dispatch threads push completed task ids here (writers are serialized
 * by a spinlock), and every painter thread drains the same ring independently
 * through its own read cursor. An entry becomes overwritable only once ALL
 * painters have advanced past it (write_pos - min(read_pos[]) <= REMOTE_RING_SIZE).
 */
typedef struct {
    uint32_t ring[REMOTE_RING_SIZE];
    _Atomic uint64_t write_pos;
    _Atomic uint64_t read_pos[PAINTER_THREAD_CNT];
    atomic_flag write_lock;
} completed_queue_t;

typedef struct ctrl {
    // 64CORES
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];
    
    uint64_t aicore_mask;

    uint32_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint32_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    uint64_t* aicore_spr_1[EXE_TYPE_CNT][AIC_CNT];
    uint64_t* aicore_spr_2[EXE_TYPE_CNT][AIC_CNT];

    uint32_t tid;
} ctrl_t;

/* Global, thread-shared queues (single instance for the whole scheduler). */
extern queue_t g_ready_queue[TASK_TYPE_CNT];
extern completed_queue_t g_completed_queue;

void *dispatch_worker(void *arg);
void init_ctrl_t(void);

/* Batch-write completed tasks to the shared multi-reader ring buffer.
 * Multi-producer safe: writers serialize on write_lock. */
static inline void completed_queue_write_batch(completed_queue_t *q, uint32_t *items, uint32_t cnt)
{
    if (cnt == 0) return;
    while (atomic_flag_test_and_set_explicit(&q->write_lock, memory_order_acquire)) {
        atomic_thread_fence(memory_order_seq_cst);
    }
    uint64_t wpos = atomic_load_explicit(&q->write_pos, memory_order_relaxed);
    uint32_t *ring = q->ring;
    for (uint32_t i = 0; i < cnt; i++) {
        ring[(wpos + i) & REMOTE_RING_MASK] = items[i];
    }
    atomic_store_explicit(&q->write_pos, wpos + cnt, memory_order_release);
    atomic_flag_clear_explicit(&q->write_lock, memory_order_release);
}

/* Batch-read from the shared multi-reader ring buffer for a specific painter.
 * Returns the number of items actually read (0 if nothing new). */
static inline uint32_t completed_queue_read_batch(completed_queue_t *q, int painter_tid,
                                                   uint32_t *buf, uint32_t max_cnt)
{
    _Atomic uint64_t *my_read = &q->read_pos[painter_tid];
    uint64_t rpos = atomic_load_explicit(my_read, memory_order_acquire);
    uint64_t wpos = atomic_load_explicit(&q->write_pos, memory_order_acquire);

    uint64_t avail = wpos - rpos;
    if (avail <= 0) return 0;

    uint32_t cnt = (uint32_t)(avail < max_cnt ? avail : max_cnt);
    uint32_t *ring = q->ring;
    for (uint32_t i = 0; i < cnt; i++) {
        buf[i] = ring[(rpos + i) & REMOTE_RING_MASK];
    }

    atomic_store_explicit(my_read, rpos + cnt, memory_order_release);
    return cnt;
}

#endif /* SCHEDULER_DISPATCH_H */