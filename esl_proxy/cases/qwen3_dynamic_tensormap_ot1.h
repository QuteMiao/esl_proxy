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

#include "mem_pool.h"
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

int g_subtask_cnt = 0;
int local_task_id = 0;
static inline void set_task_type(uint32_t task_id, task_type_t type) {
    g_basic_buf[task_id & RING_MASK].type = type;
}

static inline void set_block_num(uint32_t task_id, uint32_t count) {
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

int desc_start = 0;
int desc_end = 0;
int local_index = 0;
atomic_int g_desc_task_id = 0;

#define DESC_BATCH_SIZE 60
#define MAX_TENSOR_NUM 4096
extern Tensor g_tensors[MAX_TENSOR_NUM];

void get_desc_tasks() {
    desc_start = atomic_fetch_add(&g_desc_task_id, DESC_BATCH_SIZE);
    desc_end = desc_start + DESC_BATCH_SIZE;
}

static inline Tensor get_tensor()
{
    return g_tensors[local_index++];
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
    get_desc_tasks();
    Tensor ext_hidden_states = tensor_from_base_layout(orch_args + 0, (uint32_t[]){90, 5120}, 2, BFLOAT16); // batch=90, hidden=5120
    Tensor ext_input_rms_weight = tensor_from_base_layout(orch_args + 1, (uint32_t[]){1, 5120}, 2, FLOAT32); // hidden=5120
    Tensor ext_wq = tensor_from_base_layout(orch_args + 2, (uint32_t[]){5120, 5120}, 2, BFLOAT16); // hidden=5120
    Tensor ext_wk = tensor_from_base_layout(orch_args + 3, (uint32_t[]){5120, 1024}, 2, BFLOAT16); // hidden=5120, kv_hidden=1024
    Tensor ext_wv = tensor_from_base_layout(orch_args + 4, (uint32_t[]){5120, 1024}, 2, BFLOAT16); // hidden=5120, kv_hidden=1024
    Tensor ext_q_norm_weight = tensor_from_base_layout(orch_args + 5, (uint32_t[]){1, 128}, 2, FLOAT32); // head_dim=128
    Tensor ext_k_norm_weight = tensor_from_base_layout(orch_args + 6, (uint32_t[]){1, 128}, 2, FLOAT32); // head_dim=128
    Tensor ext_seq_lens = tensor_from_base_layout(orch_args + 7, (uint32_t[]){90}, 1, INT32); // batch=90
    Tensor ext_block_table = tensor_from_base_layout(orch_args + 8, (uint32_t[]){2880}, 1, INT32); // num_blocks=2880
    Tensor ext_slot_mapping = tensor_from_base_layout(orch_args + 9, (uint32_t[]){90}, 1, INT32); // batch=90
    Tensor ext_rope_cos = tensor_from_base_layout(orch_args + 10, (uint32_t[]){4096, 128}, 2, FLOAT32); // max_seq=4096, head_dim=128
    Tensor ext_rope_sin = tensor_from_base_layout(orch_args + 11, (uint32_t[]){4096, 128}, 2, FLOAT32); // max_seq=4096, head_dim=128
    Tensor ext_k_cache = tensor_from_base_layout(orch_args + 12, (uint32_t[]){2949120, 128}, 2, BFLOAT16); // cache_rows=2880*8*128, head_dim=128
    Tensor ext_v_cache = tensor_from_base_layout(orch_args + 13, (uint32_t[]){2949120, 128}, 2, BFLOAT16); // cache_rows=2880*8*128, head_dim=128
    Tensor ext_wo = tensor_from_base_layout(orch_args + 14, (uint32_t[]){5120, 5120}, 2, BFLOAT16); // hidden=5120
    Tensor ext_post_rms_weight = tensor_from_base_layout(orch_args + 15, (uint32_t[]){1, 5120}, 2, FLOAT32); // hidden=5120
    Tensor ext_w_gate = tensor_from_base_layout(orch_args + 16, (uint32_t[]){5120, 17408}, 2, BFLOAT16); // hidden=5120, intermediate=17408
    Tensor ext_w_up = tensor_from_base_layout(orch_args + 17, (uint32_t[]){5120, 17408}, 2, BFLOAT16); // hidden=5120, intermediate=17408
    Tensor ext_w_down = tensor_from_base_layout(orch_args + 18, (uint32_t[]){17408, 5120}, 2, BFLOAT16); // intermediate=17408, hidden=5120
    Tensor ext_out = tensor_from_base_layout(orch_args + 19, (uint32_t[]){90, 5120}, 2, BFLOAT16); // batch=90, hidden=5120
    (void)ext_seq_lens;
    (void)ext_slot_mapping;
    tm_deps_init();
    const int64_t user_batch = 90; // batch=90
    const int64_t batch_padded = 96; // ((batch+15)/16)*16
    Tensor q_proj = get_tensor();
    Tensor k_proj = get_tensor();
    Tensor v_proj = get_tensor();
    Tensor q_proj_norm = get_tensor();
    Tensor k_proj_norm = get_tensor();

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        Tensor normed_tile = get_tensor();
        if (local_task_id >= desc_start)
        {
            const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);
            new_task(local_task_id, TASK_TYPE_VECTOR, 1, DUR_RMSNORM);
            tm_in_ro(local_task_id, ext_hidden_states);
            tm_out(local_task_id, normed_tile);
            tm_in_ro(local_task_id, ext_input_rms_weight);
            add_scalar(local_task_id, b0);
            add_scalar(local_task_id, cur_valid);
        } else if(local_task_id > desc_end){
            get_desc_tasks();
        }
        local_task_id++;

        for (int base = 0; base < 20; base += qwen3_blocks_per_task(20)) {
            if (local_task_id >= desc_start) {
                int cur_blocks = qwen3_cur_blocks(20, base);
                new_task(local_task_id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_Q_PROJ);
                Tensor q_piece = view(q_proj, (uint32_t)b0, base * 256u, 16u, cur_blocks * 256u);
                tm_in(local_task_id, normed_tile);
                tm_in_ro(local_task_id, ext_wq);
                tm_out(local_task_id, q_piece);
                add_scalar(local_task_id, b0);
                add_scalar(local_task_id, base);
            } else if(local_task_id > desc_end){
                get_desc_tasks();
            }
            local_task_id++;
        }

        for (int base = 0; base < 8; base += qwen3_blocks_per_task(8)) {
            int cur_blocks = qwen3_cur_blocks(8, base);
            if (local_task_id >= desc_start) {
                new_task(local_task_id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_K_PROJ);
                tm_in(local_task_id, normed_tile);
                tm_in_ro(local_task_id, ext_wk);
                Tensor k_piece = view(k_proj, (uint32_t)b0, base * 128u, 16u, cur_blocks * 128u);
                tm_out(local_task_id, k_piece);
                add_scalar(local_task_id, b0);
                add_scalar(local_task_id, base);
            } else if(local_task_id > desc_end){
                get_desc_tasks();
            }
            local_task_id++;

            if (local_task_id >= desc_start) {
                new_task(local_task_id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_V_PROJ);
                tm_in(local_task_id, normed_tile);
                tm_in_ro(local_task_id, ext_wv);
                Tensor v_piece = view(v_proj, (uint32_t)b0, base * 128u, 16u, cur_blocks * 128u);
                tm_out(local_task_id, v_piece);
                add_scalar(local_task_id, b0);
                add_scalar(local_task_id, base); 
            } else if(local_task_id > desc_end){
                get_desc_tasks();
            }
            local_task_id++;
        }

        if (local_task_id >= desc_start) {
            new_task(local_task_id, TASK_TYPE_VECTOR, 1, DUR_QK_NORM);
            Tensor k0_norm = view(k_proj_norm, (uint32_t)b0, 0u, 16u, 1024u);
            Tensor q0_norm = view(q_proj_norm, (uint32_t)b0, 0u, 16u, 5120u);
            Tensor q0_in = view(q_proj, (uint32_t)b0, 0u, 16u, 5120u);
            Tensor k0_in = view(k_proj, (uint32_t)b0, 0u, 16u, 1024u);
            tm_out(local_task_id, k0_norm);
            tm_out(local_task_id, q0_norm);
            tm_in(local_task_id, q0_in);
            tm_in_ro(local_task_id, ext_q_norm_weight);
            tm_in_ro(local_task_id, ext_k_norm_weight);
            tm_in(local_task_id, k0_in);
        } else if(local_task_id > desc_end){
            get_desc_tasks();
        }
        local_task_id++;
    }

    Tensor attn_out[6];
    for (int i = 0; i < 6; i++) {
        attn_out[i] = get_tensor();
    }

    for (int64_t b = 0; b < user_batch; b += 1) {
        Tensor all_raw_scores = get_tensor();
        Tensor all_exp_padded = get_tensor();
        Tensor all_cur_mi = get_tensor();
        Tensor all_cur_li = get_tensor();
        Tensor all_oi_tmp = get_tensor();
        Tensor q_padded_local = get_tensor();
        Tensor k_cache_local = view(ext_k_cache, (uint32_t)b * 8u, 0u, 8u, 128u); // batch b: 8*head_dim=1024 kv_hidden
        Tensor v_cache_local = view(ext_v_cache, (uint32_t)b * 8u, 0u, 8u, 128u); // batch b: 8*head_dim=1024 kv_hidden
        Tensor k_cache_update = get_tensor(); // ROPE KV write-back
        Tensor v_cache_update = get_tensor(); // ROPE KV write-back
        const int64_t b_tile0 = (b / 16) * 16;
        const int64_t slot = b;
        const int64_t slot_block = slot / 128;
        const int64_t slot_offset = slot - slot_block * 128;
        new_task(local_task_id, TASK_TYPE_VECTOR, 1, DUR_ROPE_KV_CACHE);
        Tensor k0_norm = view(k_proj_norm, (uint32_t)b_tile0, 0u, 16u, 1024u);
        Tensor v0 = view(v_proj, (uint32_t)b_tile0, 0u, 16u, 1024u);
        Tensor q0_norm = view(q_proj_norm, (uint32_t)b_tile0, 0u, 16u, 5120u);
        tm_out(local_task_id, q_padded_local);
        tm_in_ro(local_task_id, k_cache_local);
        tm_in_ro(local_task_id, v_cache_local);
        tm_out(local_task_id, k_cache_update);
        tm_out(local_task_id, v_cache_update);
        tm_in(local_task_id, k0_norm);
        tm_in_ro(local_task_id, ext_rope_cos);
        tm_in_ro(local_task_id, ext_rope_sin);
        tm_in_ro(local_task_id, ext_rope_cos);
        tm_in_ro(local_task_id, ext_rope_sin);
        tm_in(local_task_id, v0);
        tm_in(local_task_id, q0_norm);
        add_scalar(local_task_id, slot_block);
        add_scalar(local_task_id, slot_offset);
        add_scalar(local_task_id, b);
        tm_submit(local_task_id);
        local_task_id++;

        for (int base = 0; base < 4; base += qwen3_blocks_per_task(4)) {
            int cur_blocks = qwen3_cur_blocks(4, base);
            Tensor row_piece = view(all_raw_scores, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            new_task(local_task_id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_QK_MATMUL);
            tm_in(local_task_id, q_padded_local);
            tm_out(local_task_id, row_piece);
            tm_in_ro(local_task_id, ext_block_table);
            tm_in(local_task_id, k_cache_update);
            add_scalar(local_task_id, b);
            add_scalar(local_task_id, 8);      // (1024+127)/128: KV context blocks
            add_scalar(local_task_id, b * 32); // block_table row offset for batch b
            add_scalar(local_task_id, base);
            tm_submit(local_task_id);
            local_task_id++;

            new_task(local_task_id, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_SOFTMAX);
            Tensor cur_li_piece = view(all_cur_li, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 1u);
            Tensor cur_mi_piece = view(all_cur_mi, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 1u);
            Tensor exp_padded_piece = view(all_exp_padded, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            tm_out(local_task_id, cur_li_piece);
            tm_out(local_task_id, cur_mi_piece);
            tm_out(local_task_id, exp_padded_piece);
            tm_in(local_task_id, row_piece);
            add_scalar(local_task_id, 8);    // (1024+127)/128: KV context blocks
            add_scalar(local_task_id, 1024); // context length (tokens)
            add_scalar(local_task_id, base);
            local_task_id++;

            Tensor exp_piece = view(all_exp_padded, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            new_task(local_task_id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_SV_MATMUL);
            Tensor oi_tmp_piece = view(all_oi_tmp, base * 1024u, 0u, (uint32_t)(cur_blocks * 1024), 128u);
            tm_out(local_task_id, oi_tmp_piece);
            tm_in_ro(local_task_id, ext_block_table);
            tm_in(local_task_id, exp_piece);
            tm_in(local_task_id, v_cache_update);
            add_scalar(local_task_id, 8);      // (1024+127)/128: KV context blocks
            add_scalar(local_task_id, b * 32); // block_table row offset for batch b
            add_scalar(local_task_id, base);
            local_task_id++;

            new_task(local_task_id, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_ONLINE_SOFTMAX);
            tm_in(local_task_id, oi_tmp_piece);
            tm_in(local_task_id, cur_mi_piece);
            tm_in(local_task_id, cur_li_piece);
            Tensor attn_out_piece = view(attn_out[b / 16], (uint32_t)(b % 16),
                base * 1280u, 1u, cur_blocks * 1280u);
            tm_inout(local_task_id, attn_out_piece);
            add_scalar(local_task_id, 8); // (1024+127)/128: KV context blocks
            add_scalar(local_task_id, base);
            local_task_id++;
        }
    }

    for (int64_t b0 = 0; b0 < batch_padded; b0 += 16) {
        Tensor resid1_tile = get_tensor();
        Tensor gm_pipe_buffer_0 = get_tensor();
        Tensor post_norm_tile = get_tensor();
        Tensor mlp_tile = get_tensor();
        Tensor gate_tile = get_tensor();
        Tensor up_tile = get_tensor();
        Tensor down_tile = get_tensor();
        const int64_t cur_valid = (user_batch - b0 > 16) ? 16 : (user_batch - b0);
        for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            // 40: out_proj SPMD total chunks; cols/chunk = 5120/40 = 128
            int cur_blocks = qwen3_cur_blocks(40, base);
            new_task(local_task_id, TASK_TYPE_MIX, (uint32_t)cur_blocks, DUR_OUT_PROJ);
            Tensor attn_out_tile = view(attn_out[b0 / 16], 0u, 0u, (uint32_t)cur_valid, 5120u);
            Tensor resid1_piece = view(resid1_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            tm_in_ro(local_task_id, ext_hidden_states);
            tm_in(local_task_id, attn_out_tile);
            tm_in_ro(local_task_id, ext_wo);
            tm_inout(local_task_id, resid1_piece);
            tm_out(local_task_id, gm_pipe_buffer_0);
            add_scalar(local_task_id, b0);
            add_scalar(local_task_id, cur_valid);
            add_scalar(local_task_id, base);
            tm_submit(local_task_id);
            local_task_id++;
        }

        new_task(local_task_id, TASK_TYPE_VECTOR, 1, DUR_POST_RMSNORM);
        tm_in(local_task_id, resid1_tile);
        tm_out(local_task_id, post_norm_tile);
        tm_in_ro(local_task_id, ext_post_rms_weight);
        tm_submit(local_task_id);
        local_task_id++;

        for (int base = 0; base < 34; base += qwen3_blocks_per_task(34)) {
            int cur_blocks = qwen3_cur_blocks(34, base);
            Tensor gate_piece = view(gate_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            Tensor up_piece = view(up_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            new_task(local_task_id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_GATE_PROJ);
            tm_in(local_task_id, post_norm_tile);
            tm_in_ro(local_task_id, ext_w_gate);
            tm_inout(local_task_id, gate_piece);
            add_scalar(local_task_id, base);
            tm_submit(local_task_id);
            local_task_id++;

            new_task(local_task_id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_UP_PROJ);
            tm_in(local_task_id, post_norm_tile);
            tm_in_ro(local_task_id, ext_w_up);
            tm_inout(local_task_id, up_piece);
            add_scalar(local_task_id, base);
            tm_submit(local_task_id);
            local_task_id++;

            new_task(local_task_id, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_SILU);
            tm_in(local_task_id, gate_piece);
            tm_in(local_task_id, up_piece);
            Tensor mlp_piece = view(mlp_tile, 0u, base * 512u, 16u, (uint32_t)(cur_blocks * 512));
            tm_inout(local_task_id, mlp_piece);
            add_scalar(local_task_id, base);
            tm_submit(local_task_id);
            local_task_id++;
        }
        for (int base = 0; base < 40; base += qwen3_blocks_per_task(40)) {
            int cur_blocks = qwen3_cur_blocks(40, base);
            Tensor down_piece = view(down_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            Tensor resid1_piece = view(resid1_tile, 0u, base * 128u, 16u, (uint32_t)(cur_blocks * 128));
            new_task(local_task_id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, DUR_DOWN_PROJ);
            tm_in(local_task_id, mlp_tile);
            tm_in_ro(local_task_id, ext_w_down);
            tm_inout(local_task_id, down_piece);
            add_scalar(local_task_id, base);
            tm_submit(local_task_id);
            local_task_id++;

            new_task(local_task_id, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, DUR_DOWN_PROJ_RES);
            tm_in(local_task_id, down_piece);
            tm_in(local_task_id, resid1_piece);
            tm_out_ro(local_task_id, ext_out);
            add_scalar(local_task_id, cur_valid);
            add_scalar(local_task_id, b0);
            add_scalar(local_task_id, base);
            tm_submit(local_task_id);
            local_task_id++;
        }
    }
}
