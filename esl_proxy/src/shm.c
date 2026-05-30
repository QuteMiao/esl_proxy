/*
 * shm.c - Shared memory global definitions for ring buffer task data
 *
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#include "ring_buf.h"

uint16_t g_task_id = 0;
uint16_t g_min_uncomplete_task = 0;
task_state g_state_buf[RING_SIZE];
atomic_flag g_lock_buf[RING_SIZE] = { [0 ... RING_SIZE-1] = ATOMIC_FLAG_INIT };
struct task_desc g_basic_buf[RING_SIZE];
atomic_char16_t g_predecessor_buf[RING_SIZE];
struct successorList g_successor_buf[RING_SIZE];
struct successorList g_successor_exp_buf[HALF_RING_SIZE];
