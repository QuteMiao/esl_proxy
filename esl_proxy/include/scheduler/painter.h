/*
 * cutter.h - Dependency resolution worker
 */

#ifndef PAINTER_H
#define PAINTER_H

#include "scheduler/conf.h"
#include "common/queue.h"
#include "scheduler/template_graph.h"

typedef enum {
    TASK_STATUS_EMPTY = 0,
    TASK_STATUS_CREATING,
    TASK_STATUS_SUBMITTED,
    TASK_STATUS_COMPLETED,
} task_status_t;

void *painter(void *arg);
void init_state_buf(void);

#endif
