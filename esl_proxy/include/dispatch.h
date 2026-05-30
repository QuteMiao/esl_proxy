/*
 * dispatch.h - Task Dispatch with Shared Memory and Work-Stealing
 *
 * Distributes tasks to Executors via shared memory with work-stealing
 * load balancing across multiple Dispatch instances.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef DAG_DISPATCH_H
#define DAG_DISPATCH_H

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include "executor.h"
#include "task.h"

#define THREAD_CNT 2
#define TASK_TYPE_CNT 3
#define AIC_OSTD 2
#define EXE_TYPE_CNT 2
#define AIC_CNT 60

typedef struct queue {
    uint64_t head;
    uint64_t tail;
    uint16_t tasks[RING_SIZE];
} queue_t;

typedef struct ctrl {
    // 64CORES
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];
    uint16_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint16_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];
    uint16_t ready_cnt[TASK_TYPE_CNT];
    queue_t  ready_queue[TASK_TYPE_CNT];
    queue_t  completed_queue;
    uint16_t tid;
} ctrl_t;

#endif /* DAG_DISPATCH_H */