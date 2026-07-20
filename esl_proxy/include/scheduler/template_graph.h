#ifndef SCHEDULER_GRAPH_H
#define SCHEDULER_GRAPH_H

#include <stdint.h>

#include "scheduler/conf.h"

static uint32_t task_id_1[8] = {0, 1, 2, 3, 4, 5, 6, 7};
static bool type_1[8] = {0, 1, 0, 1, 0, 1, 0};
static int duration_1[8] = {5, 5, 5, 5, 5, 5, 5, 5};
static int pre_cnt_1[8] = {0, 1, 1, 2, 2, 2, 2};
static int pre_idx_1[8] = {0, 0, 1, 2, 4, 6, 8};
static int predecessors_1[] = {0, 0, 1, 2, 1, 2, 3, 4, 3, 4, 5, 6};

static uint32_t task_id_2[8] = {0, 1, 2, 3, 4, 5, 6, 7};
static bool type_2[8] = {0, 1, 0, 1, 0, 1, 0};
static int duration_2[8] = {5, 5, 5, 5, 5, 5, 5, 5};
static int pre_cnt_2[8] = {0, 1, 1, 2, 2, 2, 2};
static int pre_idx_2[8] = {0, 0, 1, 2, 4, 6, 8};
static int predecessors_2[] = {0, 0, 1, 2, 1, 2, 3, 4, 3, 4, 5, 6};

typedef struct subgraph {
    uint32_t task_cnt;
    uint32_t* task_id;
    bool* type;
    int* duration;
    int* pre_cnt;
    int* pre_idx;
    int* predecessors;
} subgraph;

static subgraph test_graph[PAINTER_THREAD_CNT] = {
    {8, task_id_1, type_1, duration_1, pre_cnt_1, pre_idx_1, predecessors_1},
    {8, task_id_2, type_2, duration_2, pre_cnt_2, pre_idx_2, predecessors_2}
};

#endif