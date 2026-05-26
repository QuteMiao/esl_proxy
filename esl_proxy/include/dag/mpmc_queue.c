/*
 * mpmc_queue.c - Global queue definitions
 *
 * Defines the 2D ReadyQueue matrix and global CompleteQueue.
 * Default capacity: 1024 for all queues.
 */

#include "mpmc_queue.h"

/* === Default Queue Capacities === */
#define READY_QUEUE_CAPACITY 1024
#define COMPLETE_QUEUE_CAPACITY 1024

/* === Global Queue Definitions === */
mpmc_queue_t g_ready_queues[TASK_TYPE_COUNT][ORG_MODE_COUNT];
mpmc_queue_t g_complete_queue;