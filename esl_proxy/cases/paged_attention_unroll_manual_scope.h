// Orchestration Function: paged_attention_unroll (manual-scope variant).
//
// Mirrors V200-benchmark/paged_attention/paged_attention_unroll_manual_scope/
// kernels/orchestration/paged_attention_orch.cpp. Case1 parameters (README.md
// §2.2.1):
//   batch=480, num_heads=16, head_dim=128, block_size=128, context_len=8192,
//   N_UNROLL=64 -> 1 group × 4 tasks per (batch, q-tile) = 1920 tasks total.
//
// Per (batch, q-tile, bn) group — task 0..3 within the group:
//   task 0: qk_matmul      (CUBE)   succeed chain: (none) -> sf -> pv -> online
//   task 1: softmax_prep   (VECTOR) add_predecessors(sf, qk)
//   task 2: pv_matmul      (CUBE)   add_predecessors(pv, sf)
//   task 3: online_update  (VECTOR) add_predecessors(online, pv); optional add_predecessors(online, pre)
//
// Durations are per-subtask means (README.md §2.2.3 AICore View) in ns.
#include <stddef.h>
#include <stdint.h>


#include "dispatch.h"
#include "mem_pool.h"
#include "ring_buf.h"

extern atomic_int g_task_id;

/* fake kernel设计文档.md §5.2.2/§5.2.3 实测均值 (ns) + §5.2.4 mask */
#define DUR_PA_QK_MATMUL 51340U
#define DUR_PA_SOFTMAX_PREP 59130U
#define DUR_PA_PV_MATMUL 52600U
#define DUR_PA_ONLINE_UPDATE 2550U
#define MASK_PA_QK_MATMUL 8191U
#define MASK_PA_SOFTMAX_PREP 8191U
#define MASK_PA_PV_MATMUL 16383U
#define MASK_PA_ONLINE_UPDATE 2047U

int g_subtask_cnt = 0;

void aicpu_orchestration_entry(const uint64_t orch_args) {
    Tensor ext_query = tensor_from_base_layout(orch_args + 0, (uint32_t[]){7680, 128}, 2, BFLOAT16);
    Tensor ext_key_cache = tensor_from_base_layout(orch_args + 1, (uint32_t[]){3932160, 128}, 2, BFLOAT16);
    Tensor ext_value_cache = tensor_from_base_layout(orch_args + 2, (uint32_t[]){3932160, 128}, 2, BFLOAT16);
    Tensor ext_block_table = tensor_from_base_layout(orch_args + 3, (uint32_t[]){480, 64}, 2, INT32);
    Tensor ext_context_lens = tensor_from_base_layout(orch_args + 4, (uint32_t[]){480}, 1, INT32);
    Tensor ext_out = tensor_from_base_layout(orch_args + 5, (uint32_t[]){7680, 128}, 2, FLOAT32);
    Tensor oi = alloc_tensors((uint32_t[2]){16, 128}, 2, FLOAT32);
    Tensor li_update = alloc_tensors((uint32_t[2]){1, 16}, 2, FLOAT32);
    Tensor mi_update = alloc_tensors((uint32_t[2]){1, 16}, 2, FLOAT32);
    uint16_t task_preds[2];

    for (uint64_t b_idx = 0; b_idx < 480; b_idx++) {
        for (uint64_t q_idx = 0; q_idx < 1; q_idx++) {
            uint16_t pre_task_id = 0;
            int has_pre = 0;
            for (uint64_t bn = 0; bn < 64; bn += 64) {
                const uint64_t n_blocks = 64;
                const uint64_t is_first = (bn == 0) ? 1 : 0;
                const uint64_t is_last = (bn + n_blocks >= 64) ? 1 : 0;
                Tensor sij_buf = alloc_tensors((uint32_t[2]){16, 8192}, 2, FLOAT32);

                /* task 0: qk_matmul */
                new_task(g_task_id, TASK_TYPE_CUBE, 1, DUR_PA_QK_MATMUL, MASK_PA_QK_MATMUL);
                add_input(g_task_id, ext_query);
                add_input(g_task_id, ext_key_cache);
                add_input(g_task_id, ext_block_table);
                add_output(g_task_id, sij_buf);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)(b_idx * 64 + bn));
                const uint16_t qk_id = g_task_id;
                advance_task_id();
                Tensor pij_buf = alloc_tensors((uint32_t[2]){16, 8192}, 2, BFLOAT16);
                Tensor mi = alloc_tensors((uint32_t[2]){1, 16}, 2, FLOAT32);
                Tensor li = alloc_tensors((uint32_t[2]){1, 16}, 2, FLOAT32);

                /* task 1: softmax_prep */
                new_task(g_task_id, TASK_TYPE_VECTOR, 1, DUR_PA_SOFTMAX_PREP, MASK_PA_SOFTMAX_PREP);
                add_input(g_task_id, sij_buf);
                add_output(g_task_id, pij_buf);
                add_output(g_task_id, mi);
                add_output(g_task_id, li);
                add_scalar(g_task_id, 0x3f800000);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)128);
                task_preds[0] = qk_id;
                add_predecessors(g_task_id, task_preds, 1, 0);
                const uint16_t sf_id = g_task_id;
                advance_task_id();
                Tensor oi_new = alloc_tensors((uint32_t[2]){16, 128}, 2, FLOAT32);

                /* task 2: pv_matmul */
                new_task(g_task_id, TASK_TYPE_CUBE, 1, DUR_PA_PV_MATMUL, MASK_PA_PV_MATMUL);
                add_input(g_task_id, pij_buf);
                add_input(g_task_id, ext_value_cache);
                add_input(g_task_id, ext_block_table);
                add_output(g_task_id, oi_new);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)(b_idx * 64 + bn));
                task_preds[0] = sf_id;
                add_predecessors(g_task_id, task_preds, 1, 0);
                const uint16_t pv_id = g_task_id;
                advance_task_id();

                /* task 3: online_update */
                new_task(g_task_id, TASK_TYPE_VECTOR, 1, DUR_PA_ONLINE_UPDATE, MASK_PA_ONLINE_UPDATE);
                add_input(g_task_id, mi);
                add_input(g_task_id, li);
                add_input(g_task_id, oi_new);
                add_inout(g_task_id, mi_update);
                add_inout(g_task_id, li_update);
                add_inout(g_task_id, oi);
                add_inout(g_task_id, ext_out);
                add_scalar(g_task_id, (int64_t)is_first);
                add_scalar(g_task_id, (int64_t)is_last);
                task_preds[0] = pv_id;
                add_predecessors(g_task_id, task_preds, 1, 0);
                if (!is_first && has_pre) {
                    task_preds[0] = pre_task_id;
                    add_predecessors(g_task_id, task_preds, 1, 0);
                }
                pre_task_id = g_task_id;
                advance_task_id();
                has_pre = 1;
            }
        }
    }
}
