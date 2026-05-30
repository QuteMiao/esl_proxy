// Orchestration Function: qwen3_decode (tensormap auto-dependency, all-SPMD variant).
//
// Same all-SPMD task organization as cases/qwen3_decode.h, but with the manual
// succeed()/batch_succeed()/submit() wiring and producer-tracking arrays removed:
// dependencies are discovered automatically by tensormap (include/tensormap.h). Every
// task registers its OUTPUT addresses and resolves its INPUT addresses to producer
// task ids, wiring edges through esl_proxy's succeed(). Use tm_in/tm_out/tm_inout in
// place of add_input/add_output/add_inout, and tm_submit(tid) to close each task.
//
// SPMD design (mirrors cases/qwen3_decode.h):
//   * Every per-chunk loop is collapsed into a SINGLE SPMD launch via set_block_num(n):
//       q_proj=20, k_proj=8, v_proj=8,
//       qk_matmul/softmax/sv_matmul/online_softmax=4,
//       gate_proj/up_proj/silu=34, down_proj/down_proj_residual=40.
//   * online_softmax is launched ONCE per batch (block_num 4) instead of four times.
//   * Per-chunk scalars (q0/kv0/gi0/mlp_o0/d0) are dropped; gate/up/down write full
//     INOUT tiles allocated up-front (gate_tile/up_tile/mlp_tile/down_tile).
//   * Each task is tagged with its execution unit via set_task_type():
//       AIC -> TASK_TYPE_CUBE, AIV -> TASK_TYPE_VECTOR, MIX -> TASK_TYPE_MIX.
//
// Durations are the V200-benchmark per-subtask means (Readme.md, 2026/5/30 AICore
// View caliber) in ns: each SPMD block runs for the per-kernel mean.
//
// Granularity is whole-buffer (Tensor is a bare uint64_t address), so the resulting
// graph is data-flow-derived and differs from the hand-wired version:
//   * qk_norm depends on q_proj/k_proj (its real inputs) only, not v_proj.
//   * out_proj reads attn_out whole -> depends on every online_softmax that wrote
//     attn_out (one per batch under the single-launch SPMD form: 90 producers,
//     vs. 360 in the old four-launch decomposition).
#include <stddef.h>
#include <stdint.h>

#include "mem_pool.h"
#include "ring_buf.h"
#include "tensormap.h"

// SPMD / execution-unit tagging helpers. Self-contained (poke g_basic_buf directly)
// so they do not require any change to ring_buf.h.
static inline void set_task_type(uint16_t task_id, task_type_t type)
{
    g_basic_buf[task_id & RING_MASK].type = type;
}

static inline void set_block_num(uint16_t task_id, uint32_t count)
{
    g_basic_buf[task_id & RING_MASK].mode = ORG_MODE_SPMD_SYNC;
    g_basic_buf[task_id & RING_MASK].count = count;
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
    // External tensors
    Tensor ext_hidden_states = orch_args + 0;
    Tensor ext_input_rms_weight = orch_args + 1;
    Tensor ext_wq = orch_args + 2;
    Tensor ext_wk = orch_args + 3;
    Tensor ext_wv = orch_args + 4;
    Tensor ext_q_norm_weight = orch_args + 5;
    Tensor ext_k_norm_weight = orch_args + 6;
    Tensor ext_seq_lens = orch_args + 7;
    Tensor ext_block_table = orch_args + 8;
    Tensor ext_slot_mapping = orch_args + 9;
    Tensor ext_rope_cos = orch_args + 10;
    Tensor ext_rope_sin = orch_args + 11;
    Tensor ext_k_cache = orch_args + 12;
    Tensor ext_v_cache = orch_args + 13;
    Tensor ext_wo = orch_args + 14;
    Tensor ext_post_rms_weight = orch_args + 15;
    Tensor ext_w_gate = orch_args + 16;
    Tensor ext_w_up = orch_args + 17;
    Tensor ext_w_down = orch_args + 18;
    Tensor ext_out = orch_args + 19;
    (void)ext_seq_lens;
    (void)ext_slot_mapping;

    tm_deps_init();

    const int64_t user_batch = 90;
    const int64_t batch_padded = (((user_batch + 15) / 16) * 16);

    uint32_t all_q_padded_ci_shapes[2] = {11520, 128};
    Tensor all_q_padded = alloc_tensors(all_q_padded_ci_shapes, 2, BFLOAT16);

    uint32_t q_proj_ci_shapes[2] = {batch_padded, 5120};
    Tensor q_proj = alloc_tensors(q_proj_ci_shapes, 2, FLOAT32);

    uint32_t k_proj_ci_shapes[2] = {batch_padded, 1024};
    Tensor k_proj = alloc_tensors(k_proj_ci_shapes, 2, FLOAT32);

    uint32_t v_proj_ci_shapes[2] = {batch_padded, 1024};
    Tensor v_proj = alloc_tensors(v_proj_ci_shapes, 2, FLOAT32);

    uint32_t q_proj_norm_ci_shapes[2] = {batch_padded, 5120};
    Tensor q_proj_norm = alloc_tensors(q_proj_norm_ci_shapes, 2, FLOAT32);

    uint32_t k_proj_norm_ci_shapes[2] = {batch_padded, 1024};
    Tensor k_proj_norm = alloc_tensors(k_proj_norm_ci_shapes, 2, FLOAT32);

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        uint32_t normed_tile_ci_shapes[2] = {16, 5120};
        Tensor normed_tile = alloc_tensors(normed_tile_ci_shapes, 2, BFLOAT16);
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);

        // Task 0: rmsnorm (AIV, single)
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        tm_in(g_task_id, ext_hidden_states);
        tm_out(g_task_id, normed_tile);
        tm_in(g_task_id, ext_input_rms_weight);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, 23950);
        tm_submit(g_task_id);

        // Spmd q_proj (AIC, block_num 20): HIDDEN / Q_OUT_CHUNK = 5120/256 = 20
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 20);
        tm_in(g_task_id, normed_tile);
        tm_in(g_task_id, ext_wq);
        tm_out(g_task_id, q_proj);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 26060);
        tm_submit(g_task_id);

        // Spmd k_proj (AIC, block_num 8): KV_HIDDEN / KV_OUT_CHUNK = 1024/128 = 8
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 8);
        tm_in(g_task_id, normed_tile);
        tm_in(g_task_id, ext_wk);
        tm_out(g_task_id, k_proj);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 18170);
        tm_submit(g_task_id);

        // Spmd v_proj (AIC, block_num 8): KV_HIDDEN / KV_OUT_CHUNK = 1024/128 = 8
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 8);
        tm_in(g_task_id, normed_tile);
        tm_in(g_task_id, ext_wv);
        tm_out(g_task_id, v_proj);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 17890);
        tm_submit(g_task_id);

        // Task 4: qk_norm (AIV, single) — deps auto-discovered from q_proj/k_proj reads.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        tm_out(g_task_id, k_proj_norm);
        tm_out(g_task_id, q_proj_norm);
        tm_in(g_task_id, q_proj);
        tm_in(g_task_id, ext_q_norm_weight);
        tm_in(g_task_id, k_proj);
        tm_in(g_task_id, ext_k_norm_weight);
        add_scalar(g_task_id, 0);  // q0
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 13190);
        tm_submit(g_task_id);
    }

    uint32_t attn_out_ci_shapes[2] = {batch_padded, 5120};
    Tensor attn_out = alloc_tensors(attn_out_ci_shapes, 2, BFLOAT16);

    // Per-batch attention loop (Func5..Func9). Durations are V200-benchmark per-subtask
    // means in ns. The proxy cannot read tensor data, so seq_lens / slot_mapping reads
    // become fixed placeholders and rope/attn views use base tensors.
    for (int64_t b = 0; b < user_batch; b += 1) {
        uint32_t all_raw_scores_ci_shapes[2] = {4096, 128};
        Tensor all_raw_scores = alloc_tensors(all_raw_scores_ci_shapes, 2, FLOAT32);
        uint32_t all_exp_padded_ci_shapes[2] = {4096, 128};
        Tensor all_exp_padded = alloc_tensors(all_exp_padded_ci_shapes, 2, BFLOAT16);
        uint32_t all_cur_mi_ci_shapes[2] = {4096, 1};
        Tensor all_cur_mi = alloc_tensors(all_cur_mi_ci_shapes, 2, FLOAT32);
        uint32_t all_cur_li_ci_shapes[2] = {4096, 1};
        Tensor all_cur_li = alloc_tensors(all_cur_li_ci_shapes, 2, FLOAT32);
        uint32_t all_oi_tmp_ci_shapes[2] = {4096, 128};
        Tensor all_oi_tmp = alloc_tensors(all_oi_tmp_ci_shapes, 2, FLOAT32);

        // Fixed placeholders for control values (proxy has no tensor data to read).
        const int64_t ctx_len = 1024;
        const int64_t ctx_blocks = ((ctx_len + 127) / 128);
        const int64_t block_table_base = (b * 32);
        const int64_t slot = b;
        const int64_t slot_block = (slot / 128);
        const int64_t slot_offset = (slot - (slot_block * 128));

        // Task 5: rope_kv_cache (AIV, single) — dep qk_norm via k/q_proj_norm, v_proj.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        tm_out(g_task_id, all_q_padded);
        tm_out(g_task_id, ext_k_cache);
        tm_out(g_task_id, ext_v_cache);
        tm_in(g_task_id, k_proj_norm);
        tm_in(g_task_id, ext_rope_cos);  // cos_lo view -> base tensor
        tm_in(g_task_id, ext_rope_sin);  // sin_lo view -> base tensor
        tm_in(g_task_id, ext_rope_cos);  // cos_hi view -> base tensor
        tm_in(g_task_id, ext_rope_sin);  // sin_hi view -> base tensor
        tm_in(g_task_id, v_proj);
        tm_in(g_task_id, q_proj_norm);
        add_scalar(g_task_id, slot_block);
        add_scalar(g_task_id, slot_offset);
        add_scalar(g_task_id, b);
        add_duration(g_task_id, 9480);
        tm_submit(g_task_id);

        // Spmd qk_matmul (AIC, block_num 4) — dep Func5 (all_q_padded, ext_k_cache).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 4);
        tm_in(g_task_id, all_q_padded);
        tm_out(g_task_id, all_raw_scores);
        tm_in(g_task_id, ext_block_table);
        tm_in(g_task_id, ext_k_cache);
        add_scalar(g_task_id, b);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, block_table_base);
        add_duration(g_task_id, 29350);
        tm_submit(g_task_id);

        // Spmd softmax (AIV, block_num 4) — dep Func6 (all_raw_scores).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        set_block_num(g_task_id, 4);
        tm_out(g_task_id, all_cur_li);
        tm_out(g_task_id, all_cur_mi);
        tm_out(g_task_id, all_exp_padded);
        tm_in(g_task_id, all_raw_scores);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, ctx_len);
        add_duration(g_task_id, 19400);
        tm_submit(g_task_id);

        // Spmd sv_matmul (AIC, block_num 4) — dep Func5 (ext_v_cache) and Func7 (all_exp_padded).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 4);
        tm_out(g_task_id, all_oi_tmp);
        tm_in(g_task_id, ext_block_table);
        tm_in(g_task_id, all_exp_padded);
        tm_in(g_task_id, ext_v_cache);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, block_table_base);
        add_duration(g_task_id, 31650);
        tm_submit(g_task_id);

        // Spmd online_softmax (AIV, block_num 4) — single launch per batch,
        // dep Func8 (all_oi_tmp) and Func7 (all_cur_mi/all_cur_li).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        set_block_num(g_task_id, 4);
        tm_in(g_task_id, all_oi_tmp);
        tm_in(g_task_id, all_cur_mi);
        tm_in(g_task_id, all_cur_li);
        tm_inout(g_task_id, attn_out);  // attn_row view -> base tensor
        add_scalar(g_task_id, ctx_blocks);
        add_duration(g_task_id, 20820);
        tm_submit(g_task_id);
    }

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        uint32_t resid1_tile_ci_shapes[2] = {16, 5120};
        Tensor resid1_tile = alloc_tensors(resid1_tile_ci_shapes, 2, FLOAT32);
        uint32_t gm_pipe_buffer_0_ci_shapes[2] = {16384, 40};
        Tensor gm_pipe_buffer_0 = alloc_tensors(gm_pipe_buffer_0_ci_shapes, 2, FLOAT32);
        uint32_t post_norm_tile_ci_shapes[2] = {16, 5120};
        Tensor post_norm_tile = alloc_tensors(post_norm_tile_ci_shapes, 2, BFLOAT16);
        uint32_t mlp_tile_ci_shapes[2] = {16, 17408};
        Tensor mlp_tile = alloc_tensors(mlp_tile_ci_shapes, 2, BFLOAT16);
        uint32_t gate_tile_ci_shapes[2] = {16, 17408};
        Tensor gate_tile = alloc_tensors(gate_tile_ci_shapes, 2, FLOAT32);
        uint32_t up_tile_ci_shapes[2] = {16, 17408};
        Tensor up_tile = alloc_tensors(up_tile_ci_shapes, 2, FLOAT32);
        uint32_t down_tile_ci_shapes[2] = {16, 5120};
        Tensor down_tile = alloc_tensors(down_tile_ci_shapes, 2, FLOAT32);
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);

        // Task 10/11: out_proj_residual (MixedKernels AIC+AIV) — MIX, block_num 40 —
        // reads attn_out whole, so depends on every online_softmax (whole-buffer over-sync).
        // Duration is the per-mix-instance mean = max(aic, aiv_1, aiv_2).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_MIX);
        set_block_num(g_task_id, 40);
        tm_in(g_task_id, ext_hidden_states);
        tm_in(g_task_id, attn_out);
        tm_in(g_task_id, ext_wo);
        tm_inout(g_task_id, resid1_tile);
        tm_out(g_task_id, gm_pipe_buffer_0);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, 40750);
        tm_submit(g_task_id);

        // Task 12: post_rmsnorm (AIV, single) — dep Func10/11 (resid1_tile).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        tm_in(g_task_id, resid1_tile);
        tm_out(g_task_id, post_norm_tile);
        tm_in(g_task_id, ext_post_rms_weight);
        add_duration(g_task_id, 24390);
        tm_submit(g_task_id);

        // Spmd gate_proj (AIC, block_num 34): INTERMEDIATE / MLP_OUT_CHUNK = 17408/512 = 34
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 34);
        tm_in(g_task_id, post_norm_tile);
        tm_in(g_task_id, ext_w_gate);
        tm_inout(g_task_id, gate_tile);
        add_duration(g_task_id, 95700);
        tm_submit(g_task_id);

        // Spmd up_proj (AIC, block_num 34).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 34);
        tm_in(g_task_id, post_norm_tile);
        tm_in(g_task_id, ext_w_up);
        tm_inout(g_task_id, up_tile);
        add_duration(g_task_id, 97140);
        tm_submit(g_task_id);

        // Spmd silu (AIV, block_num 34) — dep gate_proj + up_proj (gate_tile, up_tile).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        set_block_num(g_task_id, 34);
        tm_in(g_task_id, gate_tile);
        tm_in(g_task_id, up_tile);
        tm_inout(g_task_id, mlp_tile);  // ret0__out_2 view -> base tensor
        add_duration(g_task_id, 2820);
        tm_submit(g_task_id);

        // Spmd down_proj (AIC, block_num 40): HIDDEN / DOWN_OUT_CHUNK = 5120/128 = 40 —
        // reads full mlp_tile, dep silu.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 40);
        tm_in(g_task_id, mlp_tile);
        tm_in(g_task_id, ext_w_down);
        tm_inout(g_task_id, down_tile);
        add_duration(g_task_id, 72220);
        tm_submit(g_task_id);

        // Spmd down_proj_residual (AIV, block_num 40) — dep down_proj (down_tile) and
        // Func10/11 (resid1_tile).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        set_block_num(g_task_id, 40);
        tm_in(g_task_id, down_tile);
        tm_in(g_task_id, resid1_tile);
        tm_out(g_task_id, ext_out);
        add_scalar(g_task_id, cur_valid);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 2590);
        tm_submit(g_task_id);
    }
}
