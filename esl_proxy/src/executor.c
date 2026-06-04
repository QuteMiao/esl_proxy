/*
 * executor.c - Executor Implementation
 *
 * Provides task execution utilities including delay functionality.
 *
 * C11 standard with _Atomic for lock-free concurrency.
 */

#include <time.h>
#include "log.h"
#include "executor.h"
#include "dispatch.h"
#include "ring_buf.h"

extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;

void executor_init(void)
{
    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int core = 0; core < AIC_CNT; core++) {
            g_executors[exe_type][core].idx = AIC_OSTD;
            for (int i = 0; i < AIC_OSTD; i++) {
                g_executors[exe_type][core].tasks[i] = 0;
                g_executors[exe_type][core].block_idx[i] = 0;
                g_executors[exe_type][core].duration[i] = 0;
                g_executors[exe_type][core].base[i] = 0;
            }
        }
    }
}

void* executor_worker(void *arg)
{
    (void)arg;
    WORKER_LOGF("worker started");
    int total_write_cnt = 0;
    while (atomic_load(&g_completed_cnt) < atomic_load(&g_task_id))
    {
        for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
            for (int core = 0; core < AIC_CNT; core++) {
                uint8_t idx = g_executors[exe_type][core].idx;
                if (idx < AIC_OSTD) {
                    // Decrement duration first
                    if (g_executors[exe_type][core].duration[idx] > 0) {
                        g_executors[exe_type][core].duration[idx]--;
                    }
                    // After decrement, if duration is 0, task is complete
                    if (g_executors[exe_type][core].duration[idx] == 0 ) {
                        g_ctrl_t[core % DISPATCH_THREAD_CNT].msg_bitmap[exe_type][idx] |= ((uint64_t)0x1 << core);
                        g_executors[exe_type][core].idx = AIC_OSTD;
                        total_write_cnt++;
                        WORKER_LOGF("total,%d,core,%d,type,%d", total_write_cnt,core,exe_type);
                    }
                } else {
                    // Find a slot with positive duration to work on
                    for (size_t i = 0; i < AIC_OSTD; i++)
                    {
                        if(g_executors[exe_type][core].duration[i] > 0) {
                            g_executors[exe_type][core].idx = i;
                            break;
                        }
                    }
                }
            }
        }
    }
    WORKER_LOGF("finished, total_write_cnt=%d g_task_id=%d", total_write_cnt, g_task_id);
    return NULL;
}
