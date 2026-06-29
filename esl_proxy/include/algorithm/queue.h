#ifndef QUEUE_H
#define QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "conf.h"
#include "executor.h"
#include "task.h"
#include "spin.h"
#include "log.h"

typedef struct queue {
    uint64_t cnt;
    uint64_t head;
    uint64_t tail;
    uint16_t tasks[RING_SIZE];
    atomic_flag lock;
} queue_t;

// Forward declarations for lock/unlock functions
static inline void lock_q(queue_t *queue);
static inline void unlock_q(queue_t *queue);

static inline bool batch_dequeue(queue_t *queue, uint16_t *item, uint16_t *n)
{
    lock_q(queue);
    *n = (uint16_t)(queue->cnt < *n ? queue->cnt : *n);
    if (*n == 0) {
        unlock_q(queue);
        return false;
    }
    uint64_t head = queue->head;
    memcpy(item, &queue->tasks[head], *n * sizeof(uint16_t));

    queue->head = queue->head + *n;
    queue->cnt -= *n;
    unlock_q(queue);
    return true;
}

static inline bool batch_enqueue(queue_t *queue, uint16_t *item, uint16_t n)
{
    lock_q(queue);
    if ((RING_SIZE - queue->cnt) < n) {
        unlock_q(queue);
        return false;
    }
    uint64_t tail = queue->tail;
    memcpy(&queue->tasks[tail], item, n * sizeof(uint16_t));
    queue->tail = tail + n;
    queue->cnt += n;
    unlock_q(queue);
    return true;
}

static inline bool dequeue(queue_t *queue, uint16_t* item)
{
    lock_q(queue);
    if (queue->cnt < 1) {
        unlock_q(queue);
        return false;
    }
    *item = queue->tasks[queue->head];
    queue->head = (queue->head + 1) & (RING_SIZE - 1);
    queue->cnt--;
    unlock_q(queue);
    return true;
}

static inline bool enqueue(queue_t *queue, uint16_t item)
{
    lock_q(queue);
    if (queue->cnt >= RING_SIZE) {
        unlock_q(queue);
        return false;
    }
    queue->tasks[queue->tail] = item;
    queue->tail = (queue->tail + 1) & (RING_SIZE - 1);
    queue->cnt++;
    unlock_q(queue);
    return true;
}

static inline void lock_q(queue_t *queue)
{
    while (atomic_flag_test_and_set_explicit(&queue->lock, memory_order_acquire)) {
        spin_wait();
    }
}

static inline void unlock_q(queue_t *queue)
{
    atomic_flag_clear_explicit(&queue->lock, memory_order_release);
}

#endif
