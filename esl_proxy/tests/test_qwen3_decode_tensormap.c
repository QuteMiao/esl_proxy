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

enum {
    DUR_RMSNORM = 22780, DUR_QPROJ = 26980, DUR_KPROJ = 17770, DUR_VPROJ = 19140,
    DUR_QKNORM = 13380, DUR_ONLINE = 20440, DUR_OUTPROJ = 91230, DUR_SILU = 2940,
    DUR_DOWN = 74320
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

    /* qk_norm of the FIRST tile: reads q_proj + k_proj -> all 20 + 8 chunk
     * producers of that tile, and NOT v_proj (which it never reads). */
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
    assert(nq == 20 && nk == 8 && nv == 0);

    /* a q_proj chunk depends on exactly its tile's rmsnorm (via normed_tile). */
    qp = first_tid_with_dur(DUR_QPROJ);
    assert(qp >= 0);
    n = producers_of((uint16_t)qp, pr, 4096);
    nr = 0;
    for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_RMSNORM) nr++;
    printf("q_proj[t%d]:    producers=%d  (rmsnorm=%d)\n", qp, n, nr);
    assert(n == 1 && nr == 1);

    /* out_proj reads attn_out whole -> depends on EVERY online_softmax that wrote
     * it (90 batches x 4 = 360): the documented whole-buffer over-synchronization. */
    op = first_tid_with_dur(DUR_OUTPROJ);
    assert(op >= 0);
    n = producers_of((uint16_t)op, pr, 4096);
    no = 0;
    for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_ONLINE) no++;
    printf("out_proj[t%d]:  producers=%d  (online_softmax=%d  <- whole-buffer over-sync)\n", op, n, no);
    assert(no == 360 && n == 360);

    /* down_proj reads full mlp_tile -> all 34 silu chunks of its tile. */
    dp = first_tid_with_dur(DUR_DOWN);
    assert(dp >= 0);
    n = producers_of((uint16_t)dp, pr, 4096);
    ns = 0;
    for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_SILU) ns++;
    printf("down_proj[t%d]: producers=%d  (silu=%d)\n", dp, n, ns);
    assert(ns == 34 && n == 34);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
