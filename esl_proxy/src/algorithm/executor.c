/*
 * executor.c - Executor Implementation
 *
 * Provides task execution utilities including delay functionality.
 *
 * C11 standard with _Atomic for lock-free concurrency.
 */

#define _POSIX_C_SOURCE 199309L

#include "conf.h"
#include <time.h>
#include "log.h"
#include "executor.h"
#include "dispatch.h"
#include "ring_buf.h"

extern _Atomic bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

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
    int total_write_cnt = 0;
    int iterations = 0;
    while (!atomic_load(&g_is_done))
    {
        for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
            for (int core = 0; core < AIC_CNT; core++) {
                uint8_t idx = g_executors[exe_type][core].idx;
                if (idx < AIC_OSTD) {
                    uint32_t task_id = g_executors[exe_type][core].tasks[idx];
                    uint32_t block_idx = g_executors[exe_type][core].block_idx[idx];
                    uint32_t block_count = g_basic_buf[task_id & RING_MASK].count;
                    
                    // Handle SPMD tasks with multiple blocks
                    if (block_count > 1) {
                        // Check if current block is done
                        if (g_executors[exe_type][core].duration[idx] == 0) {
                            block_idx++;
                            g_executors[exe_type][core].block_idx[idx] = block_idx;
                            // Reset duration for next block if not done
                            if (block_idx < block_count) {
                                // Get base duration for this task (reuse original value)
                                g_executors[exe_type][core].duration[idx] = 
                                    (uint32_t)g_basic_buf[task_id & RING_MASK].duration;
                            }
                        } else {
                            // Decrement current block's duration
                            g_executors[exe_type][core].duration[idx]--;
                        }
                        
                        // Check if all blocks are done
                        if (block_idx >= block_count) {
                            g_ctrl_t[core % DISPATCH_THREAD_CNT].msg_bitmap[exe_type][idx] |= ((uint64_t)0x1 << core);
                            g_executors[exe_type][core].idx = AIC_OSTD;
                            // Reset block_idx for reuse
                            g_executors[exe_type][core].block_idx[idx] = 0;
                            total_write_cnt++;
                            WORKER_LOGF("total,%d,core,%d,type,%d,blocks,%u", total_write_cnt, core, exe_type, block_count);
                        }
                    } else {
                        // Non-SPMD task: simple single-block execution
                        if (g_executors[exe_type][core].duration[idx] > 0) {
                            g_executors[exe_type][core].duration[idx]--;
                        }
                        if (g_executors[exe_type][core].duration[idx] == 0) {
                            g_ctrl_t[core % DISPATCH_THREAD_CNT].msg_bitmap[exe_type][idx] |= ((uint64_t)0x1 << core);
                            g_executors[exe_type][core].idx = AIC_OSTD;
                            // Reset block_idx for reuse
                            g_executors[exe_type][core].block_idx[idx] = 0;
                            total_write_cnt++;
                            WORKER_LOGF("total,%d,core,%d,type,%d", total_write_cnt, core, exe_type);
                        }
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
        iterations++;
        // Add a small delay to prevent busy-waiting
        if (iterations % 1000000 == 0) {
            WORKER_LOGF("executor iter=%d total_write=%d", iterations, total_write_cnt);
        }
    }
    WORKER_LOGF("finished, total_write_cnt=%d g_task_id=%d", total_write_cnt, g_task_id);
    return NULL;
}
