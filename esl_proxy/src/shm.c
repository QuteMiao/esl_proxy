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
uint16_t g_min_uncomplete_task = 0;
atomic_int g_completed_cnt = -1;

_Atomic task_state g_state_buf[RING_SIZE];
_Atomic uint16_t g_predecessor_buf[RING_SIZE];
struct task_desc g_basic_buf[RING_SIZE];
struct succ_list g_successor_buf[RING_SIZE];
struct succ_list g_successor_exp_buf[HALF_RING_SIZE];
uint16_t g_task_id_buf[RING_SIZE];
executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
atomic_flag g_lock_buf[RING_SIZE];
mem_pool_t g_mem_pool;
ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
