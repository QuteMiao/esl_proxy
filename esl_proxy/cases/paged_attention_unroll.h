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

#ifndef USE_TENSORMAP
#error "paged_attention_unroll.h requires -DUSE_TENSORMAP"
#endif

#define ORCH_TM_DEPS 1

#define ORCH_USES_TM_SUBMIT 1

#include "mem_pool.h"
#include "ring_buf.h"
#include "tensormap.h"

static inline void set_task_type(uint16_t task_id, task_type_t type) {
    g_basic_buf[task_id & RING_MASK].type = type;
}

void aicpu_orchestration_entry(uint64_t orch_args) {
    Tensor ext_query = tensor_from_base(orch_args + 0);
    Tensor ext_key_cache = tensor_from_base(orch_args + 1);
    Tensor ext_value_cache = tensor_from_base(orch_args + 2);
    Tensor ext_block_table = tensor_from_base(orch_args + 3);
    Tensor ext_context_lens = tensor_from_base(orch_args + 4);
    Tensor ext_out = tensor_from_base(orch_args + 5);
    (void)ext_context_lens;

    tm_deps_init();

    enum { pa_batch = 480,
        pa_q_tile = 16,
        pa_n_unroll = 64,
        pa_block_size = 128 };
    const uint32_t pa_block_elems = pa_n_unroll * pa_block_size;

    for (uint64_t b_idx = 0; b_idx < pa_batch; b_idx++) { // batch
        for (uint64_t q_idx = 0; q_idx < 1; q_idx++) {    // q_loop
            Tensor oi =
                alloc_tensors((uint32_t[2]){pa_q_tile, pa_block_size}, 2, FLOAT32);
            Tensor li_update =
                alloc_tensors((uint32_t[2]){1, pa_q_tile}, 2, FLOAT32);
            Tensor mi_update =
                alloc_tensors((uint32_t[2]){1, pa_q_tile}, 2, FLOAT32);

            for (uint64_t bn = 0; bn < pa_n_unroll; bn += pa_n_unroll) {
                uint64_t n_blocks = pa_n_unroll;
                uint64_t is_first = (bn == 0) ? 1 : 0;
                uint64_t is_last = (bn + n_blocks >= pa_n_unroll) ? 1 : 0;

                Tensor sij_buf = alloc_tensors((uint32_t[2]){pa_q_tile, pa_block_elems},
                    2, FLOAT32);

                /* task 0: qk_matmul */
                g_task_id++;
                while (!new_task(g_task_id)) {
                    spin_wait();
                }
                set_task_type(g_task_id, TASK_TYPE_CUBE);
                tm_in_ro(g_task_id, ext_query);
                tm_in_ro(g_task_id, ext_key_cache);
                tm_in_ro(g_task_id, ext_block_table);
                tm_out(g_task_id, sij_buf);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)(b_idx * 64 + bn));
                add_duration(g_task_id, 51630); // dur_qk_matmul (ns)
                tm_submit(g_task_id);

                Tensor pij_buf =
                    alloc_tensors((uint32_t[2]){pa_q_tile, pa_block_elems}, 2, BFLOAT16);
                Tensor mi = alloc_tensors((uint32_t[2]){1, pa_q_tile}, 2, FLOAT32);
                Tensor li = alloc_tensors((uint32_t[2]){1, pa_q_tile}, 2, FLOAT32);

                /* task 1: softmax_prep */
                g_task_id++;
                while (!new_task(g_task_id)) {
                    spin_wait();
                }
                set_task_type(g_task_id, TASK_TYPE_VECTOR);
                tm_in(g_task_id, sij_buf);
                tm_out(g_task_id, pij_buf);
                tm_out(g_task_id, mi);
                tm_out(g_task_id, li);
                add_scalar(g_task_id, 0x3f800000); // float 1.0 (attention scale)
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)128); // block_size
                add_duration(g_task_id, 58820);      // dur_softmax_prep (ns)
                tm_submit(g_task_id);

                Tensor oi_new =
                    alloc_tensors((uint32_t[2]){pa_q_tile, pa_block_size}, 2, FLOAT32);

                /* task 2: pv_matmul */
                g_task_id++;
                while (!new_task(g_task_id)) {
                    spin_wait();
                }
                set_task_type(g_task_id, TASK_TYPE_CUBE);
                tm_in(g_task_id, pij_buf);
                tm_in_ro(g_task_id, ext_value_cache);
                tm_in_ro(g_task_id, ext_block_table);
                tm_out(g_task_id, oi_new);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)(b_idx * 64 + bn));
                add_duration(g_task_id, 52610); // dur_pv_matmul (ns)
                tm_submit(g_task_id);

                /* task 3: online_update */
                g_task_id++;
                while (!new_task(g_task_id)) {
                    spin_wait();
                }
                set_task_type(g_task_id, TASK_TYPE_VECTOR);
                tm_in_ro(g_task_id, mi);
                tm_in_ro(g_task_id, li);
                tm_in(g_task_id, oi_new);
                tm_inout(g_task_id, mi_update);
                tm_inout(g_task_id, li_update);
                tm_inout(g_task_id, oi);
                tm_inout_ro(g_task_id, ext_out);
                add_scalar(g_task_id, (int64_t)is_first);
                add_scalar(g_task_id, (int64_t)is_last);
                add_duration(g_task_id, 2560); // dur_online_update (ns)
                tm_submit(g_task_id);
            }
        }
    }
}
