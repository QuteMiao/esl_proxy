/*
 * test_dep_dump_qwen3_edges.c
 *
 * Runs qwen3_dynamic_tensormap orchestration with real succeed() edge storage
 * and cross-checks dep_dump_count_edges() against an independent edge recorder.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

static inline Tensor tensor_chunk_2d(Tensor buf, uint32_t row0, uint32_t nrows,
                                     uint32_t col0, uint32_t ncols, dtype_t dtype)
{
    const uint64_t es = (uint64_t)dtype;
    const uint64_t off =
        (uint64_t)row0 * buf.strides[0] + (uint64_t)col0 * buf.strides[1];
    return tensor_make_2d(tensor_base(buf) + off * es, nrows, ncols, dtype);
}

static inline Tensor tensor_row_chunk(Tensor buf, uint32_t row0, uint32_t nrows)
{
    return tensor_chunk_2d(buf, row0, nrows, 0, buf.storage[1], buf.dtype);
}

static inline Tensor tensor_col_chunk(Tensor buf, uint32_t col0, uint32_t ncols)
{
    return tensor_chunk_2d(buf, 0, buf.storage[0], col0, ncols, buf.dtype);
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

#define MAX_EDGES 2000000
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
    /* Single-thread test: claim succeeds immediately (see test_qwen3_decode_tensormap.c). */
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

#include "qwen3_dynamic_tensormap.h"

int main(void)
{
    ring_buf_init_local();
    aicpu_orchestration_entry(0);

    const uint32_t dump_edges = dep_dump_count_edges();
    printf("tasks=%d recorder_edges=%d dump_edges=%u\n", (int)g_task_id,
           g_edge_n, dump_edges);
    assert(g_edge_n < MAX_EDGES);
    assert(dump_edges == (uint32_t)g_edge_n);

    dep_dump_summary(stdout);
    printf("test_dep_dump_qwen3_edges: OK\n");
    return 0;
}
