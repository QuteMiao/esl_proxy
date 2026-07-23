/*
 * Copyright (c) PyPTO Contributors.
 * This program is free software, you can redistribute it and/or modify it under
 * the terms and conditions of CANN Open Software License Agreement Version 2.0
 * (the "License"). Please refer to the License for details. You may not use
 * this file except in compliance with the License. THIS SOFTWARE IS PROVIDED ON
 * AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS
 * FOR A PARTICULAR PURPOSE. See LICENSE in the root of the software repository
 * for the full text of the License.
 * -----------------------------------------------------------------------------------------------------------
 */

/**
 * tensormap.h — TensorMap core + orchestration glue (tm_in/out/submit).
 *
 * Includes tensormap_core.h and registers IO/predecessors via ring_buf.
 * For map-only use (no tm_*), include tensormap_core.h directly.
 */

#ifndef ESL_PROXY_TENSORMAP_H
#define ESL_PROXY_TENSORMAP_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "ring_buf.h"
#include "tensormap_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TMD_POOL_SIZE
#define TMD_POOL_SIZE 8192u
#endif
#ifndef TMD_NUM_BUCKETS
#define TMD_NUM_BUCKETS 2048u
#endif
#ifndef TMD_TASK_WINDOW
#define TMD_TASK_WINDOW 65536u
#endif

#define TMD_BUF_BYTES                                                 \
    (sizeof(TmHeader) + (uint64_t)TMD_NUM_BUCKETS * sizeof(int32_t) + \
        TM_ENTRY_ALIGN + (uint64_t)TMD_POOL_SIZE * sizeof(TmEntry) +  \
        (uint64_t)TMD_POOL_SIZE * sizeof(int32_t) +                   \
        (uint64_t)TMD_TASK_WINDOW * sizeof(int32_t))

#ifndef TM_PENDING_MAX_IO
#define TM_PENDING_MAX_IO 16u
#endif
#ifndef TM_PENDING_MAX_PRED
#define TM_PENDING_MAX_PRED 64u
#endif

enum {
    TM_PEND_IN = 1u,
    TM_PEND_OUT = 2u,
    TM_PEND_INOUT = (TM_PEND_IN | TM_PEND_OUT)
};

typedef struct {
    const Tensor *t;
    uint8_t kind;
} TmPendingSlot;

typedef struct TmDepsState {
    TmTensorMap map;
    _Alignas(TM_ENTRY_ALIGN) uint8_t buf[TMD_BUF_BYTES];
    TmPendingSlot pend[TM_PENDING_MAX_IO];
    int pend_n;
} TmDepsState;

static TmDepsState g_tm_deps;

typedef struct {
    uint32_t consumer;
    uint32_t preds[TM_PENDING_MAX_PRED];
    int pn;
    bool is_inout;
} TmCollectCtx;

static inline void tm_pending_clear(void) {
    g_tm_deps.pend_n = 0;
}

static inline bool tm_collect_on_match(TmEntry *e, TmOverlap ov, void *ctx) {
    TmCollectCtx *c = (TmCollectCtx *)ctx;
    const uint32_t p = (uint32_t)tm_local_of(e->producer_id);
    if (p != c->consumer) {
        for (int i = 0; i < c->pn; i++) {
            if (c->preds[i] == p) {
                goto after_pred;
            }
        }
        if (c->pn < (int)TM_PENDING_MAX_PRED) {
            c->preds[c->pn++] = p;
        }
    }
after_pred:
    if (c->is_inout && ov == TM_OVERLAP_COVERED) {
        tm_remove(&g_tm_deps.map, e);
    }
    return true;
}

static inline void tm_pending_push(const Tensor *t, uint8_t kind) {
    if (g_tm_deps.pend_n < (int)TM_PENDING_MAX_IO) {
        g_tm_deps.pend[g_tm_deps.pend_n].t = t;
        g_tm_deps.pend[g_tm_deps.pend_n].kind = kind;
        g_tm_deps.pend_n++;
    }
}

static inline void tm_deps_init(void) {
    TmConfig cfg;
    cfg.num_buckets = TMD_NUM_BUCKETS;
    cfg.pool_size = TMD_POOL_SIZE;
    cfg.num_rings = 1;
    cfg.task_window[0] = TMD_TASK_WINDOW;
    for (uint32_t r = 1; r < TM_MAX_RINGS; r++) {
        cfg.task_window[r] = 1;
    }
    tm_init(&g_tm_deps.map, g_tm_deps.buf, &cfg);
    tm_pending_clear();
}

static inline void tm_in_ptr(uint32_t tid, const Tensor *t) {
    add_tensor_addr(tid, t->buffer_addr);
    tm_pending_push(t, TM_PEND_IN);
}

static inline void tm_in_ro_ptr(uint32_t tid, const Tensor *t) {
    add_tensor_addr(tid, t->buffer_addr);
}

static inline void tm_out_ro_ptr(uint32_t tid, const Tensor *t) {
    add_tensor_addr(tid, t->buffer_addr);
}

static inline void tm_inout_ro_ptr(uint32_t tid, const Tensor *t) {
    add_tensor_addr(tid, t->buffer_addr);
}

static inline void tm_out_ptr(uint32_t tid, const Tensor *t) {
    add_tensor_addr(tid, t->buffer_addr);
    tm_pending_push(t, TM_PEND_OUT);
}

static inline void tm_inout_ptr(uint32_t tid, const Tensor *t) {
    add_tensor_addr(tid, t->buffer_addr);
    tm_pending_push(t, TM_PEND_INOUT);
}

static inline void tm_submit_ptr(uint32_t tid) {
    /* STEP 0: advance the alive watermark and cleanup retired entries before
     * the fanin lookup, matching PTO2 submit_task ordering. Doing it first uses
     * a fresher min_uncomplete_task (avoids depending on a just-completed
     * producer) and frees pool slots before this task's inserts. */
    tm_sync_tensormap(
        &g_tm_deps.map, 0,
        (int32_t)atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire),
        tid);

    TmCollectCtx ctx = {.consumer = tid, .pn = 0};
    int i;

    for (i = 0; i < g_tm_deps.pend_n; i++) {
        if (g_tm_deps.pend[i].kind & TM_PEND_IN) {
            ctx.is_inout = (g_tm_deps.pend[i].kind == TM_PEND_INOUT);
            tm_lookup_tensor(&g_tm_deps.map, g_tm_deps.pend[i].t, tm_collect_on_match,
                &ctx);
        }
    }
    if (ctx.pn > 0) {
        add_predecessors(tid, ctx.preds, (uint32_t)ctx.pn, 0);
    }
    for (i = 0; i < g_tm_deps.pend_n; i++) {
        if (g_tm_deps.pend[i].kind & TM_PEND_OUT) {
            tm_insert_tensor(&g_tm_deps.map, g_tm_deps.pend[i].t, tid);
        }
    }
    tm_pending_clear();
}

#define tm_in(tid, t) tm_in_ptr((tid), &(t))
#define tm_in_ro(tid, t) tm_in_ro_ptr((tid), &(t))
#define tm_out(tid, t) tm_out_ptr((tid), &(t))
#define tm_out_ro(tid, t) tm_out_ro_ptr((tid), &(t))
#define tm_inout(tid, t) tm_inout_ptr((tid), &(t))
#define tm_inout_ro(tid, t) tm_inout_ro_ptr((tid), &(t))
#define tm_submit(tid) tm_submit_ptr((tid))

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_TENSORMAP_H */
