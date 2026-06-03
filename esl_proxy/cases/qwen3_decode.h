// Orchestration Function: qwen3_decode (dynamic-tensormap, manual-scope all-SPMD variant).
//
// Mirrors the all-SPMD shape of
// simpler_testcase/qwen3_dynamic_manual_scope_allspmd/orchestration/qwen3_decode.cpp.
// The manual succeed()/batch_succeed()/submit() dependency wiring and the proxy's
// fixed control-value placeholders are preserved; the task organization is all-SPMD.
//
// Key points (vs. a per-chunk single-task decomposition):
//   * Every per-chunk loop is collapsed into a SINGLE SPMD launch via set_block_num(n):
//       q_proj  -> block_num 20  (HIDDEN / Q_OUT_CHUNK   = 5120/256)
//       k_proj  -> block_num 8   (KV_HIDDEN / KV_OUT_CHUNK= 1024/128)
//       v_proj  -> block_num 8
//       qk_matmul/softmax/sv_matmul/online_softmax -> block_num 4
//       gate_proj/up_proj/silu -> block_num 34  (INTERMEDIATE / MLP_OUT_CHUNK = 17408/512)
//       down_proj/down_proj_residual -> block_num 40 (HIDDEN / DOWN_OUT_CHUNK = 5120/128)
//   * online_softmax is launched ONCE per batch (block_num 4) instead of four times,
//     so producer tracking uses online_softmax_task_by_b[user_batch] (1-D).
//   * Per-chunk scalars (q0/kv0/gi0/mlp_o0/d0) are dropped; the SPMD kernel derives its
//     own offset from the block index. gate/up/down write full INOUT tiles allocated
//     up-front (gate_tile/up_tile/mlp_tile/down_tile).
//   * Each task is tagged with its execution unit via set_task_type():
//       AIC -> TASK_TYPE_CUBE, AIV -> TASK_TYPE_VECTOR, MIX -> TASK_TYPE_MIX.
//     out_proj_residual is the MixedKernels (AIC+AIV) task -> TASK_TYPE_MIX.
//
// Dependency summary (per CALLABLE func id):
//   Tile loop b0 in [0, batch_padded), step 16:
//     Func0  (rmsnorm)          AIV single  : no deps
//     Func1/2/3 (q/k/v_proj)    AIC spmd    : dep Func0 for this tile
//     Func4  (qk_norm)          AIV single  : dep Func1+Func2+Func3 for this tile
//   Per-batch loop b in [0, user_batch):
//     Func5  (rope_kv_cache)    AIV single  : dep Func4 for tile b/16
//     Func6  (qk_matmul)        AIC spmd 4  : dep Func5
//     Func7  (softmax)          AIV spmd 4  : dep Func6
//     Func8  (sv_matmul)        AIC spmd 4  : dep Func5 and Func7
//     Func9  (online_softmax)   AIV spmd 4  : dep Func8 (single launch per batch)
//   Tile loop b0 in [0, batch_padded), step 16:
//     Func10/11 (out_proj)      MIX spmd 40 : dep every Func9 for batches in [b0, b0+cur_valid)
//     Func12 (post_rmsnorm)     AIV single  : dep Func10/11
//     Func13/14 (gate/up_proj)  AIC spmd 34 : dep Func12
//     Func15 (silu)             AIV spmd 34 : dep Func13+Func14
//     Func16 (down_proj)        AIC spmd 40 : dep Func15 (full mlp_tile read)
//     Func17 (down_proj_residual) AIV spmd 40 : dep Func10/11 and Func16
#include <stddef.h>
#include <stdint.h>

#include "mem_pool.h"
#include "ring_buf.h"
#include "dispatch.h"

extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;
extern ctrl_t g_ctrl_t[THREAD_CNT];

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
    uint16_t online_softmax_task_by_b[user_batch];

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        const size_t tix = b0 / 16;
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
        add_input(g_task_id, ext_hidden_states);
        add_output(g_task_id, normed_tile);
        add_input(g_task_id, ext_input_rms_weight);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, 23950);
        const uint16_t rmsnorm_id = g_task_id;
        queue_t* queue = &g_ctrl_t[g_task_id & (uint16_t)0x1].ready_queue[TASK_TYPE_VECTOR];
        lock_q(queue);
        enqueue(queue, rmsnorm_id);
        unlock_q(queue);

        // Spmd q_proj (AIC, block_num 20): HIDDEN / Q_OUT_CHUNK = 5120/256 = 20
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 20);
        add_input(g_task_id, normed_tile);
        add_input(g_task_id, ext_wq);
        add_output(g_task_id, q_proj);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 26060);
        q_proj_task_per_tile[tix] = g_task_id;

        // Spmd k_proj (AIC, block_num 8): KV_HIDDEN / KV_OUT_CHUNK = 1024/128 = 8
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 8);
        add_input(g_task_id, normed_tile);
        add_input(g_task_id, ext_wk);
        add_output(g_task_id, k_proj);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 18170);
        k_proj_task_per_tile[tix] = g_task_id;

        // Spmd v_proj (AIC, block_num 8): KV_HIDDEN / KV_OUT_CHUNK = 1024/128 = 8
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 8);
        add_input(g_task_id, normed_tile);
        add_input(g_task_id, ext_wv);
        add_output(g_task_id, v_proj);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 17890);
        v_proj_task_per_tile[tix] = g_task_id;

        // q/k/v_proj all depend on Func0 (rmsnorm) for this tile.
        uint16_t successor_list[3];
        successor_list[0] = q_proj_task_per_tile[tix];
        successor_list[1] = k_proj_task_per_tile[tix];
        successor_list[2] = v_proj_task_per_tile[tix];
        batch_succeed(3, successor_list, rmsnorm_id);
        batch_submit(3, successor_list);

        // Task 4: qk_norm (AIV, single) — fans in q/k/v_proj for this tile.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        add_output(g_task_id, k_proj_norm);
        add_output(g_task_id, q_proj_norm);
        add_input(g_task_id, q_proj);
        add_input(g_task_id, ext_q_norm_weight);
        add_input(g_task_id, k_proj);
        add_input(g_task_id, ext_k_norm_weight);
        add_scalar(g_task_id, 0);  // q0
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 13190);
        succeed(g_task_id, q_proj_task_per_tile[tix]);
        succeed(g_task_id, k_proj_task_per_tile[tix]);
        succeed(g_task_id, v_proj_task_per_tile[tix]);
        submit(g_task_id);
        qk_norm_task_per_tile[tix] = g_task_id;
    }

    uint32_t attn_out_ci_shapes[2] = {batch_padded, 5120};
    Tensor attn_out = alloc_tensors(attn_out_ci_shapes, 2, BFLOAT16);

    // Per-batch attention loop (Func5..Func9). Durations are V200-benchmark per-kernel
    // averages in ns. The proxy cannot read tensor data, so seq_lens / slot_mapping reads
    // become fixed placeholders and rope/attn views use base tensors.
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

        // Task 5: rope_kv_cache (AIV, single) — dep Func4 (qk_norm) for this batch's tile.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
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
        add_duration(g_task_id, 9480);
        succeed(g_task_id, qk_norm_task_per_tile[tix]);
        submit(g_task_id);
        const uint16_t rope_kv_id = g_task_id;

        // Spmd qk_matmul (AIC, block_num 4) — dep Func5.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 4);
        add_input(g_task_id, all_q_padded);
        add_output(g_task_id, all_raw_scores);
        add_input(g_task_id, ext_block_table);
        add_input(g_task_id, ext_k_cache);
        add_scalar(g_task_id, b);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, block_table_base);
        add_duration(g_task_id, 29350);
        succeed(g_task_id, rope_kv_id);
        submit(g_task_id);
        const uint16_t qk_matmul_id = g_task_id;

        // Spmd softmax (AIV, block_num 4) — dep Func6.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        set_block_num(g_task_id, 4);
        add_output(g_task_id, all_cur_li);
        add_output(g_task_id, all_cur_mi);
        add_output(g_task_id, all_exp_padded);
        add_input(g_task_id, all_raw_scores);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, ctx_len);
        add_duration(g_task_id, 19400);
        succeed(g_task_id, qk_matmul_id);
        submit(g_task_id);
        const uint16_t softmax_id = g_task_id;

        // Spmd sv_matmul (AIC, block_num 4) — dep Func5 (KV-cache writes) and Func7 (softmax).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 4);
        add_output(g_task_id, all_oi_tmp);
        add_input(g_task_id, ext_block_table);
        add_input(g_task_id, all_exp_padded);
        add_input(g_task_id, ext_v_cache);
        add_scalar(g_task_id, ctx_blocks);
        add_scalar(g_task_id, block_table_base);
        add_duration(g_task_id, 31650);
        succeed(g_task_id, rope_kv_id);
        succeed(g_task_id, softmax_id);
        submit(g_task_id);
        const uint16_t sv_matmul_id = g_task_id;

        // Spmd online_softmax (AIV, block_num 4) — single launch per batch, dep Func8.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        set_block_num(g_task_id, 4);
        add_input(g_task_id, all_oi_tmp);
        add_input(g_task_id, all_cur_mi);
        add_input(g_task_id, all_cur_li);
        add_inout(g_task_id, attn_out);  // attn_row view -> base tensor
        add_scalar(g_task_id, ctx_blocks);
        add_duration(g_task_id, 20820);
        succeed(g_task_id, sv_matmul_id);
        submit(g_task_id);
        online_softmax_task_by_b[b] = g_task_id;
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
        // dep every Func9 for batches in [b0, b0+cur_valid). Duration is the per-mix-
        // instance mean = max(aic, aiv_1, aiv_2) over the synchronized lanes.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_MIX);
        set_block_num(g_task_id, 40);
        add_input(g_task_id, ext_hidden_states);
        add_input(g_task_id, attn_out);
        add_input(g_task_id, ext_wo);
        add_inout(g_task_id, resid1_tile);
        add_output(g_task_id, gm_pipe_buffer_0);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, 40750);
        for (int64_t row = 0; row < cur_valid; row += 1) {
            succeed(g_task_id, online_softmax_task_by_b[b0 + row]);
        }
        submit(g_task_id);
        const uint16_t out_proj_mixed_id = g_task_id;

        // Task 12: post_rmsnorm (AIV, single) — dep Func10/11.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        add_input(g_task_id, resid1_tile);
        add_output(g_task_id, post_norm_tile);
        add_input(g_task_id, ext_post_rms_weight);
        add_duration(g_task_id, 24390);
        succeed(g_task_id, out_proj_mixed_id);
        submit(g_task_id);
        const uint16_t post_rmsnorm_id = g_task_id;

        // Spmd gate_proj (AIC, block_num 34): INTERMEDIATE / MLP_OUT_CHUNK = 17408/512 = 34
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 34);
        add_input(g_task_id, post_norm_tile);
        add_input(g_task_id, ext_w_gate);
        add_inout(g_task_id, gate_tile);
        add_duration(g_task_id, 95700);
        succeed(g_task_id, post_rmsnorm_id);
        submit(g_task_id);
        const uint16_t gate_id = g_task_id;

        // Spmd up_proj (AIC, block_num 34).
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 34);
        add_input(g_task_id, post_norm_tile);
        add_input(g_task_id, ext_w_up);
        add_inout(g_task_id, up_tile);
        add_duration(g_task_id, 97140);
        succeed(g_task_id, post_rmsnorm_id);
        submit(g_task_id);
        const uint16_t up_id = g_task_id;

        // Spmd silu (AIV, block_num 34) — dep Func13 + Func14.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        set_block_num(g_task_id, 34);
        add_input(g_task_id, gate_tile);
        add_input(g_task_id, up_tile);
        add_inout(g_task_id, mlp_tile);  // ret0__out_2 view -> base tensor
        add_duration(g_task_id, 2820);
        succeed(g_task_id, gate_id);
        succeed(g_task_id, up_id);
        submit(g_task_id);
        const uint16_t silu_id = g_task_id;

        // Spmd down_proj (AIC, block_num 40): HIDDEN / DOWN_OUT_CHUNK = 5120/128 = 40 —
        // reads full mlp_tile, dep Func15.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_CUBE);
        set_block_num(g_task_id, 40);
        add_input(g_task_id, mlp_tile);
        add_input(g_task_id, ext_w_down);
        add_inout(g_task_id, down_tile);
        add_duration(g_task_id, 72220);
        succeed(g_task_id, silu_id);
        submit(g_task_id);
        const uint16_t down_proj_id = g_task_id;

        // Spmd down_proj_residual (AIV, block_num 40) — dep Func16 and Func10/11.
        g_task_id++;
        while (try_new_task(g_task_id))
        {
            spin_wait();
        }
        set_task_type(g_task_id, TASK_TYPE_VECTOR);
        set_block_num(g_task_id, 40);
        add_input(g_task_id, down_tile);
        add_input(g_task_id, resid1_tile);
        add_output(g_task_id, ext_out);
        add_scalar(g_task_id, cur_valid);
        add_scalar(g_task_id, b0);
        add_duration(g_task_id, 2590);
        succeed(g_task_id, down_proj_id);
        succeed(g_task_id, out_proj_mixed_id);
        submit(g_task_id);
    }
    g_completed_cnt++;
}
