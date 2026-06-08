/*
 * test_dep_dump_paged_attention_edges.c
 *
 * Runs paged_attention_unroll orchestration with real succeed() edge storage
 * and cross-checks dep_dump_count_edges() against an independent edge recorder.
 *
 * Optional: DEP_DUMP_EDGE_FILE=/path/to/edges.csv writes sorted edges for diff.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAG_RING_BUF_H
#define DAG_MEM_POOL_H
#define DAG_MPMC_QUEUE_H

typedef enum { BFLOAT16 = 2, FLOAT32 = 4 } dtype_t;

typedef struct {
    uint64_t base;
    uint32_t storage[2];
    uint32_t shapes[2];
    uint32_t offsets[2];
    uint32_t strides[2];
    dtype_t dtype;
} Tensor;

static inline uint64_t tensor_base(Tensor t) { return t.base; }

static inline Tensor tensor_from_base(uint64_t base)
{
    Tensor t;
    t.base = base;
    t.storage[0] = t.storage[1] = 0;
    t.shapes[0] = t.shapes[1] = 0;
    t.offsets[0] = t.offsets[1] = 0;
    t.strides[0] = t.strides[1] = 0;
    t.dtype = (dtype_t)0;
    return t;
}

static inline Tensor tensor_make_2d(uint64_t base, uint32_t d0, uint32_t d1,
                                    dtype_t dtype)
{
    Tensor t;
    t.base = base;
    t.storage[0] = t.shapes[0] = d0;
    t.storage[1] = t.shapes[1] = d1;
    t.offsets[0] = 0;
    t.offsets[1] = 0;
    t.strides[0] = d1;
    t.strides[1] = 1;
    t.dtype = dtype;
    return t;
}

static inline Tensor tensor_view(Tensor t, uint32_t dim, uint32_t off, uint32_t n)
{
    t.offsets[dim] += off;
    t.shapes[dim] = n;
    return t;
}

static inline Tensor tensor_row_view(Tensor t, uint32_t row0, uint32_t nrows)
{
    return tensor_view(t, 0u, row0, nrows);
}

static inline void add_input_ptr(uint16_t tid, const Tensor *t) { (void)tid; (void)t; }
static inline void add_output_ptr(uint16_t tid, const Tensor *t) { (void)tid; (void)t; }
static inline void add_inout_ptr(uint16_t tid, const Tensor *t) { (void)tid; (void)t; }
static inline void submit(uint16_t tid) { (void)tid; }

#include <stdatomic.h>

#include "conf.h"
#include "task.h"

atomic_int g_task_id = 0;
uint16_t g_min_uncomplete_task = 0;
_Atomic task_state g_state_buf[RING_SIZE];
_Atomic uint16_t g_predecessor_buf[RING_SIZE];
struct task_desc g_basic_buf[RING_SIZE];
struct succ_list g_successor_buf[RING_SIZE];
struct succ_list g_successor_exp_buf[HALF_RING_SIZE];

#define MAX_EDGES 500000
static uint16_t g_edge_c[MAX_EDGES];
static uint16_t g_edge_p[MAX_EDGES];
static int g_edge_n = 0;

static inline bool succeed(uint16_t c, uint16_t p)
{
    const int slot = p & RING_MASK;
    task_state expected =
        atomic_load_explicit(&g_state_buf[slot], memory_order_relaxed);
    expected.state = TASK_STATUS_CREATING;
    task_state desired = expected;
    desired.successor_cnt = expected.successor_cnt + 1;
    if (!atomic_compare_exchange_strong(&g_state_buf[slot], &expected,
                                        desired))
        return false;

    uint32_t idx = expected.successor_cnt;
    struct succ_list *ptr = &g_successor_buf[slot];
    while (idx >= (uint32_t)SUCC_NODE_CNT) {
        idx -= (uint32_t)SUCC_NODE_CNT;
        ptr = ptr->next;
    }
    ptr->successor[idx] = c;
    atomic_fetch_add_explicit(&g_predecessor_buf[c & RING_MASK], 1,
                              memory_order_relaxed);
    if (g_edge_n < MAX_EDGES) {
        g_edge_c[g_edge_n] = c;
        g_edge_p[g_edge_n] = p;
        g_edge_n++;
    }
    return true;
}

static inline bool try_new_task(uint32_t task_id)
{
    atomic_store_explicit(&g_predecessor_buf[task_id & RING_MASK], 1,
                          memory_order_relaxed);

    task_state st = {.state = TASK_STATUS_CREATING,
                     .successor_cnt = 0,
                     .task_id = (uint16_t)task_id};
    atomic_store_explicit(&g_state_buf[task_id & RING_MASK], st,
                          memory_order_relaxed);
    return true;
}

static int32_t g_dur[1u << 16];
static inline void add_duration(uint16_t tid, int64_t d)
{
    g_dur[tid] = (int32_t)d;
}
static inline void add_scalar(uint16_t tid, int64_t s)
{
    (void)tid;
    (void)s;
}

static inline void ring_buf_init_local(void)
{
    for (size_t i = 0; i < RING_SIZE; i++)
        g_successor_buf[i].next = &g_successor_exp_buf[i % HALF_RING_SIZE];
    for (size_t i = 0; i < HALF_RING_SIZE; i++)
        g_successor_exp_buf[i].next = NULL;
}

#include "tensormap.h"
#include "dep_dump.h"

static uint64_t g_alloc_bump = 0x100000u;
static inline Tensor alloc_tensors(uint32_t shape[], int dim, int bytes)
{
    uint64_t a = g_alloc_bump;
    uint64_t sz = (uint64_t)shape[0] * (uint64_t)shape[1] * (uint64_t)dim *
                  (uint64_t)bytes;
    if (sz < 64u)
        sz = 64u;
    g_alloc_bump += (sz + 63u) & ~(uint64_t)63u;
    return tensor_make_2d(a, shape[0], shape[1], (dtype_t)bytes);
}

static inline void spin_wait(void) {}

#include "paged_attention_unroll.h"

/* Keep external tensor handles (orch_args+0..5) away from scratch pool bump
 * (g_alloc_bump). orch_args=0 makes qi(b) span 0..~0x1df000, which collides
 * with pool@0x100000 and yields 2 spurious cross-batch edges (1442 vs 1440). */
#define PA_TEST_ORCH_ARGS 0x10000000ULL
#define PA_EXPECTED_EDGES 1440u

static int edge_cmp(const void *a, const void *b)
{
    const int ia = *(const int *)a;
    const int ib = *(const int *)b;
    if (g_edge_p[ia] != g_edge_p[ib])
        return (int)g_edge_p[ia] - (int)g_edge_p[ib];
    return (int)g_edge_c[ia] - (int)g_edge_c[ib];
}

static void maybe_write_sorted_edges(void)
{
    const char *path = getenv("DEP_DUMP_EDGE_FILE");
    if (path == NULL || path[0] == '\0')
        return;

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        perror("fopen DEP_DUMP_EDGE_FILE");
        exit(1);
    }

    int *idx = (int *)malloc((size_t)g_edge_n * sizeof(int));
    if (idx == NULL) {
        fclose(f);
        exit(1);
    }
    for (int i = 0; i < g_edge_n; i++)
        idx[i] = i;
    qsort(idx, (size_t)g_edge_n, sizeof(int), edge_cmp);

    fprintf(f, "producer,consumer\n");
    for (int i = 0; i < g_edge_n; i++) {
        const int k = idx[i];
        fprintf(f, "%u,%u\n", (unsigned)g_edge_p[k], (unsigned)g_edge_c[k]);
    }
    free(idx);
    fclose(f);
}

int main(void)
{
    ring_buf_init_local();
    aicpu_orchestration_entry(PA_TEST_ORCH_ARGS);

    const uint32_t dump_edges = dep_dump_count_edges();
    printf("tasks=%d recorder_edges=%d dump_edges=%u (expected %u)\n",
           (int)g_task_id, g_edge_n, dump_edges, PA_EXPECTED_EDGES);
    assert(g_edge_n < MAX_EDGES);
    assert(dump_edges == (uint32_t)g_edge_n);
    assert(dump_edges == PA_EXPECTED_EDGES);

    maybe_write_sorted_edges();
    dep_dump_summary(stdout);
    printf("test_dep_dump_paged_attention_edges: OK\n");
    return 0;
}
