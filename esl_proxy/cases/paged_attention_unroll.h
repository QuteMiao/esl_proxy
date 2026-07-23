// Orchestration Function: paged_attention_unroll (tensormap auto-dependency
// variant).
//
// Mirrors V200-benchmark/paged_attention/paged_attention_unroll/
// kernels/orchestration/paged_attention_orch.cpp. Case1 parameters (README.md
// §2.2.1):
//   batch=480, num_heads=16, head_dim=128, block_size=128, context_len=8192,
//   N_UNROLL=64 -> 1 group × 4 tasks per (batch, q-tile) = 1920 tasks total.
//
// Per (batch, q-tile, bn) group — task 0..3 within the group:
//   task 0: qk_matmul      (CUBE)   qi × K^T -> sij_buf
//   task 1: softmax_prep   (VECTOR) sij_buf -> pij_buf, mi, li
//   task 2: pv_matmul      (CUBE)   pij_buf × V -> oi_new
//   task 3: online_update  (VECTOR) mi/li/oi_new + inout mi/li/oi/out_view
//
// External tensors (ext_query, ext_key/value_cache, ext_block_table, ext_out)
// use tm_*_ro only — not registered in tensormap. Scratch buffers use
// alloc_tensors at point of use (no pre-allocated pools).
#include <stddef.h>
#include <stdint.h>


#include "mem_pool.h"
#include "tensormap.h"

int g_subtask_cnt = 0;

static inline void set_task_type(uint32_t task_id, task_type_t type) {
    g_basic_buf[task_id & RING_MASK].type = type;
}

void aicpu_orchestration_entry(uint64_t orch_args) {
    Tensor ext_query = tensor_from_base_layout(orch_args + 0, (uint32_t[]){7680, 128}, 2, BFLOAT16); // batch*num_heads=480*16, head_dim=128
    Tensor ext_key_cache = tensor_from_base_layout(orch_args + 1, (uint32_t[]){3932160, 128}, 2, BFLOAT16); // total_blocks*block_size=30720*128, head_dim=128
    Tensor ext_value_cache = tensor_from_base_layout(orch_args + 2, (uint32_t[]){3932160, 128}, 2, BFLOAT16); // total_blocks*block_size=30720*128, head_dim=128
    Tensor ext_block_table = tensor_from_base_layout(orch_args + 3, (uint32_t[]){480, 64}, 2, INT32); // batch=480, max_blocks=64
    Tensor ext_context_lens = tensor_from_base_layout(orch_args + 4, (uint32_t[]){480}, 1, INT32); // batch=480
    Tensor ext_out = tensor_from_base_layout(orch_args + 5, (uint32_t[]){7680, 128}, 2, FLOAT32); // batch*num_heads=480*16, head_dim=128
    (void)ext_context_lens;
    tm_deps_init();
    for (uint64_t b_idx = 0; b_idx < 480; b_idx++) { // batch=480
        for (uint64_t q_idx = 0; q_idx < 1; q_idx++) { // q_loop=1
            Tensor oi = alloc_tensors((uint32_t[2]){16, 128}, 2, FLOAT32); // q_tile=16, block_size=128
            Tensor li_update = alloc_tensors((uint32_t[2]){1, 16}, 2, FLOAT32); // q_tile=16
            Tensor mi_update = alloc_tensors((uint32_t[2]){1, 16}, 2, FLOAT32); // q_tile=16
            for (uint64_t bn = 0; bn < 64; bn += 64) { // n_unroll=64
                uint64_t n_blocks = 64; // n_unroll=64
                uint64_t is_first = (bn == 0) ? 1 : 0;
                uint64_t is_last = (bn + n_blocks >= 64) ? 1 : 0; // n_unroll=64
                Tensor sij_buf = alloc_tensors((uint32_t[2]){16, 8192}, 2, FLOAT32); // q_tile=16, n_unroll*block_size=64*128

                /* task 0: qk_matmul */
                new_task(g_task_id, TASK_TYPE_CUBE, 1, 51630);
                tm_in_ro(g_task_id, ext_query);
                tm_in_ro(g_task_id, ext_key_cache);
                tm_in_ro(g_task_id, ext_block_table);
                tm_out(g_task_id, sij_buf);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)(b_idx * 64 + bn)); // max_blocks=64
                tm_submit(g_task_id);
        g_task_id++;
                Tensor pij_buf = alloc_tensors((uint32_t[2]){16, 8192}, 2, BFLOAT16); // q_tile=16, n_unroll*block_size=64*128
                Tensor mi = alloc_tensors((uint32_t[2]){1, 16}, 2, FLOAT32); // q_tile=16
                Tensor li = alloc_tensors((uint32_t[2]){1, 16}, 2, FLOAT32); // q_tile=16

                /* task 1: softmax_prep */
                new_task(g_task_id, TASK_TYPE_VECTOR, 1, 58820);
                tm_in(g_task_id, sij_buf);
                tm_out(g_task_id, pij_buf);
                tm_out(g_task_id, mi);
                tm_out(g_task_id, li);
                add_scalar(g_task_id, 0x3f800000); // float 1.0 (attention scale)
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)128); // block_size=128
                tm_submit(g_task_id);
        g_task_id++;
                Tensor oi_new = alloc_tensors((uint32_t[2]){16, 128}, 2, FLOAT32); // q_tile=16, block_size=128

                /* task 2: pv_matmul */
                new_task(g_task_id, TASK_TYPE_CUBE, 1, 52610);
                tm_in(g_task_id, pij_buf);
                tm_in_ro(g_task_id, ext_value_cache);
                tm_in_ro(g_task_id, ext_block_table);
                tm_out(g_task_id, oi_new);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)(b_idx * 64 + bn)); // max_blocks=64
                tm_submit(g_task_id);
        g_task_id++;

                /* task 3: online_update */
                new_task(g_task_id, TASK_TYPE_VECTOR, 1, 2560);
                tm_in_ro(g_task_id, mi);
                tm_in_ro(g_task_id, li);
                tm_in(g_task_id, oi_new);
                tm_inout(g_task_id, mi_update);
                tm_inout(g_task_id, li_update);
                tm_inout(g_task_id, oi);
                tm_inout_ro(g_task_id, ext_out);
                add_scalar(g_task_id, (int64_t)is_first);
                add_scalar(g_task_id, (int64_t)is_last);
                tm_submit(g_task_id);
        g_task_id++;
            }
        }
    }
}
