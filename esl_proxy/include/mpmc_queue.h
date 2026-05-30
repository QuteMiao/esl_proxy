/*
 * mpmc_queue.h - Ready queue enqueue stub
 */

#ifndef MPMC_QUEUE_H
#define MPMC_QUEUE_H

#include <stdint.h>

#include "task.h"

static inline void ready_enqueue(task_type_t type, org_mode_t mode, uint16_t task_id)
{
    (void)type;
    (void)mode;
    (void)task_id;
}

#endif /* MPMC_QUEUE_H */
