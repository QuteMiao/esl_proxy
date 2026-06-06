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
// Cross-group online softmax accumulation is wired via mi/li/oi INOUT tensormap
// edges. Durations are per-subtask means (README.md §2.2.3 AICore View) in ns.
//
// Per-batch qi/out_view/block_table_row use tensor_make_2d(base + row_offset) so
// each batch gets a distinct tensormap base_addr (same idea as k_cache_local in
// qwen3_dynamic_tensormap.h). Scratch pools are pre-allocated before the loop.
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

  ext_query = tensor_make_2d(tensor_base(ext_query), 480 * 16, 128, BFLOAT16);  // batch × num_heads, head_dim
  ext_out = tensor_make_2d(tensor_base(ext_out), 480 * 16, 128, FLOAT32);     // batch × num_heads, head_dim
  ext_block_table = tensor_make_2d(tensor_base(ext_block_table), 480, 64, FLOAT32);  // batch, block_num (4B elems)

  enum { pa_batch = 480, pa_q_tile = 16, pa_n_unroll = 64, pa_block_size = 128 };
  const uint32_t pa_block_elems = pa_n_unroll * pa_block_size;

  Tensor oi_pool[pa_batch];
  Tensor li_upd_pool[pa_batch];
  Tensor mi_upd_pool[pa_batch];
  Tensor sij_pool[pa_batch];
  Tensor pij_pool[pa_batch];
  Tensor mi_pool[pa_batch];
  Tensor li_pool[pa_batch];
  Tensor oi_new_pool[pa_batch];

  for (uint64_t b = 0; b < pa_batch; b++) {
    oi_pool[b] = alloc_tensors((uint32_t[2]){pa_q_tile, pa_block_size}, 2, FLOAT32);
    li_upd_pool[b] = alloc_tensors((uint32_t[2]){1, pa_q_tile}, 2, FLOAT32);
    mi_upd_pool[b] = alloc_tensors((uint32_t[2]){1, pa_q_tile}, 2, FLOAT32);
    sij_pool[b] =
        alloc_tensors((uint32_t[2]){pa_q_tile, pa_block_elems}, 2, FLOAT32);
    pij_pool[b] =
        alloc_tensors((uint32_t[2]){pa_q_tile, pa_block_elems}, 2, BFLOAT16);
    mi_pool[b] = alloc_tensors((uint32_t[2]){1, pa_q_tile}, 2, FLOAT32);
    li_pool[b] = alloc_tensors((uint32_t[2]){1, pa_q_tile}, 2, FLOAT32);
    oi_new_pool[b] =
        alloc_tensors((uint32_t[2]){pa_q_tile, pa_block_size}, 2, FLOAT32);
  }

  for (uint64_t b_idx = 0; b_idx < pa_batch; b_idx++) {  // batch
    for (uint64_t q_idx = 0; q_idx < 1; q_idx++) {  // q_loop
      Tensor oi = oi_pool[b_idx];
      Tensor li_update = li_upd_pool[b_idx];
      Tensor mi_update = mi_upd_pool[b_idx];

      uint64_t cur_offset = b_idx * pa_q_tile + q_idx * pa_q_tile;
      Tensor qi = tensor_make_2d(
          tensor_base(ext_query) +
              cur_offset * (uint64_t)pa_block_size * (uint64_t)BFLOAT16,
          pa_q_tile, pa_block_size, BFLOAT16);
      Tensor out_view = tensor_make_2d(
          tensor_base(ext_out) +
              cur_offset * (uint64_t)pa_block_size * (uint64_t)FLOAT32,
          pa_q_tile, pa_block_size, FLOAT32);
      Tensor block_table_row = tensor_make_2d(
          tensor_base(ext_block_table) + b_idx * 64u * (uint64_t)FLOAT32, 1, 64,
          FLOAT32);

      for (uint64_t bn = 0; bn < pa_n_unroll; bn += pa_n_unroll) {
        uint64_t n_blocks = pa_n_unroll;
        uint64_t is_first = (bn == 0) ? 1 : 0;
        uint64_t is_last = (bn + n_blocks >= pa_n_unroll) ? 1 : 0;

        Tensor sij_buf = sij_pool[b_idx];

        /* task 0: qk_matmul */
        g_task_id++;
        while (!try_new_task(g_task_id)) {
          spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        tm_in(g_task_id, qi);
        tm_in_ro(g_task_id, ext_key_cache);
        tm_in_ro(g_task_id, block_table_row);
        tm_out(g_task_id, sij_buf);
        add_scalar(g_task_id, (int64_t)n_blocks);
        add_scalar(g_task_id, (int64_t)(b_idx * 64 + bn));  // b_idx * block_num + bn
        add_duration(g_task_id, 51630);  // dur_qk_matmul (ns)
        tm_submit(g_task_id);

        Tensor pij_buf = pij_pool[b_idx];
        Tensor mi = mi_pool[b_idx];
        Tensor li = li_pool[b_idx];

        /* task 1: softmax_prep */
        g_task_id++;
        while (!try_new_task(g_task_id)) {
          spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        tm_in(g_task_id, sij_buf);
        tm_out(g_task_id, pij_buf);
        tm_out(g_task_id, mi);
        tm_out(g_task_id, li);
        add_scalar(g_task_id, 0x3f800000);  // float 1.0 (attention scale)
        add_scalar(g_task_id, (int64_t)n_blocks);
        add_scalar(g_task_id, (int64_t)128);  // block_size
        add_duration(g_task_id, 58820);  // dur_softmax_prep (ns)
        tm_submit(g_task_id);

        Tensor oi_new = oi_new_pool[b_idx];

        /* task 2: pv_matmul */
        g_task_id++;
        while (!try_new_task(g_task_id)) {
          spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        tm_in(g_task_id, pij_buf);
        tm_in_ro(g_task_id, ext_value_cache);
        tm_in_ro(g_task_id, block_table_row);
        tm_out(g_task_id, oi_new);
        add_scalar(g_task_id, (int64_t)n_blocks);
        add_scalar(g_task_id, (int64_t)(b_idx * 64 + bn));  // b_idx * block_num + bn
        add_duration(g_task_id, 52610);  // dur_pv_matmul (ns)
        tm_submit(g_task_id);

        /* task 3: online_update */
        g_task_id++;
        while (!try_new_task(g_task_id)) {
          spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        tm_in_ro(g_task_id, mi);
        tm_in_ro(g_task_id, li);
        tm_in(g_task_id, oi_new);
        tm_inout(g_task_id, mi_update);
        tm_inout(g_task_id, li_update);
        tm_inout(g_task_id, oi);
        tm_inout(g_task_id, out_view);
        add_scalar(g_task_id, (int64_t)is_first);
        add_scalar(g_task_id, (int64_t)is_last);
        add_duration(g_task_id, 2560);  // dur_online_update (ns)
        tm_submit(g_task_id);
      }
    }
  }
}
