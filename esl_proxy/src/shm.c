/*
 * shm.c - Shared memory global definitions for ring buffer task data
 *
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#include "mem_pool.h"
#include "ring_buf.h"
#include "executor.h"
#include "conf.h"
#include "dispatch.h"

atomic_int g_task_id = 0;
atomic_int g_min_uncomplete_task = 0;
// Keep Atomic For Multi Dispatch Thread
atomic_int g_completed_cnt = 0;
atomic_bool g_is_done = false;
atomic_bool g_orch_is_done = false;


struct task_desc g_basic_buf[RING_SIZE];
struct node_list g_successor_buf[RING_SIZE];
struct node_list g_successor_exp_buf[HALF_RING_SIZE];

struct predecessor_list g_predecessors[RING_SIZE];
struct ring_buf g_predecessor_ring;

uint16_t g_task_id_buf[RING_SIZE];
executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
atomic_flag g_lock_buf[RING_SIZE];
mem_pool_t g_mem_pool;
ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

void init_predecessors(void)
{
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_predecessors[i].cnt = 0;
        g_predecessors[i].exp = NULL;
    }
}

void init_ctrl_t(void)
{
    for (int tid = 0; tid < DISPATCH_THREAD_CNT; tid++) {
        g_ctrl_t[tid].tid = (uint16_t)tid;

        // Initialize free_bitmap for TASK_TYPE
        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].free_bitmap[i][j] = 0xFFFFFFFFFFFFFFFULL;
            }
        }

        // Initialize msg_bitmap for EXE_TYPE
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].msg_bitmap[i][j] = 0x0;
            }
        }

        // Initialize task_id_map
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_CNT; j++) {
                g_ctrl_t[tid].task_id_map1[i][j] = 0;
                g_ctrl_t[tid].task_id_map2[i][j] = 0;
            }
        }

        // Initialize queues
        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            memset(&g_ctrl_t[tid].ready_queue[i], 0, sizeof(queue_t));
        }
        memset(&g_ctrl_t[tid].completed_queue, 0, sizeof(queue_t));
    }
}
