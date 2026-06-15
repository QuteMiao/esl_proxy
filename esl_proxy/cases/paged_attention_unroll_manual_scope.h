// Orchestration Function: paged_attention_unroll (manual-scope variant).
//
// Mirrors V200-benchmark/paged_attention/paged_attention_unroll_manual_scope/
// kernels/orchestration/paged_attention_orch.cpp. Case1 parameters (README.md
// §2.2.1):
//   batch=480, num_heads=16, head_dim=128, block_size=128, context_len=8192,
//   N_UNROLL=64 -> 1 group × 4 tasks per (batch, q-tile) = 1920 tasks total.
//
// Per (batch, q-tile, bn) group — task 0..3 within the group:
//   task 0: qk_matmul      (CUBE)   succeed chain: (none) -> sf -> pv -> online
//   task 1: softmax_prep   (VECTOR) succeed(sf, qk)
//   task 2: pv_matmul      (CUBE)   succeed(pv, sf)
//   task 3: online_update  (VECTOR) succeed(online, pv); optional succeed(online, pre)
//
// Durations are per-subtask means (README.md §2.2.3 AICore View) in ns.
#include <stddef.h>
#include <stdint.h>

#ifndef USE_TENSORMAP
#error "paged_attention_unroll_manual_scope.h requires -DUSE_TENSORMAP"
#endif

#include "dispatch.h"
#include "mem_pool.h"
#include "ring_buf.h"

extern atomic_int g_task_id;

#define PA_BATCH 480
#define PA_NUM_HEADS 16
#define PA_HEAD_DIM 128
#define PA_BLOCK_SIZE 128
#define PA_CONTEXT_LEN 8192
#define PA_N_UNROLL 64
#define PA_Q_TILE 16
#define PA_Q_LOOP 1
#define PA_BN_THIS_BATCH 64
#define PA_BLOCK_NUM 64

#define DUR_QK_MATMUL 51630
#define DUR_SOFTMAX_PREP 58820
#define DUR_PV_MATMUL 52610
#define DUR_ONLINE_UPDATE 2560

static inline void set_task_type(uint16_t task_id, task_type_t type) {
    g_basic_buf[task_id & RING_MASK].type = type;
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
    Tensor ext_query = tensor_from_base(orch_args + 0);
    Tensor ext_key_cache = tensor_from_base(orch_args + 1);
    Tensor ext_value_cache = tensor_from_base(orch_args + 2);
    Tensor ext_block_table = tensor_from_base(orch_args + 3);
    Tensor ext_context_lens = tensor_from_base(orch_args + 4);
    Tensor ext_out = tensor_from_base(orch_args + 5);
    (void)ext_context_lens;

    const uint64_t batch = PA_BATCH;
    const uint64_t q_loop = PA_Q_LOOP;
    const uint64_t q_tile = PA_Q_TILE;
    const uint64_t head_dim = PA_HEAD_DIM;
    const uint64_t block_size = PA_BLOCK_SIZE;
    const uint64_t bn_this_batch = PA_BN_THIS_BATCH;
    const uint64_t block_num = PA_BLOCK_NUM;
    const uint64_t q_head_num = PA_NUM_HEADS;

    Tensor oi = alloc_tensors((uint32_t[2]){q_tile, head_dim}, 2, FLOAT32);
    Tensor li_update = alloc_tensors((uint32_t[2]){1, q_tile}, 2, FLOAT32);
    Tensor mi_update = alloc_tensors((uint32_t[2]){1, q_tile}, 2, FLOAT32);

    for (uint64_t b_idx = 0; b_idx < batch; b_idx++) {
        for (uint64_t q_idx = 0; q_idx < q_loop; q_idx++) {
            const uint64_t cur_offset = b_idx * q_head_num + q_idx * q_tile;
            uint16_t pre_task_id = 0;
            int has_pre = 0;

            for (uint64_t bn = 0; bn < bn_this_batch; bn += PA_N_UNROLL) {
                const uint64_t n_blocks = (PA_N_UNROLL < (bn_this_batch - bn))
                                              ? PA_N_UNROLL
                                              : (bn_this_batch - bn);
                const uint64_t is_first = (bn == 0) ? 1 : 0;
                const uint64_t is_last = (bn + n_blocks >= bn_this_batch) ? 1 : 0;

                Tensor sij_buf = alloc_tensors((uint32_t[2]){q_tile, n_blocks * block_size}, 2, FLOAT32);

                /* task 0: qk_matmul */
                g_task_id++;
                while (new_task(g_task_id)) {
                    spin_wait();
                }
                set_task_type(g_task_id, TASK_TYPE_CUBE);
                add_input(g_task_id, ext_query);
                add_input(g_task_id, ext_key_cache);
                add_input(g_task_id, ext_block_table);
                add_output(g_task_id, sij_buf);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)(b_idx * block_num + bn));
                add_duration(g_task_id, DUR_QK_MATMUL);
                submit(g_task_id);
                const uint16_t qk_id = g_task_id;

                Tensor pij_buf = alloc_tensors((uint32_t[2]){q_tile, n_blocks * block_size}, 2, BFLOAT16);
                Tensor mi = alloc_tensors((uint32_t[2]){1, q_tile}, 2, FLOAT32);
                Tensor li = alloc_tensors((uint32_t[2]){1, q_tile}, 2, FLOAT32);

                /* task 1: softmax_prep */
                g_task_id++;
                while (new_task(g_task_id)) {
                    spin_wait();
                }
                set_task_type(g_task_id, TASK_TYPE_VECTOR);
                add_input(g_task_id, sij_buf);
                add_output(g_task_id, pij_buf);
                add_output(g_task_id, mi);
                add_output(g_task_id, li);
                add_scalar(g_task_id, 0x3f800000);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)block_size);
                add_duration(g_task_id, DUR_SOFTMAX_PREP);
                succeed(g_task_id, qk_id);
                submit(g_task_id);
                const uint16_t sf_id = g_task_id;

                Tensor oi_new = alloc_tensors((uint32_t[2]){q_tile, head_dim}, 2, FLOAT32);

                /* task 2: pv_matmul */
                g_task_id++;
                while (new_task(g_task_id)) {
                    spin_wait();
                }
                set_task_type(g_task_id, TASK_TYPE_CUBE);
                add_input(g_task_id, pij_buf);
                add_input(g_task_id, ext_value_cache);
                add_input(g_task_id, ext_block_table);
                add_output(g_task_id, oi_new);
                add_scalar(g_task_id, (int64_t)n_blocks);
                add_scalar(g_task_id, (int64_t)(b_idx * block_num + bn));
                add_duration(g_task_id, DUR_PV_MATMUL);
                succeed(g_task_id, sf_id);
                submit(g_task_id);
                const uint16_t pv_id = g_task_id;

                /* task 3: online_update */
                g_task_id++;
                while (new_task(g_task_id)) {
                    spin_wait();
                }
                set_task_type(g_task_id, TASK_TYPE_VECTOR);
                add_input(g_task_id, mi);
                add_input(g_task_id, li);
                add_input(g_task_id, oi_new);
                add_inout(g_task_id, mi_update);
                add_inout(g_task_id, li_update);
                add_inout(g_task_id, oi);
                add_inout(g_task_id, ext_out);
                add_scalar(g_task_id, (int64_t)is_first);
                add_scalar(g_task_id, (int64_t)is_last);
                add_duration(g_task_id, DUR_ONLINE_UPDATE);
                succeed(g_task_id, pv_id);
                if (!is_first && has_pre)
                    succeed(g_task_id, pre_task_id);
                submit(g_task_id);
                pre_task_id = g_task_id;
                has_pre = 1;
                (void)cur_offset;
            }
        }
    }
}
