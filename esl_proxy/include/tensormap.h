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
 * tensormap.h — PTO2-aligned producer-lookup map (pure C).
 *
 * Aligned with simpler PTO2TensorMap:
 *   - 128B cache-line TmEntry (relocatable pool indices, not pointers)
 *   - 2D overlap: L1 element-range fast reject + L2 row-major check + L3 OTHER
 *   - Periodic cleanup (TM_CLEANUP_INTERVAL) on tm_sync_tensormap
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
  TM_MAX_DIMS = 2,
  TM_MAX_RINGS = 8
};

#ifndef TM_CLEANUP_INTERVAL
#define TM_CLEANUP_INTERVAL 64
#endif

#define TM_REGION_ALIGN 64u
#define TM_ENTRY_ALIGN 128u

typedef struct TmConfig {
  uint32_t num_buckets;
  uint32_t pool_size;
  uint32_t num_rings;
  uint32_t task_window[TM_MAX_RINGS];
} TmConfig;

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

/* Lookup / overlap probe (2D). */
typedef struct TmProbe {
  uint64_t base_addr;
  uint64_t start_offset;
  uint64_t extent_elem;
  int32_t version;
  uint32_t ndims;
  uint16_t elem_size;
  bool is_contiguous;
  uint32_t strides[TM_MAX_DIMS];
  uint32_t shapes[TM_MAX_DIMS];
  uint64_t storage_numel;
} TmProbe;

/**
 * TmEntry — 128B (2 cache lines), mirrors PTO2TensorMapEntry layout semantics.
 * Hash-bucket and per-task links are pool indices (-1 = none) for relocatability.
 */
typedef struct {
  /* === cache line 1 (64B) === */
  uint64_t base_addr;
  int32_t next_in_bucket;
  int32_t _pad_nb;
  uint64_t producer_id;
  uint64_t start_offset;
  int32_t version;
  uint32_t ndims;
  uint16_t elem_size;
  uint8_t manual_dep;
  uint8_t is_contiguous;
  uint32_t shapes[TM_MAX_DIMS];
  uint32_t _line1_pad[3];

  /* === cache line 2 (64B) === */
  int32_t prev_in_bucket;
  int32_t next_in_task;
  int32_t prev_in_task;
  int32_t bucket_index;
  uint64_t storage_numel;
  uint64_t extent_elem_cache;
  uint32_t strides[TM_MAX_DIMS];
  uint32_t _line2_pad[6];
} TmEntry;

_Static_assert(sizeof(TmEntry) == 128, "TmEntry must be 128 bytes");

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

typedef struct TmTensorMap {
  uint8_t *base;
} TmTensorMap;

typedef bool (*TmMatchFn)(TmEntry *entry, TmOverlap status, void *ctx);

/* ---- layout helpers ------------------------------------------------------ */

static inline uint64_t tm_align_up(uint64_t v, uint64_t align) {
  return (v + align - 1u) & ~(align - 1u);
}

static inline uint64_t tm_layout(const TmConfig *cfg, TmHeader *out) {
  uint64_t cur = sizeof(TmHeader);
  const uint64_t off_buckets = cur;
  cur += (uint64_t)cfg->num_buckets * sizeof(int32_t);
  cur = tm_align_up(cur, TM_ENTRY_ALIGN);
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
    out->off_buckets = (uint32_t)off_buckets;
    out->off_pool = (uint32_t)off_pool;
    out->off_free = (uint32_t)off_free;
    for (uint32_t r = 0; r < cfg->num_rings; r++) {
      out->off_task_heads[r] = (uint32_t)off_task[r];
    }
  }
  return cur;
}

static inline uint64_t tm_make_id(uint32_t ring, uint32_t local) {
  return ((uint64_t)ring << 32) | local;
}
static inline uint32_t tm_ring_of(uint64_t id) { return (uint32_t)(id >> 32); }
static inline uint32_t tm_local_of(uint64_t id) {
  return (uint32_t)(id & 0xFFFFFFFFu);
}

static inline uint64_t tm_entry_extent_elem(const TmEntry *e) {
  if (e->is_contiguous) {
    uint64_t n = 1;
    for (uint32_t i = 0; i < e->ndims; i++) {
      n *= e->shapes[i];
    }
    return n;
  }
  return e->extent_elem_cache;
}

static inline void tm_fill_strides_row_major(uint32_t ndims, const uint32_t shapes[],
                                             uint32_t strides_out[]) {
  if (ndims == 0) {
    return;
  }
  uint32_t s = 1;
  for (int32_t i = (int32_t)ndims - 1; i >= 0; i--) {
    strides_out[i] = s;
    s *= shapes[i];
  }
}

static inline void tm_probe_from_region(const TmRegion *r, TmProbe *p) {
  memset(p, 0, sizeof(*p));
  p->base_addr = r->base_addr;
  p->version = r->version;
  p->ndims = TM_MAX_DIMS;
  p->elem_size = r->elem_size;
  p->is_contiguous = true;
  p->storage_numel = r->storage_numel;
  for (uint16_t i = 0; i < TM_MAX_DIMS; i++) {
    p->strides[i] = r->strides[i];
    p->shapes[i] = r->shapes[i];
  }
  p->start_offset =
      (uint64_t)r->offsets[0] * r->strides[0] + (uint64_t)r->offsets[1];
  p->extent_elem = (uint64_t)r->shapes[0] * r->shapes[1];
}

static inline void tm_copy_region_to_entry(const TmRegion *r, TmEntry *e) {
  TmProbe probe;
  tm_probe_from_region(r, &probe);
  e->base_addr = probe.base_addr;
  e->version = probe.version;
  e->ndims = probe.ndims;
  e->elem_size = probe.elem_size;
  e->manual_dep = 0;
  e->is_contiguous = 1;
  e->start_offset = probe.start_offset;
  e->storage_numel = probe.storage_numel;
  e->extent_elem_cache = probe.extent_elem;
  for (uint16_t i = 0; i < TM_MAX_DIMS; i++) {
    e->shapes[i] = probe.shapes[i];
    e->strides[i] = probe.strides[i];
  }
}

/**
 * PTO2-style overlap (2D): L1 element-range, L2 row-major per-dim, L3 OTHER.
 */
static inline TmOverlap tm_check_overlap(const TmProbe *in, const TmEntry *e) {
  if (in->version > e->version) {
    return TM_OVERLAP_OTHER;
  }

  const uint64_t in_begin = in->start_offset;
  const uint64_t in_end = in->start_offset + in->extent_elem;
  const uint64_t ent_begin = e->start_offset;
  const uint64_t ent_end = e->start_offset + tm_entry_extent_elem(e);
  if (!(in_end > ent_begin && ent_end > in_begin)) {
    return TM_OVERLAP_NONE;
  }

  if (in->elem_size != e->elem_size || in->ndims != e->ndims || in->ndims == 0) {
    return TM_OVERLAP_OTHER;
  }
  for (uint32_t i = 0; i < in->ndims; i++) {
    if (in->strides[i] != e->strides[i]) {
      return TM_OVERLAP_OTHER;
    }
  }
  if (e->strides[in->ndims - 1u] != 1u) {
    return TM_OVERLAP_OTHER;
  }
  for (uint32_t i = 1; i < in->ndims; i++) {
    if (e->strides[i - 1u] % e->strides[i] != 0u) {
      return TM_OVERLAP_OTHER;
    }
  }

  uint32_t ref_shapes[TM_MAX_DIMS] = {0};
  for (uint32_t i = 1; i < in->ndims; i++) {
    ref_shapes[i] = e->strides[i - 1u] / e->strides[i];
  }
  const uint32_t stride0 = e->strides[0];
  if (stride0 == 0u || e->storage_numel % stride0 != 0u) {
    return TM_OVERLAP_OTHER;
  }
  ref_shapes[0] = (uint32_t)(e->storage_numel / stride0);

  uint32_t in_offsets[TM_MAX_DIMS] = {0};
  uint32_t ent_offsets[TM_MAX_DIMS] = {0};
  uint64_t in_remain = in->start_offset;
  uint64_t ent_remain = e->start_offset;
  for (uint32_t i = 0; i < in->ndims; i++) {
    const uint32_t s = e->strides[i];
    in_offsets[i] = (uint32_t)(in_remain / s);
    ent_offsets[i] = (uint32_t)(ent_remain / s);
    in_remain %= s;
    ent_remain %= s;
  }
  if (in_remain != 0u || ent_remain != 0u) {
    return TM_OVERLAP_OTHER;
  }

  for (uint32_t i = 0; i < in->ndims; i++) {
    if ((uint64_t)in_offsets[i] + in->shapes[i] > ref_shapes[i]) {
      return TM_OVERLAP_OTHER;
    }
    if ((uint64_t)ent_offsets[i] + e->shapes[i] > ref_shapes[i]) {
      return TM_OVERLAP_OTHER;
    }
  }

  bool input_contains_entry = true;
  for (uint32_t i = 0; i < in->ndims; i++) {
    const uint64_t a0 = in_offsets[i];
    const uint64_t a1 = a0 + in->shapes[i];
    const uint64_t b0 = ent_offsets[i];
    const uint64_t b1 = b0 + e->shapes[i];
    if (!(a1 > b0 && b1 > a0)) {
      return TM_OVERLAP_NONE;
    }
    if (!(a0 <= b0 && b1 <= a1)) {
      input_contains_entry = false;
    }
  }
  return input_contains_entry ? TM_OVERLAP_COVERED : TM_OVERLAP_OTHER;
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
  if (h->free_num > 0) {
    return tm_free_list(self)[--h->free_num];
  }
  if (h->next_entry_idx >= (int32_t)h->cfg.pool_size) {
    return -1;
  }
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
  if (bk[b] != -1) {
    pl[bk[b]].prev_in_bucket = idx;
  }
  bk[b] = idx;

  const uint32_t ring = tm_ring_of(producer_id);
  const uint32_t slot =
      tm_local_of(producer_id) & (tm_hdr(self)->cfg.task_window[ring] - 1u);
  int32_t *th = tm_task_heads(self, ring);
  e->prev_in_task = -1;
  e->next_in_task = th[slot];
  if (th[slot] != -1) {
    pl[th[slot]].prev_in_task = idx;
  }
  th[slot] = idx;
}

static inline void tm_remove_from_task(TmTensorMap *self, int32_t idx) {
  TmEntry *pl = tm_pool(self);
  TmEntry *e = &pl[idx];
  if (e->prev_in_task == -1) {
    const uint32_t ring = tm_ring_of(e->producer_id);
    const uint32_t slot =
        tm_local_of(e->producer_id) & (tm_hdr(self)->cfg.task_window[ring] - 1u);
    tm_task_heads(self, ring)[slot] = e->next_in_task;
  } else {
    pl[e->prev_in_task].next_in_task = e->next_in_task;
  }
  if (e->next_in_task != -1) {
    pl[e->next_in_task].prev_in_task = e->prev_in_task;
  }
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
  if (e->next_in_bucket != -1) {
    pl[e->next_in_bucket].prev_in_bucket = e->prev_in_bucket;
  }

  tm_free_list(self)[tm_hdr(self)->free_num++] = idx;
  e->bucket_index = -1;
  e->next_in_bucket = e->prev_in_bucket = -1;
  e->next_in_task = e->prev_in_task = -1;
}

/* ---- public API ---------------------------------------------------------- */

static inline uint64_t tm_bytes_required(const TmConfig *cfg) {
  return tm_layout(cfg, NULL);
}

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
  for (uint32_t i = 0; i < cfg->num_buckets; i++) {
    bk[i] = -1;
  }
  TmEntry *pl = tm_pool(self);
  for (uint32_t i = 0; i < cfg->pool_size; i++) {
    memset(&pl[i], 0, sizeof(TmEntry));
    pl[i].bucket_index = -1;
    pl[i].next_in_bucket = pl[i].prev_in_bucket = -1;
    pl[i].next_in_task = pl[i].prev_in_task = -1;
  }
  for (uint32_t r = 0; r < cfg->num_rings; r++) {
    int32_t *th = tm_task_heads(self, r);
    for (uint32_t i = 0; i < cfg->task_window[r]; i++) {
      th[i] = -1;
    }
  }
}

static inline void tm_attach(TmTensorMap *self, void *base) {
  self->base = (uint8_t *)base;
}

static inline void tm_insert(TmTensorMap *self, const TmRegion *r,
                             uint64_t producer_id) {
  const int32_t idx = tm_new_entry(self);
  if (idx < 0) {
    return;
  }
  TmEntry *e = &tm_pool(self)[idx];
  tm_copy_region_to_entry(r, e);
  tm_link_entry(self, idx, r->base_addr, producer_id);
}

static inline void tm_lookup(TmTensorMap *self, const TmRegion *r,
                             TmMatchFn on_match, void *ctx) {
  TmProbe probe;
  tm_probe_from_region(r, &probe);
  const uint32_t b = tm_hash(self, r->base_addr);
  int32_t cur = tm_buckets(self)[b];
  TmEntry *pl = tm_pool(self);
  while (cur != -1) {
    const int32_t next = pl[cur].next_in_bucket;
    TmEntry *e = &pl[cur];
    if (tm_entry_valid(self, e) && e->base_addr == r->base_addr) {
      const TmOverlap st = tm_check_overlap(&probe, e);
      if (st != TM_OVERLAP_NONE) {
        if (!on_match(e, st, ctx)) {
          return;
        }
      }
    }
    cur = next;
  }
}

static inline void tm_remove(TmTensorMap *self, TmEntry *e) {
  const int32_t idx = (int32_t)(e - tm_pool(self));
  tm_remove_from_task(self, idx);
  tm_free_entry(self, idx);
}

static inline void tm_sync(TmTensorMap *self, uint32_t ring, int32_t last_alive) {
  tm_hdr(self)->last_alive[ring] = last_alive;
}

static inline void tm_cleanup_retired(TmTensorMap *self, uint32_t ring,
                                      int32_t old_alive, int32_t new_alive) {
  const uint32_t mask = tm_hdr(self)->cfg.task_window[ring] - 1u;
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

static inline uint32_t tm_task_slot(uint32_t ring, uint32_t local,
                                    const TmConfig *cfg) {
  return local & (cfg->task_window[ring] - 1u);
}

/* PTO2-aligned: periodic cleanup + slot-wrap trigger. */
static inline void tm_sync_tensormap(TmTensorMap *self, uint32_t ring,
                                     int32_t last_alive, uint32_t current_local) {
  tm_sync(self, ring, last_alive);
  const TmHeader *h = tm_hdr(self);
  const int32_t old = h->last_cleanup[ring];
  const bool slot_overlap =
      tm_task_slot(ring, current_local, &h->cfg) ==
      tm_task_slot(ring, (uint32_t)old, &h->cfg);
  if (last_alive - old >= (int32_t)TM_CLEANUP_INTERVAL || slot_overlap) {
    tm_cleanup_retired(self, ring, old, last_alive);
  }
}

static inline int32_t tm_valid_count(const TmTensorMap *self) {
  const TmHeader *h = tm_hdr(self);
  const TmEntry *pl = tm_pool(self);
  int32_t n = 0;
  for (int32_t i = 0; i < h->next_entry_idx; i++) {
    if (pl[i].bucket_index != -1 && tm_entry_valid(self, &pl[i])) {
      n++;
    }
  }
  return n;
}

/* ===========================================================================
 * High-level tensormap dependency layer
 * ========================================================================== */
#ifdef DAG_RING_BUF_H

#ifndef TMD_POOL_SIZE
#define TMD_POOL_SIZE 8192u
#endif
#ifndef TMD_NUM_BUCKETS
#define TMD_NUM_BUCKETS 2048u
#endif
#ifndef TMD_TASK_WINDOW
#define TMD_TASK_WINDOW 65536u
#endif

#define TMD_BUF_BYTES                                                          \
  (sizeof(TmHeader) + (uint64_t)TMD_NUM_BUCKETS * sizeof(int32_t) +            \
   TM_ENTRY_ALIGN + (uint64_t)TMD_POOL_SIZE * sizeof(TmEntry) +                \
   (uint64_t)TMD_POOL_SIZE * sizeof(int32_t) +                                 \
   (uint64_t)TMD_TASK_WINDOW * sizeof(int32_t))

static TmTensorMap g_tm_map;
static _Alignas(TM_ENTRY_ALIGN) uint8_t g_tm_buf[TMD_BUF_BYTES];

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

static TmPendingSlot g_tm_pend[TM_PENDING_MAX_IO];
static int g_tm_pend_n;

static inline void tm_pending_clear(void) { g_tm_pend_n = 0; }

static inline void tm_probe_from_tensor(const Tensor *t, TmProbe *p) {
  memset(p, 0, sizeof(*p));
  p->base_addr = t->base;
  p->version = 0;
  p->ndims = TM_MAX_DIMS;
  p->elem_size = (uint16_t)t->dtype;
  p->is_contiguous = true;
  p->storage_numel = (uint64_t)t->storage[0] * t->storage[1];
  p->strides[0] = t->strides[0];
  p->strides[1] = t->strides[1];
  p->shapes[0] = t->shapes[0];
  p->shapes[1] = t->shapes[1];
  p->start_offset =
      (uint64_t)t->offsets[0] * t->strides[0] + (uint64_t)t->offsets[1];
  p->extent_elem = (uint64_t)t->shapes[0] * t->shapes[1];
}

static inline void tm_copy_tensor_to_entry(const Tensor *t, TmEntry *e) {
  TmProbe probe;
  tm_probe_from_tensor(t, &probe);
  e->base_addr = probe.base_addr;
  e->version = probe.version;
  e->ndims = probe.ndims;
  e->elem_size = probe.elem_size;
  e->manual_dep = 0;
  e->is_contiguous = 1;
  e->start_offset = probe.start_offset;
  e->storage_numel = probe.storage_numel;
  e->extent_elem_cache = probe.extent_elem;
  e->shapes[0] = probe.shapes[0];
  e->shapes[1] = probe.shapes[1];
  e->strides[0] = probe.strides[0];
  e->strides[1] = probe.strides[1];
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
  if (c->is_inout && st == TM_OVERLAP_COVERED) {
    tm_remove(&g_tm_map, e);
  }
  return true;
}

static inline void tm_lookup_tensor(TmTensorMap *self, const Tensor *t,
                                    TmMatchFn on_match, void *ctx) {
  TmProbe probe;
  tm_probe_from_tensor(t, &probe);
  const uint32_t b = tm_hash(self, t->base);
  int32_t cur = tm_buckets(self)[b];
  TmEntry *pl = tm_pool(self);

  while (cur != -1) {
    const int32_t next = pl[cur].next_in_bucket;
    TmEntry *e = &pl[cur];
    if (tm_entry_valid(self, e) && e->base_addr == t->base) {
      const TmOverlap st = tm_check_overlap(&probe, e);
      if (st != TM_OVERLAP_NONE && !on_match(e, st, ctx)) {
        return;
      }
    }
    cur = next;
  }
}

static inline void tm_insert_tensor(TmTensorMap *self, const Tensor *t,
                                    uint16_t tid) {
  const int32_t idx = tm_new_entry(self);
  if (idx < 0) {
    return;
  }
  TmEntry *e = &tm_pool(self)[idx];
  tm_copy_tensor_to_entry(t, e);
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
  for (uint32_t r = 1; r < TM_MAX_RINGS; r++) {
    cfg.task_window[r] = 1;
  }
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
  for (i = 0; i < ctx.pn; i++) {
    succeed(tid, ctx.preds[i]);
  }
  for (i = 0; i < g_tm_pend_n; i++) {
    if (g_tm_pend[i].kind & TM_PEND_OUT) {
      tm_insert_tensor(&g_tm_map, g_tm_pend[i].t, tid);
    }
  }
  tm_pending_clear();
#endif
  submit(tid);
#if !NO_DEPS
  tm_sync_tensormap(&g_tm_map, 0, (int32_t)g_min_uncomplete_task, tid);
#endif
}

#endif /* DAG_RING_BUF_H */

#ifdef __cplusplus
}
#endif

#endif /* USE_TENSORMAP */

#endif /* ESL_PROXY_TENSORMAP_H */
