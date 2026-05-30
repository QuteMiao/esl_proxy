/*
 * task.h - Task descriptor and related types for DAG engine
 *
 * Task descriptor contains ONLY description information - NO execution state.
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#ifndef DAG_TASK_H
#define DAG_TASK_H

#include <stdint.h>
#include <stdatomic.h>
#include "conf.h"

typedef uint16_t task_id_t;

typedef enum {
    TASK_TYPE_CUBE   = 0,
    TASK_TYPE_VECTOR = 1,
    TASK_TYPE_MIX    = 2,
} task_type_t;

/*
 * Organization mode enum - how task instances are created
 */
typedef enum {
    ORG_MODE_SINGLE     = 0,
    ORG_MODE_GROUP      = 1,
    ORG_MODE_SPMD_SYNC  = 2,
    ORG_MODE_SPMD_ASYNC = 3,
} org_mode_t;

typedef enum {
    TASK_STATUS_EMPTY = 0,
    TASK_STATUS_CREATING,
    TASK_STATUS_SUBMITTED,
    TASK_STATUS_COMPLETED,
} task_status_t;

typedef struct {
    task_status_t status;
    uint16_t task_id;
    uint32_t successor_cnt;
} task_state;

_Atomic task_state g_state_buf[RING_SIZE];
uint16_t g_task_id_buf[RING_SIZE];

struct task_desc {
    uint16_t    id;        /* Task identifier (2 bytes) */
    task_type_t type;      /* CUBE/VECTOR/MIX */
    org_mode_t  mode;      /* SINGLE/GROUP/SPMD_SYNC/SPMD_ASYNC */
    void       *kernel;    /* KERNEL code pointer */
    uint32_t    index;     /* base INDEX for SPMD */
    uint32_t    count;     /* number of instances */
    uint64_t    data[16];  
    int64_t     scalar[32];
    uint16_t    tensorCnt;
    uint16_t    scalarCnt;
    uint16_t    duration;
};

struct successorList {
    uint16_t successor[3];
    struct successorList* next;
};

#endif /* DAG_TASK_H */