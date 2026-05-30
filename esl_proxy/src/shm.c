/*
 * shm.c - Shared memory global definitions for ring buffer task data
 *
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#include "mem_pool.h"
#include "ring_buf.h"

#include "conf.h"

uint16_t g_task_id = 0;
uint16_t g_min_uncomplete_task = 0;

_Atomic task_state g_state_buf[RING_SIZE];
struct task_desc g_basic_buf[RING_SIZE];
_Atomic uint16_t g_predecessor_buf[RING_SIZE];
struct succ_list g_successor_buf[RING_SIZE];
struct succ_list g_successor_exp_buf[HALF_RING_SIZE];
uint16_t g_task_id_buf[RING_SIZE];

atomic_flag g_lock_buf[RING_SIZE];

mem_pool_t g_mem_pool;

atomic_int g_task_cnt;
atomic_int g_completed_cnt;
