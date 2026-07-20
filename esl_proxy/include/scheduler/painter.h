/*
 * cutter.h - Dependency resolution worker
 */

#ifndef PAINTER_H
#define PAINTER_H

#include <stdatomic.h>

#include "scheduler/conf.h"
#include "task.h"
#include "common/queue.h"
#include "scheduler/dispatch.h"
#include "scheduler/template_graph.h"

/* Extern declarations from ring_buf / task system (avoid pulling in algorithm/ring_buf.h
 * which conflicts with common/queue.h) */
extern atomic_int g_task_id;
extern atomic_int g_min_uncomplete_task;
extern struct node_list g_successor_buf[RING_SIZE];

void *painter(void *arg);
void init_state_buf(void);

#endif
