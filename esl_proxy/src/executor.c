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

extern ctrl_t g_ctrl_t[THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;

/*
 * executor_tick - Process executor timers and update control structures
 *
 * Iterates through g_executors[exe_type][core], checks duration based on ping_pong,
 * writes to msg_bitmap when duration reaches 0, decrements otherwise.
 */
void* executor_worker(void *arg)
{
    (void)arg;
    WORKER_LOGF("executor", "worker started");
    int total_write_cnt = 0;
    while (g_completed_cnt < g_task_id)
    {
        for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
            for (int core = 0; core < AIC_CNT; core++) {
                uint8_t idx = g_executors[exe_type][core].idx;
                if (idx < AIC_OSTD) {
                    uint16_t dur = g_executors[exe_type][core].duration[idx]--;
                    if (dur < 0) {
                        g_ctrl_t[core % THREAD_CNT].msg_bitmap[exe_type][idx] |= ((uint64_t)0x1 << core);
                        g_executors[exe_type][core].idx = AIC_OSTD;
                        total_write_cnt++;
                    }
                } else {
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
    WORKER_LOGF("executor", "finished, total_write_cnt=%d g_task_id=%d", total_write_cnt, g_task_id);
    return NULL;
}