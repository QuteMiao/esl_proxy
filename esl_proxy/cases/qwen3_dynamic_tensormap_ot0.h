// Orchestration Function: qwen3_decode (dynamic tensormap, configurable-SPMD
// variant).
//
// Mirrors
// V200-benchmark/qwen3/qwen3_dynamic_tensormap/orchestration/qwen3_decode.cpp.
// Dependencies are discovered automatically via tensormap
// (tm_in/tm_out/tm_submit). SPMD tier is selected at compile time via
// QWEN3_SPMD_TIER (0=non-spmd .. 4=all-spmd).
//
// Durations are V200-benchmark per-subtask means (README.md §1.2.1 AICore View)
// in ns.
#include <stddef.h>
#include <stdint.h>


#include "mem_pool.h"
#include "tensormap.h"

#define MAX_TENSOR_NUM 4096
Tensor g_tensors[MAX_TENSOR_NUM];

#ifndef QWEN3_SPMD_TIER
#define QWEN3_SPMD_TIER 4
#endif
#if QWEN3_SPMD_TIER < 0 || QWEN3_SPMD_TIER > 4
#error "QWEN3_SPMD_TIER must be 0..4"
#endif

static inline int qwen3_min_i(int a, int b) {
    return a < b ? a : b;
}

static inline int qwen3_blocks_per_task(int total_chunks) {
    static const int targets[5] = {1, 2, 4, 8, 1 << 30};
    int target = targets[QWEN3_SPMD_TIER];
    return qwen3_min_i(total_chunks, target);
}

int g_tensor_index = 0;


static inline int alloc_tensors_v2(uint32_t shape[], int dim, int bytes)
{
    size_t size = (size_t)shape[0] * (size_t)shape[1] * (size_t)dim * (size_t)bytes;
    uint64_t base = (uint64_t)(uintptr_t)mem_pool_alloc(&g_mem_pool, size);
    const uint32_t shapes[2] = {shape[0], shape[1]};
    g_tensors[g_tensor_index] = tensor_from_base_layout(base, shapes, 2, (dtype_t)bytes);
    g_tensor_index++;
    return (g_tensor_index - 1);
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
    const int64_t user_batch = 90; 
    const int64_t batch_padded = 96;
    int t0 = alloc_tensors_v2((uint32_t[2]){batch_padded, 5120}, 2, FLOAT32);
    int q_proj = alloc_tensors_v2((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    int k_proj = alloc_tensors_v2((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    int v_proj = alloc_tensors_v2((uint32_t[2]){batch_padded, 5120}, 2, FLOAT32);
    int q_proj_norm = alloc_tensors_v2((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    int k_proj_norm = alloc_tensors_v2((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        int t5 = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, BFLOAT16);
        g_task_id++;
        for (int base = 0; base < 20; base += qwen3_blocks_per_task(20)) {
            g_task_id++;
        }

        for (int base = 0; base < 8; base += qwen3_blocks_per_task(8)) {
            g_task_id++;
            g_task_id++;
        }
        g_task_id++;
    }
    int attn_out[6];
    for (int i = 0; i < 6; i++) {
        attn_out[i] = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, BFLOAT16);
    }
    for (int64_t b = 0; b < user_batch; b += 1) {
        int all_raw_scores = alloc_tensors_v2((uint32_t[2]){4096, 128}, 2, FLOAT32);
        int all_exp_padded = alloc_tensors_v2((uint32_t[2]){4096, 128}, 2, BFLOAT16);
        int all_cur_mi = alloc_tensors_v2((uint32_t[2]){4096, 1}, 2, FLOAT32);
        int all_cur_li = alloc_tensors_v2((uint32_t[2]){4096, 1}, 2, FLOAT32);
        int all_oi_tmp = alloc_tensors_v2((uint32_t[2]){4096, 128}, 2, FLOAT32);
        int q_padded_local = alloc_tensors_v2((uint32_t[2]){128, 128}, 2, BFLOAT16);
        int k_cache_update = alloc_tensors_v2((uint32_t[2]){8, 128}, 2, BFLOAT16); // ROPE KV write-back
        int v_cache_update = alloc_tensors_v2((uint32_t[2]){8, 128}, 2, BFLOAT16); // ROPE KV write-back
        g_task_id++;

        for (int base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
            g_task_id++;
            g_task_id++;
            g_task_id++;
            g_task_id++;
        }
    }

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        int resid1_tile = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, FLOAT32);
        int gm_pipe_buffer_0 = alloc_tensors_v2((uint32_t[2]){16384, 40}, 2, FLOAT32);
        int post_norm_tile = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, BFLOAT16);
        int mlp_tile = alloc_tensors_v2((uint32_t[2]){16, 17408}, 2, BFLOAT16);
        int gate_tile = alloc_tensors_v2((uint32_t[2]){16, 17408}, 2, FLOAT32);
        int up_tile = alloc_tensors_v2((uint32_t[2]){16, 17408}, 2, FLOAT32);
        int down_tile = alloc_tensors_v2((uint32_t[2]){16, 5120}, 2, FLOAT32);
        for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            g_task_id++;
        }
        g_task_id++;

        for (int base = 0; base < 34; base += qwen3_blocks_per_task(34)) {
            g_task_id++;
            g_task_id++;
            g_task_id++;
        }
        
        for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            g_task_id++;
            g_task_id++;
        }
    }
}
