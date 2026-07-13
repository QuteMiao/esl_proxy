/*
 * dispatch.h - Task Dispatch with Shared Memory and Work-Stealing
 *
 * Distributes tasks to Executors via shared memory with work-stealing
 * load balancing across multiple Dispatch instances.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef DISPATCH_H
#define DISPATCH_H

#include <stdint.h>
#include <stdatomic.h>

#include "conf.h"
#include "task.h"
#include "queue.h"
#include "runtime.h"

/* ===== types / globals ===== */

typedef struct ctrl {
    // 64CORES
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];

    uint16_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint16_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    queue_t  completed_queue;
    uint16_t tid;
} ctrl_t;

extern EslRuntime *g_runtime;

/* Single shared ready queue, one per task type (shape). Cutters push all newly-
 * ready tasks here; every dispatch lane pops its own free-core count from it, so
 * a starving lane always finds queued work. No per-lane local queue — aligns with
 * simpler's shared shape queues. MPMC-safe via queue_t's own lock. */
extern queue_t g_shared_ready[TASK_TYPE_CNT];

/* Number of AICore cores owned by lane `tid` under the strided partition. */
static inline int lane_core_count(int tid)
{
    return (AIC_CNT / DISPATCH_THREAD_CNT) +
           ((AIC_CNT % DISPATCH_THREAD_CNT) > tid ? 1 : 0);
}

/* ===== worker / poll / init ===== */

void *dispatch_worker(void *arg);
void init_ctrl_t(void);

/* 把硬件 AICore 完成事件拉到 msg_bitmap。 */
void dispatch_poll(int tid);

/* ===== SPMD (external: cutter) ===== */

/* Reset SPMD block cursor when a task becomes ready (called by cutter). */
void dispatch_spmd_on_ready(uint16_t task_id);

#endif /* DISPATCH_H */
