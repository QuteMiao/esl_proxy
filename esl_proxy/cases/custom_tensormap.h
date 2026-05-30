/*
 * tensormap_deps.h - additive auto-dependency glue for esl_proxy cases.
 *
 * Replaces manual succeed()/batch_succeed() bookkeeping: each task's producers
 * are discovered from its input tensor ADDRESSES via the ported tensormap
 * (include/tensormap.h), and the resulting edges are wired through esl_proxy's
 * own succeed(). Whole-buffer granularity (Tensor is a bare uint64_t address).
 *
 * Caller contract — these must be in scope BEFORE including this header
 * (provided by ring_buf.h in production, or by stubs in a test harness):
 *   - typedef/macro  Tensor                     (uint64_t base address)
 *   - void add_input/add_output/add_inout(uint16_t task_id, Tensor t)
 *   - bool succeed(uint16_t consumer, uint16_t producer)
 *   - void submit(uint16_t task_id)
 *   - uint16_t g_min_uncomplete_task            (retire watermark)
 *
 * Usage in a case: call tm_deps_init() once, use tm_in/tm_out/tm_inout in place
 * of add_input/add_output/add_inout, and end every task with tm_submit(tid)
 * instead of succeed(...)+submit(tid).
 */

#ifndef ESL_PROXY_TENSORMAP_DEPS_H
#define ESL_PROXY_TENSORMAP_DEPS_H

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "tensormap.h"

#ifndef TM_DEPS_POOL_SIZE
#define TM_DEPS_POOL_SIZE 8192u
#endif
#define TM_DEPS_NUM_BUCKETS 4096u
#define TM_DEPS_TASK_WINDOW 4096u
#define TM_DEPS_MAX_IO 64
#define TM_DEPS_MAX_PRED 1024

static TmTensorMap g_tm_deps;
static _Alignas(64) uint8_t g_tm_deps_buf[2u * 1024u * 1024u];
static uint64_t g_tm_in[TM_DEPS_MAX_IO];
static uint64_t g_tm_out[TM_DEPS_MAX_IO];
static int g_tm_in_n;
static int g_tm_out_n;

/* Degenerate whole-buffer region: any producer sharing the base address overlaps
 * (ndims==0 -> tm_overlap returns OTHER, i.e. a valid dependency edge). */
static inline TmRegion tm_deps_region(uint64_t addr) {
    TmRegion r;
    memset(&r, 0, sizeof r);
    r.base_addr = addr;
    r.extent_elem = 1;
    r.ndims = 0;
    return r;
}

static inline void tm_deps_init(void) {
    TmConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.num_buckets = TM_DEPS_NUM_BUCKETS;
    cfg.pool_size = TM_DEPS_POOL_SIZE;
    cfg.num_rings = 1;
    cfg.task_window[0] = TM_DEPS_TASK_WINDOW;
    assert(tm_bytes_required(&cfg) <= sizeof(g_tm_deps_buf));
    tm_init(&g_tm_deps, g_tm_deps_buf, &cfg);
    g_tm_in_n = 0;
    g_tm_out_n = 0;
}

static inline void tm_in(uint16_t tid, Tensor t) {
    add_input(tid, t);
    if (g_tm_in_n < TM_DEPS_MAX_IO) g_tm_in[g_tm_in_n++] = (uint64_t)t;
}

static inline void tm_out(uint16_t tid, Tensor t) {
    add_output(tid, t);
    if (g_tm_out_n < TM_DEPS_MAX_IO) g_tm_out[g_tm_out_n++] = (uint64_t)t;
}

static inline void tm_inout(uint16_t tid, Tensor t) {
    add_inout(tid, t);
    if (g_tm_in_n < TM_DEPS_MAX_IO) g_tm_in[g_tm_in_n++] = (uint64_t)t;
    if (g_tm_out_n < TM_DEPS_MAX_IO) g_tm_out[g_tm_out_n++] = (uint64_t)t;
}

typedef struct {
    uint16_t *preds;
    int *n;
    uint16_t self;
} TmDepsCollect;

static inline bool tm_deps_collect(TmEntry *e, TmOverlap st, void *ctx) {
    TmDepsCollect *c = (TmDepsCollect *)ctx;
    uint16_t p = (uint16_t)tm_local_of(e->producer_id);
    (void)st;
    if (p == c->self) return true;
    for (int i = 0; i < *c->n; i++) {
        if (c->preds[i] == p) return true; /* dedup */
    }
    if (*c->n < TM_DEPS_MAX_PRED) c->preds[(*c->n)++] = p;
    return true;
}

/* Resolve inputs -> succeed() on each unique producer; register outputs; submit.
 * Lookup runs before insert so an inout task never matches itself. */
static inline void tm_submit(uint16_t tid) {
    uint16_t preds[TM_DEPS_MAX_PRED];
    int pn = 0;
    TmDepsCollect ctx;
    int i;

    tm_sync(&g_tm_deps, 0, (int32_t)g_min_uncomplete_task);

    ctx.preds = preds;
    ctx.n = &pn;
    ctx.self = tid;
    for (i = 0; i < g_tm_in_n; i++) {
        TmRegion r = tm_deps_region(g_tm_in[i]);
        tm_lookup(&g_tm_deps, &r, tm_deps_collect, &ctx);
    }
    for (i = 0; i < pn; i++) {
        succeed(tid, preds[i]);
    }
    for (i = 0; i < g_tm_out_n; i++) {
        TmRegion r = tm_deps_region(g_tm_out[i]);
        tm_insert(&g_tm_deps, &r, tm_make_id(0, tid));
    }
    submit(tid);
    g_tm_in_n = 0;
    g_tm_out_n = 0;
}

#endif /* ESL_PROXY_TENSORMAP_DEPS_H */
