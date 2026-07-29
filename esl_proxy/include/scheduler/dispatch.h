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

#include "scheduler/conf.h"
#include "common/queue.h"
#include "common/task.h"

typedef struct ctrl {
    /* free_bitmap[CUBE/VECTOR] authoritative; free_bitmap[MIX] derived (cube & vector). */
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];
    
    uint64_t aicore_mask;

    uint32_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint32_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    uint64_t* aicore_spr_1[EXE_TYPE_CNT][AIC_CNT];
    uint64_t* aicore_spr_2[EXE_TYPE_CNT][AIC_CNT];

    /* MIX dual-side completion: bit0=CUBE done, bit1=VECTOR done at (slot, core). */
    uint8_t mix_side_done[AIC_OSTD][AIC_CNT];

    queue_t  ready_queue[TASK_TYPE_CNT];
    queue_t  completed_queue;
    queue_t  remote_completed_queue;
    uint32_t tid;
} ctrl_t;


void *dispatch_worker(void *arg);
void init_ctrl_t(void);

#endif /* SCHEDULER_DISPATCH_H */
