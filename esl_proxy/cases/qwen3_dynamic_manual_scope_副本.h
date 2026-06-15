// Orchestration Function: qwen3_decode (dynamic manual-scope, configurable-SPMD
// variant).
//
// Mirrors
// V200-benchmark/qwen3/qwen3_dynamic_manual_scope/orchestration/qwen3_decode.cpp.
// Task/tensor wiring matches qwen3_dynamic_tensormap.h; cross-task ordering is
// expressed explicitly via add_predecessors() before submit(). SPMD tier:
// QWEN3_SPMD_TIER (0=non-spmd .. 4=all-spmd).
//
// Durations are V200-benchmark per-subtask means (README.md §1.2.1 AICore View)
// in ns.
#include <stddef.h>
#include <stdint.h>

#ifndef USE_TENSORMAP
#error "qwen3_dynamic_manual_scope.h requires -DUSE_TENSORMAP for struct Tensor"
#endif

#include "dispatch.h"
#include "mem_pool.h"
#include "ring_buf.h"

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

int g_subtask_cnt = 0;

static inline void set_task_type(uint16_t task_id, task_type_t type) {
    g_basic_buf[task_id & RING_MASK].type = type;
}

static inline void set_block_num(uint16_t task_id, uint32_t count) {
    g_basic_buf[task_id & RING_MASK].mode = ORG_MODE_SPMD_SYNC;
    g_basic_buf[task_id & RING_MASK].count = count;
    g_subtask_cnt += count;
}

static inline int qwen3_min_i(int a, int b) {
    return a < b ? a : b;
}

static inline int qwen3_blocks_per_task(int total_chunks) {
    static const int targets[5] = {1, 2, 4, 8, 1 << 30};
    int target = targets[QWEN3_SPMD_TIER];
    return qwen3_min_i(total_chunks, target);
}

static inline int qwen3_cur_blocks(int total_chunks, int base) {
    return qwen3_min_i(qwen3_blocks_per_task(total_chunks), total_chunks - base);
}

static inline int qwen3_n_tasks(int total_chunks, int bpt) {
    int n = 0;
    for (int base = 0; base < total_chunks; base += bpt)
        n++;
    return n;
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

    const int64_t user_batch = 96;
    const int64_t batch_padded = 96;
    ext_out = tensor_make_2d(tensor_base(ext_out), (uint32_t)batch_padded, 5120, BFLOAT16);

    Tensor q_proj = alloc_tensors((uint32_t[2]){batch_padded, 5120}, 2, FLOAT32);
    Tensor k_proj = alloc_tensors((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    Tensor v_proj = alloc_tensors((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);
    Tensor q_proj_norm = alloc_tensors((uint32_t[2]){batch_padded, 5120}, 2, FLOAT32);
    Tensor k_proj_norm = alloc_tensors((uint32_t[2]){batch_padded, 1024}, 2, FLOAT32);

    uint16_t qk_norm_per_tile[6];
    uint16_t v_ids_per_tile[6][8];
    uint16_t v_cnt_per_tile[6];
    uint16_t os_by_b[90][4];
    uint16_t os_cnt_by_b[90];
    uint16_t batch_predecessors[24];
    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        const size_t tix = (size_t)(b0 / 16);
        Tensor normed_tile = alloc_tensors((uint32_t[2]){16, 5120}, 2, BFLOAT16);
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);

        uint16_t q_ids[20];
        uint16_t k_ids[8];
        uint16_t v_ids[8];

        new_task(g_task_id, TASK_TYPE_VECTOR);
        add_input(g_task_id, ext_hidden_states);
        add_output(g_task_id, normed_tile);
        add_input(g_task_id, ext_input_rms_weight);
        add_scalar(g_task_id, b0);
        add_scalar(g_task_id, cur_valid);
        add_duration(g_task_id, DUR_RMSNORM);
        const uint16_t rmsnorm_id = g_task_id;
        g_task_id++;
        

        for (int qi = 0, base = 0; base < 20; base += qwen3_blocks_per_task(20)) {
            int cur_blocks = qwen3_cur_blocks(20, base);
            new_task(g_task_id, TASK_TYPE_CUBE);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, normed_tile);
            add_input(g_task_id, ext_wq);
            Tensor q_chunk = tensor_view_2d(q_proj, (uint32_t)b0, base * 256u, 16u, cur_blocks * 256u);
            add_output(g_task_id, q_chunk);
            add_scalar(g_task_id, b0);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_Q_PROJ);
            batch_predecessors[0] = rmsnorm_id;
            add_predecessors(g_task_id, batch_predecessors, 1, 0);
            q_ids[qi++] = g_task_id;
            g_task_id++;
            
        }

        for (int ki = 0, vi = 0, base = 0; base < 8; base += qwen3_blocks_per_task(8)) {
            int cur_blocks = qwen3_cur_blocks(8, base);
            new_task(g_task_id, TASK_TYPE_CUBE);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, normed_tile);
            add_input(g_task_id, ext_wk);
            Tensor k_chunk = tensor_view_2d(k_proj, (uint32_t)b0, base * 128u, 16u, cur_blocks * 128u);
            add_output(g_task_id, k_chunk);
            add_scalar(g_task_id, b0);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_K_PROJ);
            batch_predecessors[0] = rmsnorm_id;
            add_predecessors(g_task_id, batch_predecessors, 1, 0);
            k_ids[ki++] = g_task_id;
            g_task_id++;
            
            new_task(g_task_id, TASK_TYPE_CUBE);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, normed_tile);
            add_input(g_task_id, ext_wv);
            Tensor v_chunk = tensor_view_2d(v_proj, (uint32_t)b0, base * 128u, 16u, cur_blocks * 128u);
            add_output(g_task_id, v_chunk);
            add_scalar(g_task_id, b0);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_V_PROJ);
            batch_predecessors[0] = rmsnorm_id;
            add_predecessors(g_task_id, batch_predecessors, 1, 0);
            v_ids[vi++] = g_task_id;
            g_task_id++;
            
        }
        for (int i = 0; i < qwen3_n_tasks(8, qwen3_blocks_per_task(8)); i++)
            v_ids_per_tile[tix][i] = v_ids[i];
        v_cnt_per_tile[tix] = qwen3_n_tasks(8, qwen3_blocks_per_task(8));

        new_task(g_task_id, TASK_TYPE_VECTOR);
        Tensor k0_norm = tensor_view(k_proj_norm, 0u, (uint32_t)b0, 16);
        add_output(g_task_id, k0_norm);
        Tensor q0_norm = tensor_view(q_proj_norm, 0u, (uint32_t)b0, 16);
        add_output(g_task_id, q0_norm);
        Tensor q0_in = tensor_view(q_proj, 0u, (uint32_t)b0, 16);
        add_input(g_task_id, q0_in);
        add_input(g_task_id, ext_q_norm_weight);
        add_input(g_task_id, ext_k_norm_weight);
        Tensor k0_in = tensor_view(k_proj, 0u, (uint32_t)b0, 16);
        add_input(g_task_id, k0_in);
        add_duration(g_task_id, DUR_QK_NORM);
        int idx = add_predecessors(g_task_id, q_ids, qwen3_n_tasks(20, qwen3_blocks_per_task(20)), 0);
        add_predecessors(g_task_id, k_ids, qwen3_n_tasks(8, qwen3_blocks_per_task(8)), idx);
        qk_norm_per_tile[tix] = g_task_id;
        g_task_id++;
        
    }

    Tensor attn_out = alloc_tensors((uint32_t[2]){batch_padded, 5120}, 2, BFLOAT16);

    for (int64_t b = 0; b < user_batch; b += 1) {
        Tensor all_raw_scores = alloc_tensors((uint32_t[2]){4096, 128}, 2, FLOAT32);
        Tensor all_exp_padded = alloc_tensors((uint32_t[2]){4096, 128}, 2, BFLOAT16);
        Tensor all_cur_mi = alloc_tensors((uint32_t[2]){4096, 1}, 2, FLOAT32);
        Tensor all_cur_li = alloc_tensors((uint32_t[2]){4096, 1}, 2, FLOAT32);
        Tensor all_oi_tmp = alloc_tensors((uint32_t[2]){4096, 128}, 2, FLOAT32);
        Tensor q_padded_local = alloc_tensors((uint32_t[2]){128, 128}, 2, BFLOAT16);
        Tensor k_cache_local = tensor_make_2d(
            tensor_base(ext_k_cache) + (uint64_t)b * (uint64_t)1024u * (uint64_t)BFLOAT16, 1,
            1024, BFLOAT16);
        Tensor v_cache_local = tensor_make_2d(
            tensor_base(ext_v_cache) + (uint64_t)b * (uint64_t)1024u * (uint64_t)BFLOAT16, 1,
            1024, BFLOAT16);

        const int64_t b_tile0 = (b / 16) * 16;
        const size_t tix = (size_t)(b / 16);
        const int64_t ctx_len = 1024;
        const int64_t ctx_blocks = ((ctx_len + 127) / 128);
        const int64_t block_table_base = (b * 32);
        const int64_t slot = b;
        const int64_t slot_block = (slot / 128);
        const int64_t slot_offset = (slot - (slot_block * 128));

        uint16_t qk_ids[4];
        uint16_t sm_ids[4];
        uint16_t sv_ids[4];


        new_task(g_task_id, TASK_TYPE_VECTOR);
        add_output(g_task_id, q_padded_local);
        add_output(g_task_id, k_cache_local);
        add_output(g_task_id, v_cache_local);
        Tensor k0_norm = tensor_view(k_proj_norm, 0u, (uint32_t)b_tile0, 16u);
        Tensor v0 = tensor_view(v_proj, 0u, (uint32_t)b_tile0, 16u);
        Tensor q0_norm = tensor_view(q_proj_norm, 0u, (uint32_t)b_tile0, 16u);
        add_input(g_task_id, k0_norm);
        add_input(g_task_id, ext_rope_cos);
        add_input(g_task_id, ext_rope_sin);
        add_input(g_task_id, ext_rope_cos);
        add_input(g_task_id, ext_rope_sin);
        add_input(g_task_id, v0);
        add_input(g_task_id, q0_norm);
        add_scalar(g_task_id, slot_block);
        add_scalar(g_task_id, slot_offset);
        add_scalar(g_task_id, b);
        add_duration(g_task_id, DUR_ROPE_KV_CACHE);
        batch_predecessors[0] = qk_norm_per_tile[tix];
        add_predecessors(g_task_id, batch_predecessors, 1, 0);
        const uint16_t rope_id = g_task_id;
        g_task_id++;
        

        for (int ci = 0, base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
            int cur_blocks = qwen3_cur_blocks(4, base);
            Tensor row_piece =
                tensor_view(all_raw_scores, 0u, base * 1024u, cur_blocks * 1024u);

            new_task(g_task_id, TASK_TYPE_CUBE);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, q_padded_local);
            add_output(g_task_id, row_piece);
            add_input(g_task_id, ext_block_table);
            add_input(g_task_id, k_cache_local);
            add_scalar(g_task_id, b);
            add_scalar(g_task_id, ctx_blocks);
            add_scalar(g_task_id, block_table_base);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_QK_MATMUL);
            batch_predecessors[0] = rope_id;
            add_predecessors(g_task_id, batch_predecessors, 1, 0);
            qk_ids[ci] = g_task_id;
            g_task_id++;
            


            new_task(g_task_id, TASK_TYPE_VECTOR);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            Tensor cur_li_piece =
                tensor_view(all_cur_li, 0u, base * 1024u, cur_blocks * 1024u);
            Tensor cur_mi_piece =
                tensor_view(all_cur_mi, 0u, base * 1024u, cur_blocks * 1024u);
            Tensor exp_padded_piece =
                tensor_view(all_exp_padded, 0u, base * 1024u, cur_blocks * 1024u);
            add_output(g_task_id, cur_li_piece);
            add_output(g_task_id, cur_mi_piece);
            add_output(g_task_id, exp_padded_piece);
            add_input(g_task_id, row_piece);
            add_scalar(g_task_id, ctx_blocks);
            add_scalar(g_task_id, ctx_len);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_SOFTMAX);
            batch_predecessors[0] = qk_ids[ci];
            add_predecessors(g_task_id, batch_predecessors, 1, 0);
            sm_ids[ci] = g_task_id;
            g_task_id++;
            


            new_task(g_task_id, TASK_TYPE_CUBE);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            Tensor oi_tmp_piece =
                tensor_view(all_oi_tmp, 0u, base * 1024u, cur_blocks * 1024u);
            Tensor exp_piece =
                tensor_view(all_exp_padded, 0u, base * 1024u, cur_blocks * 1024u);
            add_output(g_task_id, oi_tmp_piece);
            add_input(g_task_id, ext_block_table);
            add_input(g_task_id, exp_piece);
            add_input(g_task_id, v_cache_local);
            add_scalar(g_task_id, ctx_blocks);
            add_scalar(g_task_id, block_table_base);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_SV_MATMUL);
            batch_predecessors[0] = rope_id;
            batch_predecessors[1] = sm_ids[ci];
            add_predecessors(g_task_id, batch_predecessors, 2, 0);
            sv_ids[ci] = g_task_id;
            g_task_id++;

            new_task(g_task_id, TASK_TYPE_VECTOR);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, oi_tmp_piece);
            add_input(g_task_id, cur_mi_piece);
            add_input(g_task_id, cur_li_piece);
            Tensor attn_out_piece = tensor_view_2d(attn_out, (uint32_t)b, base * 1280u, 1u,
                cur_blocks * 1280u);
            add_inout(g_task_id, attn_out_piece);
            add_scalar(g_task_id, ctx_blocks);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_ONLINE_SOFTMAX);
            batch_predecessors[0] = sv_ids[ci];
            batch_predecessors[1] = sm_ids[ci];
            add_predecessors(g_task_id, batch_predecessors, 2, 0);
            os_by_b[b][ci] = g_task_id;
            ci++;
            g_task_id++;

        }
        os_cnt_by_b[b] = qwen3_n_tasks(4, qwen3_blocks_per_task(4));
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

        uint16_t op_ids[40];
        uint16_t gate_ids[34];
        uint16_t up_ids[34];
        uint16_t silu_ids[34];
        uint16_t down_ids[40];

        for (int opi = 0, base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            int cur_blocks = qwen3_cur_blocks(40, base);

            new_task(g_task_id, TASK_TYPE_MIX);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, tensor_view(ext_hidden_states, 1u, base * 128u, cur_blocks * 128u));
            Tensor attn_out_tile =
                tensor_view(attn_out, 0u, (uint32_t)b0, (uint32_t)cur_valid);
            Tensor resid1_piece =
                tensor_view(resid1_tile, 1u, base * 128u, cur_blocks * 128u);
            add_input(g_task_id, attn_out_tile);
            add_input(g_task_id, tensor_view(ext_wo, 1u, base * 128u, cur_blocks * 128u));
            add_inout(g_task_id, resid1_piece);
            add_output(g_task_id, gm_pipe_buffer_0);
            add_scalar(g_task_id, b0);
            add_scalar(g_task_id, cur_valid);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_OUT_PROJ);
            uint16_t idx = 0;
            for (int64_t row = 0; row < cur_valid; row++) {
                const int64_t bb = b0 + row;
                idx = add_predecessors(g_task_id, os_by_b[bb], os_cnt_by_b[bb], idx);
            }
            op_ids[opi++] = g_task_id;
            g_task_id++;
            
        }

        new_task(g_task_id, TASK_TYPE_VECTOR);
        add_input(g_task_id, resid1_tile);
        add_output(g_task_id, post_norm_tile);
        add_input(g_task_id, ext_post_rms_weight);
        add_duration(g_task_id, DUR_POST_RMSNORM);
        add_predecessors(g_task_id, op_ids, qwen3_n_tasks(40, qwen3_blocks_per_task(40)), 0);
        const uint16_t post_rmsnorm_id = g_task_id;
        g_task_id++;
        

        for (int gi = 0, ui = 0, si = 0, base = 0; base < 34;
             base += qwen3_blocks_per_task(34)) {
            int cur_blocks = qwen3_cur_blocks(34, base);
            Tensor gate_piece = tensor_view(gate_tile, 1u, base * 512u, cur_blocks * 512u);
            Tensor up_piece = tensor_view(up_tile, 1u, base * 512u, cur_blocks * 512u);

            new_task(g_task_id, TASK_TYPE_CUBE);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, post_norm_tile);
            add_input(g_task_id, tensor_view(ext_w_gate, 1u, base * 512u, cur_blocks * 512u));
            add_inout(g_task_id, gate_piece);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_GATE_PROJ);
            batch_predecessors[0] = post_rmsnorm_id;
            add_predecessors(g_task_id, batch_predecessors, 1, 0);
            gate_ids[gi++] = g_task_id;
            g_task_id++;
            
            new_task(g_task_id, TASK_TYPE_CUBE);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, post_norm_tile);
            add_input(g_task_id, tensor_view(ext_w_up, 1u, base * 512u, cur_blocks * 512u));
            add_inout(g_task_id, up_piece);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_UP_PROJ);
            batch_predecessors[0] = post_rmsnorm_id;
            add_predecessors(g_task_id, batch_predecessors, 1, 0);
            up_ids[ui++] = g_task_id;
            g_task_id++;
            
            new_task(g_task_id, TASK_TYPE_VECTOR);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, gate_piece);
            add_input(g_task_id, up_piece);
            Tensor mlp_piece = tensor_view(mlp_tile, 1u, base * 512u, cur_blocks * 512u);
            add_inout(g_task_id, mlp_piece);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_SILU);
            batch_predecessors[0] = gate_ids[si];
            batch_predecessors[1] = up_ids[si];
            add_predecessors(g_task_id, batch_predecessors, 2, 0);
            silu_ids[si++] = g_task_id;
            g_task_id++;
            
        }

        for (int di = 0, dri = 0, base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            int cur_blocks = qwen3_cur_blocks(40, base);
            Tensor down_piece = tensor_view(down_tile, 1u, base * 128u, cur_blocks * 128u);
            Tensor resid1_dres_piece = tensor_view(resid1_tile, 1u, base * 128u, cur_blocks * 128u);

            new_task(g_task_id, TASK_TYPE_CUBE);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, mlp_tile);
            add_input(g_task_id, tensor_view(ext_w_down, 1u, base * 128u, cur_blocks * 128u));
            add_inout(g_task_id, down_piece);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_DOWN_PROJ);
            add_predecessors(g_task_id, silu_ids, qwen3_n_tasks(34, qwen3_blocks_per_task(34)), 0);
            down_ids[di++] = g_task_id;
            g_task_id++;
            
            new_task(g_task_id, TASK_TYPE_VECTOR);
            set_block_num(g_task_id, (uint32_t)cur_blocks);
            add_input(g_task_id, down_piece);
            add_input(g_task_id, resid1_dres_piece);
            Tensor out_tile = tensor_view(ext_out, 0u, (uint32_t)b0, (uint32_t)cur_valid);
            add_output(g_task_id, tensor_view(out_tile, 1u, base * 128u, cur_blocks * 128u));
            add_scalar(g_task_id, cur_valid);
            add_scalar(g_task_id, b0);
            add_scalar(g_task_id, base);
            add_duration(g_task_id, DUR_DOWN_PROJ_RES);
            batch_predecessors[0] = down_ids[dri];
            batch_predecessors[1] = op_ids[dri];
            add_predecessors(g_task_id, batch_predecessors, 2, 0);
            dri++;
            g_task_id++;
        }
    }
}
