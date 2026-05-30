// Orchestration Function: qwen3_decode (tensormap auto-dependency variant).
//
// Copy of cases/qwen3_decode.h with all manual succeed()/batch_succeed() and the
// producer-tracking arrays removed. Dependencies are discovered automatically by
// tensormap (cases/tensormap_deps.h): every task registers its OUTPUT addresses
// and resolves its INPUT addresses to producer task ids, wiring edges through
// esl_proxy's succeed(). Use tm_in/tm_out/tm_inout in place of
// add_input/add_output/add_inout, and tm_submit(tid) to close each task.
//
// Granularity is whole-buffer (Tensor is a bare uint64_t address), so the
// resulting graph is data-flow-derived and differs from the hand-wired version:
//   * qk_norm depends on q_proj/k_proj (its real inputs) only, not v_proj.
//   * qk_norm depends on ALL q_proj/k_proj chunk producers, not just the last.
//   * out_proj reads attn_out whole -> depends on every online_softmax that
//     wrote attn_out (over-synchronized vs. the per-tile hand-wired subset).
#include <stddef.h>
#include <stdint.h>

#include "mem_pool.h"
#include "ring_buf.h"
#include "tensormap_deps.h"

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

        // Task 0: rmsnorm (root: no producers)
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            wait();
        }
        tm_in(g_task_id, ext_hidden_states);
        tm_out(g_task_id, normed_tile);
        tm_in(g_task_id, ext_input_rms_weight);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, 22780);
        tm_submit(g_task_id);

        // q_proj loop (Q_OUT_CHUNK = 256, HIDDEN = 5120 -> 20 chunks)
        for (int64_t q0 = 0; q0 < 5120; q0 += 256) {
            // Task 1: q_proj
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, normed_tile);
            tm_in(g_task_id, ext_wq);
            tm_out(g_task_id, q_proj);
            add_scalar(g_task_id, q0);
            add_scalar(g_task_id, b0);
            add_duration(g_task_id, 26980);
            tm_submit(g_task_id);
        }

        // k_proj loop (KV_OUT_CHUNK = 128, KV_HIDDEN = 1024 -> 8 chunks)
        for (int64_t kv0 = 0; kv0 < 1024; kv0 += 128) {
            // Task 2: k_proj
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, normed_tile);
            tm_in(g_task_id, ext_wk);
            tm_out(g_task_id, k_proj);
            add_scalar(g_task_id, kv0);
            add_scalar(g_task_id, b0);
            add_duration(g_task_id, 17770);
            tm_submit(g_task_id);
        }

        // v_proj loop (KV_OUT_CHUNK = 128, KV_HIDDEN = 1024 -> 8 chunks)
        for (int64_t kv0 = 0; kv0 < 1024; kv0 += 128) {
            // Task 3: v_proj
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, normed_tile);
            tm_in(g_task_id, ext_wv);
            tm_out(g_task_id, v_proj);
            add_scalar(g_task_id, kv0);
            add_scalar(g_task_id, b0);
            add_duration(g_task_id, 19140);
            tm_submit(g_task_id);
        }

        // Task 4: qk_norm — fans in q/k_proj chunks of this tile via q_proj/k_proj reads.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            wait();
        }
        tm_out(g_task_id, k_proj_norm);
        tm_out(g_task_id, q_proj_norm);
        tm_in(g_task_id, q_proj);
        tm_in(g_task_id, ext_q_norm_weight);
        tm_in(g_task_id, k_proj);
        tm_in(g_task_id, ext_k_norm_weight);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 13380);
        tm_submit(g_task_id);
    }

    uint32_t attn_out_ci_shapes[2] = {batch_padded, 5120};
    Tensor attn_out = alloc_tensors(attn_out_ci_shapes, 2, BFLOAT16);

    // Per-batch attention loop (Func5..Func9).
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

        // Task 5: rope_kv_cache — dep qk_norm of this batch's tile (via k/q_proj_norm, v_proj).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            wait();
        }
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
        add_duration(g_task_id, 9560);
        tm_submit(g_task_id);

        // Task 6: qk_matmul — dep Func5 (all_q_padded, ext_k_cache).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            wait();
        }
        tm_in(g_task_id, all_q_padded);
        tm_out(g_task_id, all_raw_scores);
        tm_in(g_task_id, ext_block_table);
        tm_in(g_task_id, ext_k_cache);
        add_scalar(g_task_id, b);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, block_table_base);
        add_duration(g_task_id, 29500);
        tm_submit(g_task_id);

        // Task 7: softmax — dep Func6 (all_raw_scores).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            wait();
        }
        tm_out(g_task_id, all_cur_li);
        tm_out(g_task_id, all_cur_mi);
        tm_out(g_task_id, all_exp_padded);
        tm_in(g_task_id, all_raw_scores);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, ctx_len);
        add_duration(g_task_id, 20010);
        tm_submit(g_task_id);

        // Task 8: sv_matmul — dep Func5 (ext_v_cache) and Func7 (all_exp_padded).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            wait();
        }
        tm_out(g_task_id, all_oi_tmp);
        tm_in(g_task_id, ext_block_table);
        tm_in(g_task_id, all_exp_padded);
        tm_in(g_task_id, ext_v_cache);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, block_table_base);
        add_duration(g_task_id, 31480);
        tm_submit(g_task_id);

        // Task 9: online_softmax — four launches (gi0 = 0, 2, 4, 6), each dep Func8/Func7.
        for (int64_t gi0 = 0; gi0 < 8; gi0 += 2) {
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, all_oi_tmp);
            tm_in(g_task_id, all_cur_mi);
            tm_in(g_task_id, all_cur_li);
            tm_out(g_task_id, attn_out);  // attn_row view -> base tensor
            add_scalar(g_task_id, gi0);
            add_scalar(g_task_id, ctx_blocks);
            add_duration(g_task_id, 20440);
            tm_submit(g_task_id);
        }
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
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);

        // Task 10/11: out_proj_residual (MixedKernels AIC+AIV) — reads attn_out whole, so
        // depends on every online_softmax that wrote attn_out (whole-buffer over-sync).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            wait();
        }
        tm_in(g_task_id, ext_hidden_states);
        tm_in(g_task_id, attn_out);
        tm_in(g_task_id, ext_wo);
        tm_inout(g_task_id, resid1_tile);
        tm_out(g_task_id, gm_pipe_buffer_0);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, 91230);
        tm_submit(g_task_id);

        // Task 12: post_rmsnorm — dep Func10/11 (resid1_tile).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            wait();
        }
        tm_in(g_task_id, resid1_tile);
        tm_out(g_task_id, post_norm_tile);
        tm_in(g_task_id, ext_post_rms_weight);
        add_duration(g_task_id, 27790);
        tm_submit(g_task_id);

        // MLP gate/up/silu loop (34 chunks of 512 = 17408 = INTERMEDIATE).
        for (int64_t ob = 0; ob < 34; ob += 1) {
            uint32_t ret0__out_ci_shapes[2] = {16, 512};
            Tensor ret0__out = alloc_tensors(ret0__out_ci_shapes, 2, FLOAT32);
            uint32_t ret0__out_1_ci_shapes[2] = {16, 512};
            Tensor ret0__out_1 = alloc_tensors(ret0__out_1_ci_shapes, 2, FLOAT32);
            const int64_t mlp_o0 = (ob * 512);

            // Task 13: gate_proj — dep Func12 (post_norm_tile).
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, post_norm_tile);
            tm_in(g_task_id, ext_w_gate);
            tm_out(g_task_id, ret0__out);
            add_scalar(g_task_id, mlp_o0);
            add_duration(g_task_id, 97020);
            tm_submit(g_task_id);

            // Task 14: up_proj — dep Func12 (post_norm_tile).
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, post_norm_tile);
            tm_in(g_task_id, ext_w_up);
            tm_out(g_task_id, ret0__out_1);
            add_scalar(g_task_id, mlp_o0);
            add_duration(g_task_id, 98440);
            tm_submit(g_task_id);

            // Task 15: silu — dep Func13 + Func14 (ret0__out, ret0__out_1) for that ob.
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, ret0__out);
            tm_in(g_task_id, ret0__out_1);
            tm_out(g_task_id, mlp_tile);  // ret0__out_2 view -> base tensor
            add_scalar(g_task_id, mlp_o0);
            add_duration(g_task_id, 2940);
            tm_submit(g_task_id);
        }

        // Final down_proj + down_proj_residual loop (HIDDEN / DOWN_OUT_CHUNK = 5120/128 = 40).
        for (int64_t dob = 0; dob < 40; dob += 1) {
            uint32_t fp32_chunk_gm_ci_shapes[2] = {16, 128};
            Tensor fp32_chunk_gm = alloc_tensors(fp32_chunk_gm_ci_shapes, 2, FLOAT32);
            const int64_t d0 = (dob * 128);

            // Task 16: down_proj — reads full mlp_tile, dep every Func15 of this tile.
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, mlp_tile);
            tm_in(g_task_id, ext_w_down);
            tm_inout(g_task_id, fp32_chunk_gm);
            add_scalar(g_task_id, d0);
            add_duration(g_task_id, 74320);
            tm_submit(g_task_id);

            // Task 17: down_proj_residual — dep Func16 (fp32_chunk_gm) and Func10/11 (resid1_tile).
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                wait();
            }
            tm_in(g_task_id, fp32_chunk_gm);
            tm_in(g_task_id, resid1_tile);
            tm_out(g_task_id, ext_out);
            add_scalar(g_task_id, d0);
            add_scalar(g_task_id, cur_valid);
            add_scalar(g_task_id, b0);
            add_duration(g_task_id, 3130);
            tm_submit(g_task_id);
        }
    }
}
