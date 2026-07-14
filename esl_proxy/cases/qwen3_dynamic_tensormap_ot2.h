// Orchestration Function: qwen3_decode (dynamic tensormap, configurable-SPMD
// variant).
//
// Mirrors
// V200-benchmark/qwen3/qwen3_dynamic_tensormap/orchestration/qwen3_decode.cpp.
// Dependencies are discovered automatically via tensormap
#include <stddef.h>
#include <stdint.h>

#include "mem_pool.h"
#include "tensormap.h"

atomic_int g_desc_task_id = 0;
atomic_bool g_desc_done = false;

#define DETECT_BATCH_SIZE 60
#define MAX_TENSOR_NUM 4096
extern Tensor g_tensors[MAX_TENSOR_NUM];

bool get_detect_tasks(int &detect_start, int &detect_end) {
    detect_start = atomic_fetch_add(&g_desc_task_id, DETECT_BATCH_SIZE);
    bool desc_done = atomic_load(&g_desc_done);
    int desc_task_id = atomic_load(&g_desc_task_id);
    
    detect_end = detect_start + DETECT_BATCH_SIZE;

    if (desc_done) {
        if(detect_start > desc_task_id) {
            return false;
        } else if (detect_end > desc_task_id) {
            detect_end = desc_task_id;
        } 
    }
    return true;
}

void update_tensor_map() {
    
}

void detector() {
    int detect_start = 0;
    int detect_end = 0;
    while (get_detect_tasks(detect_start, detect_end))
    {
        update_tensor_map();
        for (size_t i = detect_start; i < detect_end; i++)
        {
            for (size_t j = 0; i < count; i++)
            {
                lookup_tensor();
            }
            add_predecessors();
        }
    }
}
