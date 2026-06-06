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
 * tensormap.h — minimal, dependency-free producer-lookup map (pure C).
 *
 * Ported verbatim from simpler's src/common/tensormap/tm_tensormap_c.h. Kept as
 * an additive, zero-coupling module: it does NOT depend on esl_proxy's Tensor,
 * ring_buf, or task headers. The manual succeed()/add_dep() dependency path in
 * ring_buf.h is unaffected — this map is an optional, side-channel "find the
 * producer task of an address" tool that callers may layer on top.
 *
 * Producer lookup + sub-region overlap detection + lazy invalidation + pooled
 * entries. It uses only <assert.h>/<stdint.h>/<string.h>/<stdbool.h> and
 * compiles as C (C99+) or C++.
 *
 * Memory: the map never allocates. The caller hands it one raw buffer (sized
 * via tm_bytes_required) at the single dependency point tm_init()/tm_attach().
 * All state — header, hash buckets, entry pool, free list, per-ring task heads
 * — lives inside that buffer, addressed by offsets, with intrusive links stored
 * as pool indices (not pointers). The image is therefore position-independent:
 * build it on the host, memcpy it elsewhere, and tm_attach(base) with no
 * pointer fix-up.
 *
 * Producer identity is an opaque uint64_t whose ring/local encoding is owned by
 * this module (tm_make_id / tm_ring_of / tm_local_of); validity follows a
 * per-ring "last alive" watermark (tm_sync / tm_cleanup_retired). In esl_proxy
 * the natural mapping is producer_id = tm_make_id(0, task_id) and the watermark
 * is g_min_uncomplete_task.
 *
 * C vs. C++ API note: C has no templates or lambdas, so tm_lookup() takes a
 * function pointer plus an opaque user-context pointer instead of a callable.
 */

#ifndef ESL_PROXY_TENSORMAP_H
#define ESL_PROXY_TENSORMAP_H

#ifdef USE_TENSORMAP

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  TM_MAX_DIMS = 2, /* matches esl_proxy 2D Tensor */
  TM_MAX_RINGS = 8 /* task-id ring layers cap */
};

/* Configuration POD. num_buckets and every task_window[r] must be powers of
 * two. */
typedef struct TmConfig {
  uint32_t num_buckets;
  uint32_t pool_size;
  uint32_t num_rings;
  uint32_t task_window[TM_MAX_RINGS];
} TmConfig;

/* Region descriptor for unit tests (2D row-major geometry). */
typedef struct TmRegion {
  uint64_t base_addr;
  uint32_t storage_numel;
  uint16_t elem_size;
  int32_t version;
  uint16_t strides[TM_MAX_DIMS];
  uint16_t shapes[TM_MAX_DIMS];
  uint16_t offsets[TM_MAX_DIMS];
} TmRegion;

typedef enum TmOverlap {
  TM_OVERLAP_NONE = 0,
  TM_OVERLAP_COVERED = 1,
  TM_OVERLAP_OTHER = 2
} TmOverlap;

/* Pool entry. Links are pool indices (-1 = none) so the buffer is relocatable.
 */
typedef struct TmEntry {
  uint64_t base_addr;
  uint64_t producer_id;
  int32_t version;
  uint16_t elem_size;
  uint16_t strides[TM_MAX_DIMS];
  uint16_t shapes[TM_MAX_DIMS];
  uint16_t offsets[TM_MAX_DIMS];
  int32_t next_in_bucket;
  int32_t prev_in_bucket;
  int32_t next_in_task;
  int32_t prev_in_task;
  int32_t bucket_index; /* -1 when unlinked */
} TmEntry;

/* In-buffer header: config echo + cursors + sub-region offsets. */
typedef struct TmHeader {
  TmConfig cfg;
  int32_t next_entry_idx;
  int32_t free_num;
  int32_t last_alive[TM_MAX_RINGS];
  int32_t last_cleanup[TM_MAX_RINGS];
  uint32_t off_buckets;
  uint32_t off_pool;
  uint32_t off_free;
  uint32_t off_task_heads[TM_MAX_RINGS];
} TmHeader;

/* The map handle — just a base pointer into the caller-provided buffer. */
typedef struct TmTensorMap {
  uint8_t *base;
} TmTensorMap;

/* lookup() callback: invoked for each valid overlapping entry. Return true to
 * continue, false to stop early. The callback may call tm_remove() on the
 * current entry; the next link is latched beforehand. */
typedef bool (*TmMatchFn)(TmEntry *entry, TmOverlap status, void *ctx);

/* ---- internal helpers ---------------------------------------------------- */

#define TM_REGION_ALIGN 64u

/* Single source of truth for region placement, shared by tm_bytes_required /
 * tm_init. */
static inline uint64_t tm_layout(const TmConfig *cfg,
                                 TmHeader *out /* may be NULL */) {
  uint64_t cur = sizeof(TmHeader);
  const uint64_t off_buckets = cur;
  cur += (uint64_t)cfg->num_buckets * sizeof(int32_t);
  const uint64_t off_pool = cur;
  cur += (uint64_t)cfg->pool_size * sizeof(TmEntry);
  const uint64_t off_free = cur;
  cur += (uint64_t)cfg->pool_size * sizeof(int32_t);
  uint64_t off_task[TM_MAX_RINGS] = {0};
  for (uint32_t r = 0; r < cfg->num_rings; r++) {
    off_task[r] = cur;
    cur += (uint64_t)cfg->task_window[r] * sizeof(int32_t);
  }
  if (out != NULL) {
    out->off_buckets = off_buckets;
    out->off_pool = off_pool;
    out->off_free = off_free;
    for (uint32_t r = 0; r < cfg->num_rings; r++) {
      out->off_task_heads[r] = off_task[r];
    }
  }
  return cur;
}

/* Producer-id encoding (owned by this module). */
static inline uint64_t tm_make_id(uint32_t ring, uint32_t local) {
  return ((uint64_t)ring << 32) | local;
}
static inline uint32_t tm_ring_of(uint64_t id) { return (uint32_t)(id >> 32); }
static inline uint32_t tm_local_of(uint64_t id) {
  return (uint32_t)(id & 0xFFFFFFFFu);
}

/* Overlap cascade (2D hyper-rectangle / conservative OTHER). */
static inline TmOverlap tm_overlap(const TmRegion *in, const TmEntry *e) {
  if (in->version > e->version)
    return TM_OVERLAP_OTHER;
  if (in->elem_size != e->elem_size)
    return TM_OVERLAP_OTHER;
  for (uint16_t i = 0; i < TM_MAX_DIMS; i++) {
    if (in->strides[i] != e->strides[i])
      return TM_OVERLAP_OTHER;
  }
  bool covered = true;
  for (uint16_t i = 0; i < TM_MAX_DIMS; i++) {
    const uint64_t a0 = in->offsets[i], a1 = a0 + in->shapes[i];
    const uint64_t b0 = e->offsets[i], b1 = b0 + e->shapes[i];
    if (!(a1 > b0 && b1 > a0))
      return TM_OVERLAP_NONE;
    if (!(a0 <= b0 && b1 <= a1))
      covered = false;
  }
  return covered ? TM_OVERLAP_COVERED : TM_OVERLAP_OTHER;
}

/* ---- in-buffer accessors ------------------------------------------------- */

static inline TmHeader *tm_hdr(const TmTensorMap *self) {
  return (TmHeader *)self->base;
}
static inline int32_t *tm_buckets(const TmTensorMap *self) {
  return (int32_t *)(self->base + tm_hdr(self)->off_buckets);
}
static inline TmEntry *tm_pool(const TmTensorMap *self) {
  return (TmEntry *)(self->base + tm_hdr(self)->off_pool);
}
static inline int32_t *tm_free_list(const TmTensorMap *self) {
  return (int32_t *)(self->base + tm_hdr(self)->off_free);
}
static inline int32_t *tm_task_heads(const TmTensorMap *self, uint32_t ring) {
  return (int32_t *)(self->base + tm_hdr(self)->off_task_heads[ring]);
}

static inline uint32_t tm_hash(const TmTensorMap *self, uint64_t key) {
  key *= 0x9E3779B97F4A7C15ULL;
  return (uint32_t)(key >> (64 - __builtin_ctz(tm_hdr(self)->cfg.num_buckets)));
}

static inline bool tm_entry_valid(const TmTensorMap *self, const TmEntry *e) {
  return (int32_t)tm_local_of(e->producer_id) >=
         tm_hdr(self)->last_alive[tm_ring_of(e->producer_id)];
}

static inline int32_t tm_new_entry(TmTensorMap *self) {
  TmHeader *h = tm_hdr(self);
  if (h->free_num > 0)
    return tm_free_list(self)[--h->free_num];
  if (h->next_entry_idx >= (int32_t)h->cfg.pool_size)
    return -1;
  return h->next_entry_idx++;
}

static inline void tm_link_entry(TmTensorMap *self, int32_t idx, uint64_t addr,
                                 uint64_t producer_id) {
  TmEntry *pl = tm_pool(self);
  int32_t *bk = tm_buckets(self);
  TmEntry *e = &pl[idx];
  e->producer_id = producer_id;

  const uint32_t b = tm_hash(self, addr);
  e->bucket_index = (int32_t)b;
  e->prev_in_bucket = -1;
  e->next_in_bucket = bk[b];
  if (bk[b] != -1)
    pl[bk[b]].prev_in_bucket = idx;
  bk[b] = idx;

  const uint32_t ring = tm_ring_of(producer_id);
  const uint32_t slot =
      tm_local_of(producer_id) & (tm_hdr(self)->cfg.task_window[ring] - 1);
  int32_t *th = tm_task_heads(self, ring);
  e->prev_in_task = -1;
  e->next_in_task = th[slot];
  if (th[slot] != -1)
    pl[th[slot]].prev_in_task = idx;
  th[slot] = idx;
}

static inline void tm_remove_from_task(TmTensorMap *self, int32_t idx) {
  TmEntry *pl = tm_pool(self);
  TmEntry *e = &pl[idx];
  if (e->prev_in_task == -1) {
    const uint32_t ring = tm_ring_of(e->producer_id);
    const uint32_t slot =
        tm_local_of(e->producer_id) & (tm_hdr(self)->cfg.task_window[ring] - 1);
    tm_task_heads(self, ring)[slot] = e->next_in_task;
  } else {
    pl[e->prev_in_task].next_in_task = e->next_in_task;
  }
  if (e->next_in_task != -1)
    pl[e->next_in_task].prev_in_task = e->prev_in_task;
  e->next_in_task = e->prev_in_task = -1;
}

static inline void tm_free_entry(TmTensorMap *self, int32_t idx) {
  TmEntry *pl = tm_pool(self);
  int32_t *bk = tm_buckets(self);
  TmEntry *e = &pl[idx];
  if (e->prev_in_bucket == -1) {
    bk[e->bucket_index] = e->next_in_bucket;
  } else {
    pl[e->prev_in_bucket].next_in_bucket = e->next_in_bucket;
  }
  if (e->next_in_bucket != -1)
    pl[e->next_in_bucket].prev_in_bucket = e->prev_in_bucket;

  tm_free_list(self)[tm_hdr(self)->free_num++] = idx;
  e->bucket_index = -1;
  e->next_in_bucket = e->prev_in_bucket = -1;
  e->next_in_task = e->prev_in_task = -1;
}

/* ---- public API ---------------------------------------------------------- */

/* Bytes the caller must provide to tm_init() for this config. */
static inline uint64_t tm_bytes_required(const TmConfig *cfg) {
  return tm_layout(cfg, NULL);
}

/* Single memory dependency point: lay out and zero the map inside `base`.
 * `base` must be at least tm_bytes_required(cfg) bytes and 64-byte aligned. */
static inline void tm_init(TmTensorMap *self, void *base, const TmConfig *cfg) {
  self->base = (uint8_t *)base;
  TmHeader *h = tm_hdr(self);
  h->cfg = *cfg;
  h->next_entry_idx = 0;
  h->free_num = 0;
  for (uint32_t r = 0; r < TM_MAX_RINGS; r++) {
    h->last_alive[r] = 0;
    h->last_cleanup[r] = 0;
    h->off_task_heads[r] = 0;
  }
  tm_layout(cfg, h);

  int32_t *bk = tm_buckets(self);
  for (uint32_t i = 0; i < cfg->num_buckets; i++)
    bk[i] = -1;
  TmEntry *pl = tm_pool(self);
  for (uint32_t i = 0; i < cfg->pool_size; i++) {
    pl[i].base_addr = 0;
    pl[i].producer_id = 0;
    pl[i].version = 0;
    pl[i].elem_size = 0;
    pl[i].bucket_index = -1;
    pl[i].next_in_bucket = pl[i].prev_in_bucket = -1;
    pl[i].next_in_task = pl[i].prev_in_task = -1;
  }
  for (uint32_t r = 0; r < cfg->num_rings; r++) {
    int32_t *th = tm_task_heads(self, r);
    for (uint32_t i = 0; i < cfg->task_window[r]; i++)
      th[i] = -1;
  }
}

/* Bind to a buffer that already holds an initialized image (no fix-up needed).
 */
static inline void tm_attach(TmTensorMap *self, void *base) {
  self->base = (uint8_t *)base;
}

/* Register a region produced by `producer_id`. */
static inline void tm_insert(TmTensorMap *self, const TmRegion *r,
                             uint64_t producer_id) {
  const int32_t idx = tm_new_entry(self);
  if (idx < 0)
    return;
  TmEntry *e = &tm_pool(self)[idx];
  e->base_addr = r->base_addr;
  e->version = r->version;
  e->elem_size = r->elem_size;
  for (uint16_t i = 0; i < TM_MAX_DIMS; i++) {
    e->strides[i] = r->strides[i];
    e->shapes[i] = r->shapes[i];
    e->offsets[i] = r->offsets[i];
  }
  tm_link_entry(self, idx, r->base_addr, producer_id);
}

/* Invoke on_match(entry, overlap, ctx) for each valid overlapping entry.
 * Return true to continue, false to stop early. The callback may call
 * tm_remove() on the current entry; the next link is latched beforehand. */
static inline void tm_lookup(TmTensorMap *self, const TmRegion *r,
                             TmMatchFn on_match, void *ctx) {
  const uint32_t b = tm_hash(self, r->base_addr);
  int32_t cur = tm_buckets(self)[b];
  TmEntry *pl = tm_pool(self);
  while (cur != -1) {
    const int32_t next = pl[cur].next_in_bucket;
    TmEntry *e = &pl[cur];
    if (tm_entry_valid(self, e) && e->base_addr == r->base_addr) {
      const TmOverlap st = tm_overlap(r, e);
      if (st != TM_OVERLAP_NONE) {
        if (!on_match(e, st, ctx))
          return;
      }
    }
    cur = next;
  }
}

/* Unlink one entry from both chains and return it to the pool. */
static inline void tm_remove(TmTensorMap *self, TmEntry *e) {
  const int32_t idx = (int32_t)(e - tm_pool(self));
  tm_remove_from_task(self, idx);
  tm_free_entry(self, idx);
}

/* Advance the per-ring validity watermark. */
static inline void tm_sync(TmTensorMap *self, uint32_t ring,
                           int32_t last_alive) {
  tm_hdr(self)->last_alive[ring] = last_alive;
}

/* Reclaim entries of tasks in [old_alive, new_alive) on `ring`. */
static inline void tm_cleanup_retired(TmTensorMap *self, uint32_t ring,
                                      int32_t old_alive, int32_t new_alive) {
  const uint32_t mask = tm_hdr(self)->cfg.task_window[ring] - 1;
  int32_t *th = tm_task_heads(self, ring);
  TmEntry *pl = tm_pool(self);
  for (int32_t local = old_alive; local < new_alive; local++) {
    const uint32_t slot = (uint32_t)local & mask;
    int32_t cur = th[slot];
    while (cur != -1) {
      const int32_t next = pl[cur].next_in_task;
      tm_free_entry(self, cur);
      cur = next;
    }
    th[slot] = -1;
  }
  tm_hdr(self)->last_cleanup[ring] = new_alive;
}

/* Convenience for the submit hot path: advance the watermark and reclaim any
 * entries that just retired. Correctness-first (cleans up to the watermark on
 * every advance); no periodic-gating optimization. */
static inline void tm_sync_tensormap(TmTensorMap *self, uint32_t ring,
                                     int32_t last_alive) {
  tm_sync(self, ring, last_alive);
  const int32_t old = tm_hdr(self)->last_cleanup[ring];
  if (last_alive > old)
    tm_cleanup_retired(self, ring, old, last_alive);
}

/* Number of currently-valid (non-retired) entries — debug/testing helper. */
static inline int32_t tm_valid_count(const TmTensorMap *self) {
  const TmHeader *h = tm_hdr(self);
  const TmEntry *pl = tm_pool(self);
  int32_t n = 0;
  for (int32_t i = 0; i < h->next_entry_idx; i++) {
    if (pl[i].bucket_index != -1 && tm_entry_valid(self, &pl[i]))
      n++;
  }
  return n;
}

/* ===========================================================================
 * High-level tensormap dependency layer: tm_deps_init / tm_in / tm_out /
 * tm_inout / tm_submit.
 *
 * Two modes (pick one via compile flags):
 *   - Default (struct Tensor): sub-range geometry via tensor_view(dim);
 * immediate lookup on tm_in / insert on tm_out.  Requires USE_TENSORMAP.
 *   - TENSORMAP_WHOLE_BUFFER: whole-buffer address granularity (uint64_t
 *     Tensor); deferred batch lookup/insert at tm_submit.  Merged from the
 *     former cases/custom_tensormap.h.
 *
 * Include ring_buf.h before tensormap.h in case files.
 * ========================================================================== */
#ifdef DAG_RING_BUF_H

#ifdef TENSORMAP_WHOLE_BUFFER

#include <assert.h>

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

static inline TmRegion tm_deps_region(uint64_t addr) {
  TmRegion r;
  memset(&r, 0, sizeof r);
  r.base_addr = addr;
  r.elem_size = 1;
  r.shapes[0] = 1;
  r.shapes[1] = 1;
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
  if (g_tm_in_n < TM_DEPS_MAX_IO)
    g_tm_in[g_tm_in_n++] = (uint64_t)t;
}

static inline void tm_out(uint16_t tid, Tensor t) {
  add_output(tid, t);
  if (g_tm_out_n < TM_DEPS_MAX_IO)
    g_tm_out[g_tm_out_n++] = (uint64_t)t;
}

static inline void tm_inout(uint16_t tid, Tensor t) {
  add_inout(tid, t);
  if (g_tm_in_n < TM_DEPS_MAX_IO)
    g_tm_in[g_tm_in_n++] = (uint64_t)t;
  if (g_tm_out_n < TM_DEPS_MAX_IO)
    g_tm_out[g_tm_out_n++] = (uint64_t)t;
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
  if (p == c->self)
    return true;
  for (int i = 0; i < *c->n; i++) {
    if (c->preds[i] == p)
      return true;
  }
  if (*c->n < TM_DEPS_MAX_PRED)
    c->preds[(*c->n)++] = p;
  return true;
}

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
  for (i = 0; i < pn; i++)
    succeed(tid, preds[i]);
  for (i = 0; i < g_tm_out_n; i++) {
    TmRegion r = tm_deps_region(g_tm_out[i]);
    tm_insert(&g_tm_deps, &r, tm_make_id(0, tid));
  }
  submit(tid);
  g_tm_in_n = 0;
  g_tm_out_n = 0;
}

#else /* struct Tensor — geometric dependency layer */

#ifndef TMD_POOL_SIZE
#define TMD_POOL_SIZE 8192u /* >= peak live producer regions */
#endif
#ifndef TMD_NUM_BUCKETS
#define TMD_NUM_BUCKETS 2048u /* power of two */
#endif
#ifndef TMD_TASK_WINDOW
#define TMD_TASK_WINDOW 65536u /* power of two; covers every uint16 task id */
#endif

#define TMD_BUF_BYTES                                                          \
  (sizeof(TmHeader) + (uint64_t)TMD_NUM_BUCKETS * sizeof(int32_t) +            \
   (uint64_t)TMD_POOL_SIZE * sizeof(TmEntry) +                                 \
   (uint64_t)TMD_POOL_SIZE * sizeof(int32_t) +                                 \
   (uint64_t)TMD_TASK_WINDOW * sizeof(int32_t))

static TmTensorMap g_tm_map;
static _Alignas(TM_REGION_ALIGN) uint8_t g_tm_buf[TMD_BUF_BYTES];

#ifndef TM_PENDING_MAX_IO
#define TM_PENDING_MAX_IO 16u
#endif
#ifndef TM_PENDING_MAX_PRED
#define TM_PENDING_MAX_PRED 32u
#endif

enum {
  TM_PEND_IN = 1u,
  TM_PEND_OUT = 2u,
  TM_PEND_INOUT = (TM_PEND_IN | TM_PEND_OUT)
};

typedef struct {
  const Tensor *t;
  uint8_t kind; /* TM_PEND_IN | TM_PEND_OUT | TM_PEND_INOUT */
} TmPendingSlot;

static TmPendingSlot g_tm_pend[TM_PENDING_MAX_IO];
static int g_tm_pend_n;

static inline void tm_pending_clear(void) { g_tm_pend_n = 0; }

/* Overlap using Tensor fields directly (no TmRegion / memset on hot path). */
static inline TmOverlap tm_overlap_tensor(const Tensor *in, const TmEntry *e) {
  if ((uint16_t)in->dtype != e->elem_size)
    return TM_OVERLAP_OTHER;
  for (uint16_t i = 0; i < TM_MAX_DIMS; i++) {
    if (in->strides[i] != (uint32_t)e->strides[i])
      return TM_OVERLAP_OTHER;
  }
  bool covered = true;
  for (uint16_t i = 0; i < TM_MAX_DIMS; i++) {
    const uint64_t a0 = in->offsets[i], a1 = a0 + in->shapes[i];
    const uint64_t b0 = e->offsets[i], b1 = b0 + e->shapes[i];
    if (!(a1 > b0 && b1 > a0))
      return TM_OVERLAP_NONE;
    if (!(a0 <= b0 && b1 <= a1))
      covered = false;
  }
  return covered ? TM_OVERLAP_COVERED : TM_OVERLAP_OTHER;
}

typedef struct {
  uint16_t consumer;
  uint16_t preds[TM_PENDING_MAX_PRED];
  int pn;
  bool is_inout;
} TmCollectCtx;

static inline bool tm_collect_on_match(TmEntry *e, TmOverlap st, void *ctx) {
  TmCollectCtx *c = (TmCollectCtx *)ctx;
  const uint16_t p = (uint16_t)tm_local_of(e->producer_id);
  if (p != c->consumer) {
    for (int i = 0; i < c->pn; i++)
      if (c->preds[i] == p)
        goto after_pred;
    if (c->pn < (int)TM_PENDING_MAX_PRED)
      c->preds[c->pn++] = p;
  }
after_pred:
  if (c->is_inout && st == TM_OVERLAP_COVERED)
    tm_remove(&g_tm_map, e);
  return true;
}

static inline void tm_lookup_tensor(TmTensorMap *self, const Tensor *t,
                                    TmMatchFn on_match, void *ctx) {
  const uint32_t b = tm_hash(self, t->base);
  int32_t cur = tm_buckets(self)[b];
  TmEntry *pl = tm_pool(self);

  while (cur != -1) {
    const int32_t next = pl[cur].next_in_bucket;
    TmEntry *e = &pl[cur];
    if (tm_entry_valid(self, e) && e->base_addr == t->base) {
      const TmOverlap st = tm_overlap_tensor(t, e);
      if (st != TM_OVERLAP_NONE && !on_match(e, st, ctx))
        return;
    }
    cur = next;
  }
}

static inline void tm_insert_tensor(TmTensorMap *self, const Tensor *t,
                                    uint16_t tid) {
  const int32_t idx = tm_new_entry(self);
  if (idx < 0)
    return;
  TmEntry *e = &tm_pool(self)[idx];
  e->base_addr = t->base;
  e->version = 0;
  e->elem_size = (uint16_t)t->dtype;
  e->strides[0] = (uint16_t)t->strides[0];
  e->strides[1] = (uint16_t)t->strides[1];
  e->shapes[0] = (uint16_t)t->shapes[0];
  e->shapes[1] = (uint16_t)t->shapes[1];
  e->offsets[0] = (uint16_t)t->offsets[0];
  e->offsets[1] = (uint16_t)t->offsets[1];
  tm_link_entry(self, idx, t->base, tm_make_id(0, tid));
}

static inline void tm_pending_push(const Tensor *t, uint8_t kind) {
  if (g_tm_pend_n < (int)TM_PENDING_MAX_IO) {
    g_tm_pend[g_tm_pend_n].t = t;
    g_tm_pend[g_tm_pend_n].kind = kind;
    g_tm_pend_n++;
  }
}

static inline void tm_deps_init(void) {
  TmConfig cfg;
  cfg.num_buckets = TMD_NUM_BUCKETS;
  cfg.pool_size = TMD_POOL_SIZE;
  cfg.num_rings = 1;
  cfg.task_window[0] = TMD_TASK_WINDOW;
  for (uint32_t r = 1; r < TM_MAX_RINGS; r++)
    cfg.task_window[r] = 1;
  tm_init(&g_tm_map, g_tm_buf, &cfg);
  tm_pending_clear();
}

static inline void tm_in_ptr(uint16_t tid, const Tensor *t) {
  add_input_ptr(tid, t);
#if !NO_DEPS
  tm_pending_push(t, TM_PEND_IN);
#endif
}

static inline void tm_in_ro_ptr(uint16_t tid, const Tensor *t) {
  add_input_ptr(tid, t);
}

static inline void tm_out_ptr(uint16_t tid, const Tensor *t) {
  add_output_ptr(tid, t);
#if !NO_DEPS
  tm_pending_push(t, TM_PEND_OUT);
#endif
}

static inline void tm_inout_ptr(uint16_t tid, const Tensor *t) {
  add_inout_ptr(tid, t);
#if !NO_DEPS
  tm_pending_push(t, TM_PEND_INOUT);
#endif
}

#define tm_in(tid, t) tm_in_ptr((tid), &(t))
#define tm_in_ro(tid, t) tm_in_ro_ptr((tid), &(t))
#define tm_out(tid, t) tm_out_ptr((tid), &(t))
#define tm_inout(tid, t) tm_inout_ptr((tid), &(t))

static inline void tm_submit(uint16_t tid) {
#if !NO_DEPS
  TmCollectCtx ctx = {.consumer = tid, .pn = 0};
  int i;

  for (i = 0; i < g_tm_pend_n; i++) {
    if (g_tm_pend[i].kind & TM_PEND_IN) {
      ctx.is_inout = (g_tm_pend[i].kind == TM_PEND_INOUT);
      tm_lookup_tensor(&g_tm_map, g_tm_pend[i].t, tm_collect_on_match, &ctx);
    }
  }
  for (i = 0; i < ctx.pn; i++)
    succeed(tid, ctx.preds[i]);
  for (i = 0; i < g_tm_pend_n; i++) {
    if (g_tm_pend[i].kind & TM_PEND_OUT)
      tm_insert_tensor(&g_tm_map, g_tm_pend[i].t, tid);
  }
  tm_pending_clear();
#endif
  submit(tid);
#if !NO_DEPS
  tm_sync_tensormap(&g_tm_map, 0, (int32_t)g_min_uncomplete_task);
#endif
}

#endif /* TENSORMAP_WHOLE_BUFFER */

#endif /* DAG_RING_BUF_H */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* USE_TENSORMAP */

#endif /* ESL_PROXY_TENSORMAP_H */
