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
    TASK_TYPE_MIX    = 1,
    TASK_TYPE_CNT    = 3,
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

enum {
    EMPTY     = TASK_STATUS_EMPTY,
    PENDING   = TASK_STATUS_CREATING,
    COMPLETED = TASK_STATUS_COMPLETED,
};

typedef struct {
    task_status_t state;
    uint16_t task_id;
    uint32_t successor_cnt;
} task_state;

struct task_desc {
    uint16_t       id;          /* ring-buffer task id */
    task_type_t    type;        /* CUBE / VECTOR / MIX */
    org_mode_t     mode;        /* SINGLE / GROUP / SPMD_SYNC / SPMD_ASYNC */
    void          *kernel;      /* device kernel entry, NULL if unset */
    uint32_t       index;       /* SPMD base block index */
    uint32_t       count;       /* SPMD instance count (block_num) */
    uint64_t       data[16];    /* tensor addresses (Tensor handles) */
    int64_t        scalar[32];  /* scalar kernel arguments */
    uint16_t       tensor_cnt;  /* number of valid data[] entries */
    uint16_t       scalar_cnt;  /* number of valid scalar[] entries */
    uint16_t       duration;    /* estimated kernel cycles (low 16 bits) */
};

struct predecessor_list {
    uint16_t cnt;
    uint16_t* exp;
};

struct node_list {
    uint16_t cnt;
    uint16_t node[CON_NODE_CNT];
    struct node_list* next;
};

#endif /* DAG_TASK_H */