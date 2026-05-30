/*
 * test_qwen3_decode_tensormap.c
 *
 * Compiles and RUNS the real cases/qwen3_decode_tensormap.h entry to verify the
 * tensormap auto-dependency conversion (producer discovery + edge set).
 *
 * esl_proxy's own mem_pool.h/ring_buf.h/task.h do NOT compile under the project
 * flags (pre-existing: g_state_buf type clash, when2free_fifo_t used before
 * defined, GNU range-init, implicit-int lock(), u_int32_t, undeclared
 * ready_enqueue). We neutralize their include guards and supply compatible stubs
 * so the converted entry can be exercised. The stub succeed() records every
 * resolved (consumer, producer) edge; add_duration() tags each task by its unique
 * per-kernel duration so assertions can identify task types.
 *
 * Build:
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic -I include -I cases \
 *       tests/test_qwen3_decode_tensormap.c -o /tmp/test_qwen3_tm
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Neutralize esl_proxy's pre-broken headers; we supply the symbols ourselves. */
#define DAG_TASK_H
#define DAG_RING_BUF_H
#define DAG_MEM_POOL_H
#define DAG_MPMC_QUEUE_H

#define Tensor uint64_t
#define BFLOAT16 2
#define FLOAT32 4

/* Symbols the case file's set_task_type()/set_block_num() helpers need from the
 * neutralized task.h/conf.h. A flat g_basic_buf (RING_MASK == identity over the
 * task ids used here) is enough to record type/mode/count. */
#define RING_SIZE 65536
#define RING_MASK (RING_SIZE - 1)
typedef enum { TASK_TYPE_CUBE = 0, TASK_TYPE_VECTOR = 1, TASK_TYPE_MIX = 2 } task_type_t;
typedef enum { ORG_MODE_SINGLE = 0, ORG_MODE_SPMD_SYNC = 2 } org_mode_t;
struct task_desc { task_type_t type; org_mode_t mode; uint32_t count; };
static struct task_desc g_basic_buf[RING_SIZE];
static inline void spin_wait(void) {}

static uint16_t g_task_id = 0;
static uint16_t g_min_uncomplete_task = 0;

static uint64_t g_alloc_bump = 0x100000u;
static inline Tensor alloc_tensors(uint32_t shape[], int dim, int bytes) {
    uint64_t a = g_alloc_bump;
    uint64_t sz = (uint64_t)shape[0] * (uint64_t)shape[1] * (uint64_t)dim * (uint64_t)bytes;
    if (sz < 64u) sz = 64u;
    g_alloc_bump += (sz + 63u) & ~(uint64_t)63u;
    return a;
}
static inline void wait(void) {}
static inline bool try_new_task(uint32_t id) {
    (void)id;
    return false; /* claim succeeds immediately; loop body never runs */
}

static int32_t g_dur[1u << 16];
static inline void add_duration(uint16_t tid, int64_t d) { g_dur[tid] = (int32_t)d; }
static inline void add_scalar(uint16_t tid, int64_t s) { (void)tid; (void)s; }
static inline void add_input(uint16_t tid, Tensor t) { (void)tid; (void)t; }
static inline void add_output(uint16_t tid, Tensor t) { (void)tid; (void)t; }
static inline void add_inout(uint16_t tid, Tensor t) { (void)tid; (void)t; }

#define MAX_EDGES 1000000
static uint16_t g_edge_c[MAX_EDGES];
static uint16_t g_edge_p[MAX_EDGES];
static int g_edge_n = 0;
static inline bool succeed(uint16_t c, uint16_t p) {
    if (g_edge_n < MAX_EDGES) {
        g_edge_c[g_edge_n] = c;
        g_edge_p[g_edge_n] = p;
        g_edge_n++;
    }
    return true;
}
static inline void submit(uint16_t tid) { (void)tid; }

#include "qwen3_decode_tensormap.h"

/* Per-kernel durations, matching cases/qwen3_decode_tensormap.h (used as task
 * type tags so assertions can identify producers by kernel). */
enum {
    DUR_RMSNORM = 23950, DUR_QPROJ = 26060, DUR_KPROJ = 18170, DUR_VPROJ = 17890,
    DUR_QKNORM = 13190, DUR_ONLINE = 20820, DUR_OUTPROJ = 40750, DUR_SILU = 2820,
    DUR_DOWN = 72220
};

static int first_tid_with_dur(int32_t d) {
    for (int t = 1; t <= (int)g_task_id; t++) {
        if (g_dur[t] == d) return t;
    }
    return -1;
}

static int producers_of(uint16_t c, uint16_t *out, int cap) {
    int n = 0;
    for (int i = 0; i < g_edge_n; i++) {
        if (g_edge_c[i] == c && n < cap) out[n++] = g_edge_p[i];
    }
    return n;
}

int main(void) {
    static uint16_t pr[4096];
    int qk, qp, op, dp, n, i;
    int nq, nk, nv, no, ns, nr;

    aicpu_orchestration_entry(0);
    printf("tasks=%d edges=%d\n", (int)g_task_id, g_edge_n);
    assert(g_edge_n < MAX_EDGES); /* no truncation */

    /* qk_norm of the FIRST tile reads block `tix` of q_proj and k_proj (its own
     * tile), so it depends on exactly that tile's q_proj + k_proj tasks (one
     * each) and NOT v_proj (never read) nor any other tile. */
    qk = first_tid_with_dur(DUR_QKNORM);
    assert(qk >= 0);
    n = producers_of((uint16_t)qk, pr, 4096);
    nq = nk = nv = 0;
    for (i = 0; i < n; i++) {
        int32_t d = g_dur[pr[i]];
        if (d == DUR_QPROJ) nq++;
        else if (d == DUR_KPROJ) nk++;
        else if (d == DUR_VPROJ) nv++;
    }
    printf("qk_norm[t%d]:   producers=%d  (q_proj=%d k_proj=%d v_proj=%d)\n", qk, n, nq, nk, nv);
    assert(nq == 1 && nk == 1 && nv == 0);

    /* a q_proj task depends on exactly its tile's rmsnorm (via normed_tile). */
    qp = first_tid_with_dur(DUR_QPROJ);
    assert(qp >= 0);
    n = producers_of((uint16_t)qp, pr, 4096);
    nr = 0;
    for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_RMSNORM) nr++;
    printf("q_proj[t%d]:    producers=%d  (rmsnorm=%d)\n", qp, n, nr);
    assert(n == 1 && nr == 1);

    /* out_proj of the FIRST tile reads attn_out rows [0, 16) -> depends on only
     * the 16 online_softmax tasks of its tile, NOT all 90 batches. This is the
     * over-synchronization that whole-buffer granularity used to introduce
     * (90 producers); block views cut it to the tile's batch count. */
    op = first_tid_with_dur(DUR_OUTPROJ);
    assert(op >= 0);
    n = producers_of((uint16_t)op, pr, 4096);
    no = 0;
    for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_ONLINE) no++;
    printf("out_proj[t%d]:  producers=%d  (online_softmax=%d  <- block-view, was 90)\n", op, n, no);
    assert(no == 16 && n == 16);

    /* The LAST tile covers batches [80, 90): only 10 valid -> 10 online_softmax,
     * demonstrating per-tile precision (out_proj edge count tracks cur_valid). */
    {
        int last_op = -1, j;
        for (j = 1; j <= (int)g_task_id; j++) if (g_dur[j] == DUR_OUTPROJ) last_op = j;
        n = producers_of((uint16_t)last_op, pr, 4096);
        no = 0;
        for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_ONLINE) no++;
        printf("out_proj[t%d]:  producers=%d  (online_softmax=%d  <- last tile, cur_valid=10)\n",
               last_op, n, no);
        assert(no == 10 && n == 10);
    }

    /* down_proj reads full (whole-buffer) mlp_tile, which silu writes as one
     * tile-local task -> a single producer. */
    dp = first_tid_with_dur(DUR_DOWN);
    assert(dp >= 0);
    n = producers_of((uint16_t)dp, pr, 4096);
    ns = 0;
    for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_SILU) ns++;
    printf("down_proj[t%d]: producers=%d  (silu=%d)\n", dp, n, ns);
    assert(ns == 1 && n == 1);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
