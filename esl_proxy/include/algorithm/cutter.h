/*
 * cutter.h - Dependency resolution worker
 */

#ifndef CUTTER_H
#define CUTTER_H

#include "conf.h"
#include "queue.h"
#include "task.h"

#include <stdatomic.h>

extern task_state g_state_buf[RING_SIZE];
extern _Atomic uint16_t g_commit_task_id;

void cutter(queue_t *cq, queue_t *rq);
void *cutter_worker(void *arg);
void init_state_buf(void);

#endif /* CUTTER_H */
