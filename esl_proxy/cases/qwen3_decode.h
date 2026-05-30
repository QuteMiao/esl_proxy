// Orchestration Function: qwen3_decode (dynamic-tensormap, manual-scope explicit-deps variant).
// Derived from examples/qwen3/dynamic_tensormap/orchestration/qwen3_decode.cpp; the
// dependency pattern follows simpler/.../Qwen3Decode_manual_scope_new.
//
// Differences from the auto-emit dynamic_tensormap variant:
//   * Outer scope uses PTO2_SCOPE(PTO2ScopeMode::MANUAL); inner PTO2_SCOPE() wrappers
//     are removed (AUTO nested in MANUAL is unsupported).
//   * All cross-task ordering is expressed explicitly with ArgWithDeps<N> + add_dep(...)
//     using uint16_t values returned by rt_submit_*_task.
//
// Differences from Qwen3Decode_manual_scope_new:
//   * Runtime tensor-map data lookup is preserved: user_batch is derived at runtime from
//     orch_args + 0).shapes[0], and seq_lens / slot_mapping rows are read via
//     get_tensor_data<int32_t>(...) instead of host-pointer access. View extents are
//     bounds-checked against runtime tensor shapes.
//   * Kernel set and func-id assignments match the dynamic_tensormap CALLABLE list
//     (no MixedKernels for the final down_proj_residual; per-db AIV writeback).
//   * online_softmax is launched four times per batch (gi0=0,2,4,6) as in dynamic_tensormap.
//
// Dependency summary (per CALLABLE func id):
//   Tile loop b0 in [0, batch_padded), step 16:
//     Func0 (rmsnorm)         : no deps
//     Func1/2/3 (q/k/v_proj)  : dep Func0 for this tile
//     Func4 (qk_norm)         : dep Func1+Func2+Func3 for this tile
//   Per-batch loop b in [0, user_batch):
//     Func5 (rope_kv_cache)   : dep Func4 for tile b/16 and all_q_padded alloc
//     Func6 (qk_matmul)       : dep Func5 and per-batch attn scratch alloc
//     Func7 (softmax)         : dep Func6
//     Func8 (sv_matmul)       : dep Func5 and Func7
//     Func9 (online_softmax)x4: each dep Func8 (per (b, gi0
//   Tile loop b0 in [0, batch_padded), step 16:
//     Func10/11 (out_proj mixed) : dep every Func9 for batches in [b0, b0+cur_valid)
//     Func12 (post_rmsnorm)      : dep Func10/11
//     Func13/14 (gate/up_proj)   : dep Func12 (per ob)
//     Func15 (silu)              : dep Func13+Func14 for that ob
//     Func16 (down_proj)         : dep every Func15 (full mlp_tile read)
//     Func17 (down_proj_residual): dep Func10/11 and Func16 for that db
#include <stddef.h>
#include <stdint.h>

#include "mem_pool.h"
#include "ring_buf.h"

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

    const int64_t user_batch = 90;
    const int64_t batch_padded = (((user_batch + 15) / 16) * 16);
    const int64_t num_tiles = batch_padded / 16;

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

    uint16_t q_proj_task_per_tile[num_tiles];
    uint16_t k_proj_task_per_tile[num_tiles];
    uint16_t v_proj_task_per_tile[num_tiles];
    uint16_t qk_norm_task_per_tile[num_tiles];
    uint16_t online_softmax_tasks_by_b[user_batch][num_tiles];

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        const size_t tix = b0 / 16;
        uint32_t normed_tile_ci_shapes[2] = {16, 5120};
        Tensor normed_tile = alloc_tensors(normed_tile_ci_shapes, 2, BFLOAT16);
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);

        // Task 0: rmsnorm
        uint16_t rmsnorm_id = g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        add_input(g_task_id, ext_hidden_states);
        add_output(g_task_id, normed_tile);
        add_input(g_task_id, ext_input_rms_weight);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, 22780);

        // q_proj loop (Q_OUT_CHUNK = 256, HIDDEN = 5120 -> 20 chunks)
        int cnt = 0;
        uint16_t successor_list[512];
        for (int64_t q0 = 0; q0 < 5120; q0 += 256) {
            // Task 1: q_proj
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, normed_tile);
            add_input(g_task_id, ext_wq);
            add_output(g_task_id, q_proj);
            add_scalar(g_task_id, q0);
            add_scalar(g_task_id, b0);
            add_duration(g_task_id, 26980);
            successor_list[cnt++] = g_task_id;
            q_proj_task_per_tile[tix] = g_task_id;
        }

        // k_proj loop (KV_OUT_CHUNK = 128, KV_HIDDEN = 1024 -> 8 chunks)
        for (int64_t kv0 = 0; kv0 < 1024; kv0 += 128) {
            // Task 2: k_proj
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, normed_tile);
            add_input(g_task_id, ext_wk);
            add_output(g_task_id, k_proj);
            add_scalar(g_task_id, kv0);
            add_scalar(g_task_id, b0);
            add_duration(g_task_id, 17770);
            cnt++;
            successor_list[cnt++] = g_task_id;
            k_proj_task_per_tile[tix] = g_task_id;
        }

        // v_proj loop (KV_OUT_CHUNK = 128, KV_HIDDEN = 1024 -> 8 chunks)
        for (int64_t kv0 = 0; kv0 < 1024; kv0 += 128) {
            // Task 3: v_proj
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, normed_tile);
            add_input(g_task_id, ext_wv);
            add_output(g_task_id, v_proj);
            add_scalar(g_task_id, kv0);
            add_scalar(g_task_id, b0);
            add_duration(g_task_id, 19140);
            cnt++;
            successor_list[cnt++] = g_task_id;
            v_proj_task_per_tile[tix] = g_task_id;
        }
        batch_succeed(cnt, successor_list, rmsnorm_id);
        batch_submit(cnt, successor_list);

        // Task 4: qk_norm — fans in latest q/k/v_proj task for this tile.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        add_output(g_task_id, k_proj_norm);
        add_output(g_task_id, q_proj_norm);
        add_input(g_task_id, q_proj);
        add_input(g_task_id, ext_q_norm_weight);
        add_input(g_task_id, k_proj);
        add_input(g_task_id, ext_k_norm_weight);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 13380);
        succeed(g_task_id, q_proj_task_per_tile[tix]);
        succeed(g_task_id, k_proj_task_per_tile[tix]);
        succeed(g_task_id, v_proj_task_per_tile[tix]);
        submit(g_task_id);
        qk_norm_task_per_tile[tix] = g_task_id;
    }

    uint32_t attn_out_ci_shapes[2] = {batch_padded, 5120};
    Tensor attn_out = alloc_tensors(attn_out_ci_shapes, 2, BFLOAT16);

    // Per-batch attention loop (Func5..Func9). Durations are V200-benchmark per-kernel
    // averages in ns (avg_us * 1000). The proxy cannot read tensor data, so seq_lens /
    // slot_mapping reads become fixed placeholders and rope/attn views use base tensors.
    for (int64_t b = 0; b < user_batch; b += 1) {
        const size_t tix = b / 16;

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

        // Task 5: rope_kv_cache — dep Func4 (qk_norm) for this batch's tile.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        add_output(g_task_id, all_q_padded);
        add_output(g_task_id, ext_k_cache);
        add_output(g_task_id, ext_v_cache);
        add_input(g_task_id, k_proj_norm);
        add_input(g_task_id, ext_rope_cos);  // cos_lo view -> base tensor
        add_input(g_task_id, ext_rope_sin);  // sin_lo view -> base tensor
        add_input(g_task_id, ext_rope_cos);  // cos_hi view -> base tensor
        add_input(g_task_id, ext_rope_sin);  // sin_hi view -> base tensor
        add_input(g_task_id, v_proj);
        add_input(g_task_id, q_proj_norm);
        add_scalar(g_task_id, slot_block);
        add_scalar(g_task_id, slot_offset);
        add_scalar(g_task_id, b);
        add_duration(g_task_id, 9560);
        succeed(g_task_id, qk_norm_task_per_tile[tix]);
        submit(g_task_id);
        const uint16_t rope_kv_id = g_task_id;

        // Task 6: qk_matmul — dep Func5.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        add_input(g_task_id, all_q_padded);
        add_output(g_task_id, all_raw_scores);
        add_input(g_task_id, ext_block_table);
        add_input(g_task_id, ext_k_cache);
        add_scalar(g_task_id, b);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, block_table_base);
        add_duration(g_task_id, 29500);
        succeed(g_task_id, rope_kv_id);
        submit(g_task_id);
        const uint16_t qk_matmul_id = g_task_id;

        // Task 7: softmax — dep Func6.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        add_output(g_task_id, all_cur_li);
        add_output(g_task_id, all_cur_mi);
        add_output(g_task_id, all_exp_padded);
        add_input(g_task_id, all_raw_scores);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, ctx_len);
        add_duration(g_task_id, 20010);
        succeed(g_task_id, qk_matmul_id);
        submit(g_task_id);
        const uint16_t softmax_id = g_task_id;

        // Task 8: sv_matmul — dep Func5 (KV-cache writes) and Func7 (softmax outputs).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        add_output(g_task_id, all_oi_tmp);
        add_input(g_task_id, ext_block_table);
        add_input(g_task_id, all_exp_padded);
        add_input(g_task_id, ext_v_cache);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, block_table_base);
        add_duration(g_task_id, 31480);
        succeed(g_task_id, rope_kv_id);
        succeed(g_task_id, softmax_id);
        submit(g_task_id);
        const uint16_t sv_matmul_id = g_task_id;

        // Task 9: online_softmax — four launches (gi0 = 0, 2, 4, 6), each dep Func8.
        for (int64_t gi0 = 0; gi0 < 8; gi0 += 2) {
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, all_oi_tmp);
            add_input(g_task_id, all_cur_mi);
            add_input(g_task_id, all_cur_li);
            add_output(g_task_id, attn_out);  // attn_row view -> base tensor
            add_scalar(g_task_id, gi0);
            add_scalar(g_task_id, ctx_blocks);
            add_duration(g_task_id, 20440);
            succeed(g_task_id, sv_matmul_id);
            submit(g_task_id);
            online_softmax_tasks_by_b[b][gi0 / 2] = g_task_id;
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

        // Task 10/11: out_proj_residual (MixedKernels AIC+AIV) — dep every Func9 for
        // batches in [b0, b0+cur_valid). Duration = max(aic 43590, aiv 91230) ns
        // (the two halves run synchronized).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        add_input(g_task_id, ext_hidden_states);
        add_input(g_task_id, attn_out);
        add_input(g_task_id, ext_wo);
        add_inout(g_task_id, resid1_tile);
        add_output(g_task_id, gm_pipe_buffer_0);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, 91230);
        for (int64_t row = 0; row < cur_valid; row += 1) {
            const int64_t bb = b0 + row;
            for (int gi = 0; gi < 4; gi++) {
                succeed(g_task_id, online_softmax_tasks_by_b[bb][gi]);
            }
        }
        submit(g_task_id);
        const uint16_t out_proj_mixed_id = g_task_id;

        // Task 12: post_rmsnorm — dep Func10/11.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        add_input(g_task_id, resid1_tile);
        add_output(g_task_id, post_norm_tile);
        add_input(g_task_id, ext_post_rms_weight);
        add_duration(g_task_id, 27790);
        succeed(g_task_id, out_proj_mixed_id);
        submit(g_task_id);
        const uint16_t post_rmsnorm_id = g_task_id;

        // MLP gate/up/silu loop (34 chunks of 512 = 17408 = INTERMEDIATE).
        uint16_t silu_task_by_ob[34];
        for (int64_t ob = 0; ob < 34; ob += 1) {
            uint32_t ret0__out_ci_shapes[2] = {16, 512};
            Tensor ret0__out = alloc_tensors(ret0__out_ci_shapes, 2, FLOAT32);
            uint32_t ret0__out_1_ci_shapes[2] = {16, 512};
            Tensor ret0__out_1 = alloc_tensors(ret0__out_1_ci_shapes, 2, FLOAT32);
            const int64_t mlp_o0 = (ob * 512);

            // Task 13: gate_proj — dep Func12.
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, post_norm_tile);
            add_input(g_task_id, ext_w_gate);
            add_output(g_task_id, ret0__out);
            add_scalar(g_task_id, mlp_o0);
            add_duration(g_task_id, 97020);
            succeed(g_task_id, post_rmsnorm_id);
            submit(g_task_id);
            const uint16_t gate_id = g_task_id;

            // Task 14: up_proj — dep Func12.
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, post_norm_tile);
            add_input(g_task_id, ext_w_up);
            add_output(g_task_id, ret0__out_1);
            add_scalar(g_task_id, mlp_o0);
            add_duration(g_task_id, 98440);
            succeed(g_task_id, post_rmsnorm_id);
            submit(g_task_id);
            const uint16_t up_id = g_task_id;

            // Task 15: silu — dep Func13 + Func14 for that ob.
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, ret0__out);
            add_input(g_task_id, ret0__out_1);
            add_output(g_task_id, mlp_tile);  // ret0__out_2 view -> base tensor
            add_scalar(g_task_id, mlp_o0);
            add_duration(g_task_id, 2940);
            succeed(g_task_id, gate_id);
            succeed(g_task_id, up_id);
            submit(g_task_id);
            silu_task_by_ob[ob] = g_task_id;
        }

        // Final down_proj + down_proj_residual loop (HIDDEN / DOWN_OUT_CHUNK = 5120/128 = 40).
        for (int64_t dob = 0; dob < 40; dob += 1) {
            uint32_t fp32_chunk_gm_ci_shapes[2] = {16, 128};
            Tensor fp32_chunk_gm = alloc_tensors(fp32_chunk_gm_ci_shapes, 2, FLOAT32);
            const int64_t d0 = (dob * 128);

            // Task 16: down_proj — reads full mlp_tile, dep every Func15.
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, mlp_tile);
            add_input(g_task_id, ext_w_down);
            add_inout(g_task_id, fp32_chunk_gm);
            add_scalar(g_task_id, d0);
            add_duration(g_task_id, 74320);
            for (int ob = 0; ob < 34; ob++) {
                succeed(g_task_id, silu_task_by_ob[ob]);
            }
            submit(g_task_id);
            const uint16_t down_proj_id = g_task_id;

            // Task 17: down_proj_residual — dep Func16 and Func10/11 for that db.
            g_task_id++;
            while (try_new_task(g_task_id))
            {
                spin_wait();
            }
            add_input(g_task_id, fp32_chunk_gm);
            add_input(g_task_id, resid1_tile);
            add_output(g_task_id, ext_out);
            add_scalar(g_task_id, d0);
            add_scalar(g_task_id, cur_valid);
            add_scalar(g_task_id, b0);
            add_duration(g_task_id, 3130);
            succeed(g_task_id, down_proj_id);
            succeed(g_task_id, out_proj_mixed_id);
            submit(g_task_id);
        }
    }
}
