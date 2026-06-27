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

#include "conf.h"
#include "queue.h"
#include "runtime.h"
#include "task.h"

typedef struct ctrl {
    // 64CORES
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];

    uint16_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint16_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    queue_t  ready_queue[TASK_TYPE_CNT];
    queue_t  completed_queue;
    uint16_t tid;
} ctrl_t;

extern EslRuntime *g_runtime;

void *dispatch_worker(void *arg);
void init_ctrl_t(void);

/* 把硬件 AICore 完成事件拉到 msg_bitmap。 */
void dispatch_poll(int tid);

/* dispatch_poll 与 send_task 内联下发共用的硬件辅助（定义在 dispatch_expand.c）。 */
uint64_t dispatch_core_reg_addr(int worker_id);
void dispatch_mark_slot_complete(int exe_type, int core, int slot, uint64_t reg_addr, uint32_t reg_task);

uint32_t dispatch_executor_duration(uint32_t raw_duration);

/* 调度收尾——发布最终统计到 device_wall（定义在 dispatch_expand.c）。 */
void dispatch_publish_final_stats(uint64_t elapsed_ns);

#endif /* DISPATCH_H */
