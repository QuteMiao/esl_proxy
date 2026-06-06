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

#ifndef USE_TENSORMAP
#error "qwen3_dynamic_tensormap.h requires -DUSE_TENSORMAP"
#endif

#define ORCH_TM_DEPS 1

#define ORCH_USES_TM_SUBMIT 1

#include "mem_pool.h"
#include "ring_buf.h"
#include "tensormap.h"

#ifndef QWEN3_SPMD_TIER
#define QWEN3_SPMD_TIER 4
#endif
#if QWEN3_SPMD_TIER < 0 || QWEN3_SPMD_TIER > 4
#error "QWEN3_SPMD_TIER must be 0..4"
#endif

#define DUR_RMSNORM 23950
#define DUR_Q_PROJ 26060
#define DUR_K_PROJ 18170
#define DUR_V_PROJ 17890
#define DUR_QK_NORM 13190
#define DUR_ROPE_KV_CACHE 9480
#define DUR_QK_MATMUL 29350
#define DUR_SOFTMAX 19400
#define DUR_SV_MATMUL 31650
#define DUR_ONLINE_SOFTMAX 20820
#define DUR_OUT_PROJ 40750
#define DUR_POST_RMSNORM 24390
#define DUR_GATE_PROJ 95700
#define DUR_UP_PROJ 97140
#define DUR_SILU 2820
#define DUR_DOWN_PROJ 72220
#define DUR_DOWN_PROJ_RES 2590

static inline void set_task_type(uint16_t task_id, task_type_t type) {
  g_basic_buf[task_id & RING_MASK].type = type;
}

static inline void set_block_num(uint16_t task_id, uint32_t count) {
  g_basic_buf[task_id & RING_MASK].mode = ORG_MODE_SPMD_SYNC;
  g_basic_buf[task_id & RING_MASK].count = count;
}

static inline int qwen3_blocks_per_task(int total_chunks) {
  static const int targets[5] = {1, 2, 4, 8, 1 << 30};
  int target = targets[QWEN3_SPMD_TIER];
  return total_chunks < target ? total_chunks : target;
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
  Tensor ext_hidden_states = tensor_from_base(orch_args + 0);
  Tensor ext_input_rms_weight = tensor_from_base(orch_args + 1);
  Tensor ext_wq = tensor_from_base(orch_args + 2);
  Tensor ext_wk = tensor_from_base(orch_args + 3);
  Tensor ext_wv = tensor_from_base(orch_args + 4);
  Tensor ext_q_norm_weight = tensor_from_base(orch_args + 5);
  Tensor ext_k_norm_weight = tensor_from_base(orch_args + 6);
  Tensor ext_seq_lens = tensor_from_base(orch_args + 7);
  Tensor ext_block_table = tensor_from_base(orch_args + 8);
  Tensor ext_slot_mapping = tensor_from_base(orch_args + 9);
  Tensor ext_rope_cos = tensor_from_base(orch_args + 10);
  Tensor ext_rope_sin = tensor_from_base(orch_args + 11);
  Tensor ext_k_cache = tensor_from_base(orch_args + 12);
  Tensor ext_v_cache = tensor_from_base(orch_args + 13);
  Tensor ext_wo = tensor_from_base(orch_args + 14);
  Tensor ext_post_rms_weight = tensor_from_base(orch_args + 15);
  Tensor ext_w_gate = tensor_from_base(orch_args + 16);
  Tensor ext_w_up = tensor_from_base(orch_args + 17);
  Tensor ext_w_down = tensor_from_base(orch_args + 18);
  Tensor ext_out = tensor_from_base(orch_args + 19);
  (void)ext_seq_lens;
  (void)ext_slot_mapping;

  tm_deps_init();

  const int64_t user_batch = 90;
  const int64_t batch_padded = (((user_batch + 15) / 16) * 16);
  ext_out = tensor_make_2d(tensor_base(ext_out), (uint32_t)batch_padded, 5120, BFLOAT16);
  (void)alloc_tensors((uint32_t[2]){11520, 128}, 2, BFLOAT16);
  Tensor q_proj = alloc_tensors((uint32_t[2]){batch_padded, 5120}, 2, FLOAT32);
  Tensor k_proj = alloc_tensors((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
  Tensor v_proj = alloc_tensors((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
  Tensor q_proj_norm = alloc_tensors((uint32_t[2]){batch_padded, 5120}, 2, FLOAT32);
  Tensor k_proj_norm = alloc_tensors((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);

  for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
    Tensor normed_tile = alloc_tensors((uint32_t[2]){16, 5120}, 2, BFLOAT16);
    const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);

    g_task_id++;
    while (!try_new_task(g_task_id)) {
      spin_wait();
    }
    set_task_type(g_task_id, TASK_TYPE_VECTOR);
    tm_in_ro(g_task_id, ext_hidden_states);
    tm_out(g_task_id, normed_tile);
    tm_in_ro(g_task_id, ext_input_rms_weight);
    add_scalar(g_task_id, b0);
    add_scalar(g_task_id, cur_valid);
    add_duration(g_task_id, DUR_RMSNORM);
    tm_submit(g_task_id);

    for (int base = 0; base < 20; base += qwen3_blocks_per_task(20)) {
      // 20: q_proj SPMD total chunks; cols/chunk = 5120/20 = 256
      int cur_blocks = qwen3_blocks_per_task(20) < (20 - base) ? qwen3_blocks_per_task(20) : (20 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_CUBE);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor q_chunk = tensor_chunk_2d(q_proj, (uint32_t)b0, 16u,
                                       (uint32_t)(base * 256u),
                                       (uint32_t)(cur_blocks * 256u), FLOAT32);
      tm_in(g_task_id, normed_tile);
      tm_in_ro(g_task_id, ext_wq);
      tm_out(g_task_id, q_chunk);
      add_scalar(g_task_id, b0);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_Q_PROJ);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 8; base += qwen3_blocks_per_task(8)) {
      // 8: k_proj SPMD total chunks; cols/chunk = 1024/8 = 128
      int cur_blocks = qwen3_blocks_per_task(8) < (8 - base) ? qwen3_blocks_per_task(8) : (8 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_CUBE);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor k_chunk = tensor_chunk_2d(k_proj, (uint32_t)b0, 16u,
                                       (uint32_t)(base * 128u),
                                       (uint32_t)(cur_blocks * 128u), FLOAT32);
      tm_in(g_task_id, normed_tile);
      tm_in_ro(g_task_id, ext_wk);
      tm_out(g_task_id, k_chunk);
      add_scalar(g_task_id, b0);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_K_PROJ);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 8; base += qwen3_blocks_per_task(8)) {
      // 8: v_proj SPMD total chunks; cols/chunk = 1024/8 = 128
      int cur_blocks = qwen3_blocks_per_task(8) < (8 - base) ? qwen3_blocks_per_task(8) : (8 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_CUBE);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor v_chunk = tensor_chunk_2d(v_proj, (uint32_t)b0, 16u,
                                       (uint32_t)(base * 128u),
                                       (uint32_t)(cur_blocks * 128u), FLOAT32);
      tm_in(g_task_id, normed_tile);
      tm_in_ro(g_task_id, ext_wv);
      tm_out(g_task_id, v_chunk);
      add_scalar(g_task_id, b0);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_V_PROJ);
      tm_submit(g_task_id);
    }

    g_task_id++;
    while (!try_new_task(g_task_id)) {
      spin_wait();
    }
    set_task_type(g_task_id, TASK_TYPE_VECTOR);
    Tensor k0_norm = tensor_row_chunk(k_proj_norm, (uint32_t)b0, 16u);
    Tensor q0_norm = tensor_row_chunk(q_proj_norm, (uint32_t)b0, 16u);
    Tensor q0_in = tensor_row_chunk(q_proj, (uint32_t)b0, 16u);
    Tensor k0_in = tensor_row_chunk(k_proj, (uint32_t)b0, 16u);
    tm_out(g_task_id, k0_norm);
    tm_out(g_task_id, q0_norm);
    tm_in(g_task_id, q0_in);
    tm_in_ro(g_task_id, ext_q_norm_weight);
    tm_in_ro(g_task_id, ext_k_norm_weight);
    tm_in(g_task_id, k0_in);
    add_duration(g_task_id, DUR_QK_NORM);
    tm_submit(g_task_id);
  }

  Tensor attn_out = alloc_tensors((uint32_t[2]){batch_padded, 5120}, 2, BFLOAT16);

  for (int64_t b = 0; b < user_batch; b += 1) {
    Tensor all_raw_scores = alloc_tensors((uint32_t[2]){4096, 128}, 2, FLOAT32);
    Tensor all_exp_padded = alloc_tensors((uint32_t[2]){4096, 128}, 2, BFLOAT16);
    Tensor all_cur_mi = alloc_tensors((uint32_t[2]){4096, 1}, 2, FLOAT32);
    Tensor all_cur_li = alloc_tensors((uint32_t[2]){4096, 1}, 2, FLOAT32);
    Tensor all_oi_tmp = alloc_tensors((uint32_t[2]){4096, 128}, 2, FLOAT32);
    Tensor q_padded_local = alloc_tensors((uint32_t[2]){128, 128}, 2, BFLOAT16);
    Tensor k_cache_local = tensor_make_2d(tensor_base(ext_k_cache) + (uint64_t)b * (uint64_t)1024u * (uint64_t)BFLOAT16, 1, 1024, BFLOAT16);
    Tensor v_cache_local = tensor_make_2d(tensor_base(ext_v_cache) + (uint64_t)b * (uint64_t)1024u * (uint64_t)BFLOAT16, 1, 1024, BFLOAT16);

    const int64_t b_tile0 = (b / 16) * 16;
    const int64_t slot = b;
    const int64_t slot_block = slot / 128;
    const int64_t slot_offset = slot - slot_block * 128;

    g_task_id++;
    while (!try_new_task(g_task_id)) {
      spin_wait();
    }
    set_task_type(g_task_id, TASK_TYPE_VECTOR);
    Tensor k0_norm = tensor_row_chunk(k_proj_norm, (uint32_t)b_tile0, 16u);
    Tensor v0 = tensor_row_chunk(v_proj, (uint32_t)b_tile0, 16u);
    Tensor q0_norm = tensor_row_chunk(q_proj_norm, (uint32_t)b_tile0, 16u);
    tm_out(g_task_id, q_padded_local);
    tm_out(g_task_id, k_cache_local);
    tm_out(g_task_id, v_cache_local);
    tm_in(g_task_id, k0_norm);
    tm_in_ro(g_task_id, ext_rope_cos);
    tm_in_ro(g_task_id, ext_rope_sin);
    tm_in_ro(g_task_id, ext_rope_cos);
    tm_in_ro(g_task_id, ext_rope_sin);
    tm_in(g_task_id, v0);
    tm_in(g_task_id, q0_norm);
    add_scalar(g_task_id, slot_block);
    add_scalar(g_task_id, slot_offset);
    add_scalar(g_task_id, b);
    add_duration(g_task_id, DUR_ROPE_KV_CACHE);
    tm_submit(g_task_id);

    for (int base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
      // 4: qk_matmul SPMD total chunks; rows/chunk = 4096/4 = 1024
      int cur_blocks = qwen3_blocks_per_task(4) < (4 - base) ? qwen3_blocks_per_task(4) : (4 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_CUBE);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor raw_scores_chunk =
          tensor_row_chunk(all_raw_scores, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      tm_in(g_task_id, q_padded_local);
      tm_out(g_task_id, raw_scores_chunk);
      tm_in_ro(g_task_id, ext_block_table);
      tm_in(g_task_id, k_cache_local);
      add_scalar(g_task_id, b);
      add_scalar(g_task_id, 8);       // (1024+127)/128: KV context blocks
      add_scalar(g_task_id, b * 32);  // block_table row offset for batch b
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_QK_MATMUL);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
      // 4: softmax SPMD total chunks; rows/chunk = 4096/4 = 1024
      int cur_blocks = qwen3_blocks_per_task(4) < (4 - base) ? qwen3_blocks_per_task(4) : (4 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_VECTOR);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor cur_li_chunk =
          tensor_row_chunk(all_cur_li, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      Tensor cur_mi_chunk =
          tensor_row_chunk(all_cur_mi, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      Tensor exp_padded_chunk =
          tensor_row_chunk(all_exp_padded, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      Tensor raw_scores_chunk =
          tensor_row_chunk(all_raw_scores, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      tm_out(g_task_id, cur_li_chunk);
      tm_out(g_task_id, cur_mi_chunk);
      tm_out(g_task_id, exp_padded_chunk);
      tm_in(g_task_id, raw_scores_chunk);
      add_scalar(g_task_id, 8);     // (1024+127)/128: KV context blocks
      add_scalar(g_task_id, 1024);  // context length (tokens)
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_SOFTMAX);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
      // 4: sv_matmul SPMD total chunks; rows/chunk = 4096/4 = 1024
      int cur_blocks = qwen3_blocks_per_task(4) < (4 - base) ? qwen3_blocks_per_task(4) : (4 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_CUBE);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor oi_tmp_chunk =
          tensor_row_chunk(all_oi_tmp, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      Tensor exp_padded_chunk =
          tensor_row_chunk(all_exp_padded, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      tm_out(g_task_id, oi_tmp_chunk);
      tm_in_ro(g_task_id, ext_block_table);
      tm_in(g_task_id, exp_padded_chunk);
      tm_in(g_task_id, v_cache_local);
      add_scalar(g_task_id, 8);       // (1024+127)/128: KV context blocks
      add_scalar(g_task_id, b * 32);  // block_table row offset for batch b
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_SV_MATMUL);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
      // 4: online_softmax SPMD total chunks; cols/chunk = 5120/4 = 1280
      int cur_blocks = qwen3_blocks_per_task(4) < (4 - base) ? qwen3_blocks_per_task(4) : (4 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_VECTOR);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor oi_tmp_chunk =
          tensor_row_chunk(all_oi_tmp, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      Tensor cur_mi_chunk =
          tensor_row_chunk(all_cur_mi, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      Tensor cur_li_chunk =
          tensor_row_chunk(all_cur_li, (uint32_t)(base * 1024),
                           (uint32_t)(cur_blocks * 1024));
      Tensor attn_out_chunk = tensor_view(
          tensor_view(attn_out, 0u, (uint32_t)b, 1u), 1u,
          (uint32_t)(base * 1280u), (uint32_t)(cur_blocks * 1280u));
      tm_in(g_task_id, oi_tmp_chunk);
      tm_in(g_task_id, cur_mi_chunk);
      tm_in(g_task_id, cur_li_chunk);
      tm_inout(g_task_id, attn_out_chunk);
      add_scalar(g_task_id, 8);  // (1024+127)/128: KV context blocks
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_ONLINE_SOFTMAX);
      tm_submit(g_task_id);
    }
  }

  for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
    Tensor resid1_tile = alloc_tensors((uint32_t[2]){16, 5120}, 2, FLOAT32);
    Tensor gm_pipe_buffer_0 = alloc_tensors((uint32_t[2]){16384, 40}, 2, FLOAT32);
    Tensor post_norm_tile = alloc_tensors((uint32_t[2]){16, 5120}, 2, BFLOAT16);
    Tensor mlp_tile = alloc_tensors((uint32_t[2]){16, 17408}, 2, BFLOAT16);
    Tensor gate_tile = alloc_tensors((uint32_t[2]){16, 17408}, 2, FLOAT32);
    Tensor up_tile = alloc_tensors((uint32_t[2]){16, 17408}, 2, FLOAT32);
    Tensor down_tile = alloc_tensors((uint32_t[2]){16, 5120}, 2, FLOAT32);
    const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);

    for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
      // 40: out_proj SPMD total chunks; cols/chunk = 5120/40 = 128
      int cur_blocks = qwen3_blocks_per_task(40) < (40 - base) ? qwen3_blocks_per_task(40) : (40 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_MIX);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor hidden_states_chunk =
          tensor_col_chunk(ext_hidden_states, (uint32_t)(base * 128u),
                           (uint32_t)(cur_blocks * 128u));
      Tensor attn_out_tile =
          tensor_view(attn_out, 0u, (uint32_t)b0, (uint32_t)cur_valid);
      Tensor wo_chunk = tensor_col_chunk(ext_wo, (uint32_t)(base * 128u),
                                         (uint32_t)(cur_blocks * 128u));
      Tensor resid1_chunk =
          tensor_col_chunk(resid1_tile, (uint32_t)(base * 128u),
                           (uint32_t)(cur_blocks * 128u));
      tm_in_ro(g_task_id, hidden_states_chunk);
      tm_in(g_task_id, attn_out_tile);
      tm_in_ro(g_task_id, wo_chunk);
      tm_inout(g_task_id, resid1_chunk);
      tm_out(g_task_id, gm_pipe_buffer_0);
      add_scalar(g_task_id, b0);
      add_scalar(g_task_id, cur_valid);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_OUT_PROJ);
      tm_submit(g_task_id);
    }

    g_task_id++;
    while (!try_new_task(g_task_id)) {
      spin_wait();
    }
    set_task_type(g_task_id, TASK_TYPE_VECTOR);
    tm_in(g_task_id, resid1_tile);
    tm_out(g_task_id, post_norm_tile);
    tm_in_ro(g_task_id, ext_post_rms_weight);
    add_duration(g_task_id, DUR_POST_RMSNORM);
    tm_submit(g_task_id);

    for (int base = 0; base < 34; base += qwen3_blocks_per_task(34)) {
      // 34: gate_proj SPMD total chunks; cols/chunk = 17408/34 = 512
      int cur_blocks = qwen3_blocks_per_task(34) < (34 - base) ? qwen3_blocks_per_task(34) : (34 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_CUBE);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor w_gate_chunk = tensor_col_chunk(ext_w_gate, (uint32_t)(base * 512u),
                                           (uint32_t)(cur_blocks * 512u));
      Tensor gate_chunk = tensor_col_chunk(gate_tile, (uint32_t)(base * 512u),
                                          (uint32_t)(cur_blocks * 512u));
      tm_in(g_task_id, post_norm_tile);
      tm_in_ro(g_task_id, w_gate_chunk);
      tm_inout(g_task_id, gate_chunk);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_GATE_PROJ);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 34; base += qwen3_blocks_per_task(34)) {
      // 34: up_proj SPMD total chunks; cols/chunk = 17408/34 = 512
      int cur_blocks = qwen3_blocks_per_task(34) < (34 - base) ? qwen3_blocks_per_task(34) : (34 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_CUBE);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor w_up_chunk = tensor_col_chunk(ext_w_up, (uint32_t)(base * 512u),
                                          (uint32_t)(cur_blocks * 512u));
      Tensor up_chunk = tensor_col_chunk(up_tile, (uint32_t)(base * 512u),
                                         (uint32_t)(cur_blocks * 512u));
      tm_in(g_task_id, post_norm_tile);
      tm_in_ro(g_task_id, w_up_chunk);
      tm_inout(g_task_id, up_chunk);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_UP_PROJ);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 34; base += qwen3_blocks_per_task(34)) {
      // 34: silu SPMD total chunks; cols/chunk = 17408/34 = 512
      int cur_blocks = qwen3_blocks_per_task(34) < (34 - base) ? qwen3_blocks_per_task(34) : (34 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_VECTOR);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor gate_chunk = tensor_col_chunk(gate_tile, (uint32_t)(base * 512u),
                                           (uint32_t)(cur_blocks * 512u));
      Tensor up_chunk = tensor_col_chunk(up_tile, (uint32_t)(base * 512u),
                                         (uint32_t)(cur_blocks * 512u));
      Tensor mlp_chunk = tensor_col_chunk(mlp_tile, (uint32_t)(base * 512u),
                                          (uint32_t)(cur_blocks * 512u));
      tm_in(g_task_id, gate_chunk);
      tm_in(g_task_id, up_chunk);
      tm_inout(g_task_id, mlp_chunk);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_SILU);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
      // 40: down_proj SPMD total chunks; cols/chunk = 5120/40 = 128
      int cur_blocks = qwen3_blocks_per_task(40) < (40 - base) ? qwen3_blocks_per_task(40) : (40 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_CUBE);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor w_down_chunk = tensor_col_chunk(ext_w_down, (uint32_t)(base * 128u),
                                           (uint32_t)(cur_blocks * 128u));
      Tensor down_chunk = tensor_col_chunk(down_tile, (uint32_t)(base * 128u),
                                           (uint32_t)(cur_blocks * 128u));
      tm_in(g_task_id, mlp_tile);
      tm_in_ro(g_task_id, w_down_chunk);
      tm_inout(g_task_id, down_chunk);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_DOWN_PROJ);
      tm_submit(g_task_id);
    }

    for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
      // 40: down_proj_res SPMD total chunks; cols/chunk = 5120/40 = 128
      int cur_blocks = qwen3_blocks_per_task(40) < (40 - base) ? qwen3_blocks_per_task(40) : (40 - base);
      g_task_id++;
      while (!try_new_task(g_task_id)) {
        spin_wait();
      }
      set_task_type(g_task_id, TASK_TYPE_VECTOR);
      set_block_num(g_task_id, (uint32_t)cur_blocks);
      Tensor down_chunk = tensor_col_chunk(down_tile, (uint32_t)(base * 128u),
                                           (uint32_t)(cur_blocks * 128u));
      Tensor resid1_chunk =
          tensor_col_chunk(resid1_tile, (uint32_t)(base * 128u),
                           (uint32_t)(cur_blocks * 128u));
      Tensor out_chunk = tensor_chunk_2d(ext_out, (uint32_t)b0, (uint32_t)cur_valid,
                                         (uint32_t)(base * 128u),
                                         (uint32_t)(cur_blocks * 128u), BFLOAT16);
      tm_in(g_task_id, down_chunk);
      tm_in(g_task_id, resid1_chunk);
      tm_out(g_task_id, out_chunk);
      add_scalar(g_task_id, cur_valid);
      add_scalar(g_task_id, b0);
      add_scalar(g_task_id, base);
      add_duration(g_task_id, DUR_DOWN_PROJ_RES);
      tm_submit(g_task_id);
    }
  }
}
