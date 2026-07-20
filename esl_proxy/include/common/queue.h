#ifndef COMMON_QUEUE_H
#define COMMON_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>


#define QUEUE_DEPTH 1024

typedef struct queue {
    uint64_t cnt;
    uint64_t head;
    uint64_t tail;
    uint16_t tasks[QUEUE_DEPTH];
    atomic_flag lock;
} queue_t;

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
    if ((QUEUE_DEPTH - queue->cnt) < n) {
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
    queue->head = (queue->head + 1) & (QUEUE_DEPTH - 1);
    queue->cnt--;
    unlock_q(queue);
    return true;
}

static inline bool enqueue(queue_t *queue, uint16_t item)
{
    lock_q(queue);
    if (queue->cnt >= QUEUE_DEPTH) {
        unlock_q(queue);
        return false;
    }
    queue->tasks[queue->tail] = item;
    queue->tail = (queue->tail + 1) & (QUEUE_DEPTH - 1);
    queue->cnt++;
    unlock_q(queue);
    return true;
}

static inline void lock_q(queue_t *queue)
{
    while (atomic_flag_test_and_set_explicit(&queue->lock, memory_order_acquire)) {
        atomic_thread_fence(memory_order_seq_cst);
    }
}

static inline void unlock_q(queue_t *queue)
{
    atomic_flag_clear_explicit(&queue->lock, memory_order_release);
}

#endif /* COMMON_QUEUE_H */
