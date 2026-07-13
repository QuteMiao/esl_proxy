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
 * tensormap_core.h — PTO2-aligned producer-lookup map (Layer A, pure C).
 *
 * Aligned with simpler PTO2TensorMap. Orchestration glue: tensormap.h.
 */

#ifndef ESL_PROXY_TENSORMAP_CORE_H
#define ESL_PROXY_TENSORMAP_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TM_MAX_DIMS = 5,
    TM_MAX_RINGS = 4
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

typedef enum TmOverlap {
    TM_OVERLAP_NONE = 0,
    TM_OVERLAP_COVERED = 1,
    TM_OVERLAP_OTHER = 2
} TmOverlap;

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
    uint8_t dtype;
    uint8_t manual_dep;
    uint8_t is_contiguous;
    uint8_t __padding1__;
    uint32_t shapes[TM_MAX_DIMS];

    /* === cache line 2 (64B) === */
    int32_t prev_in_bucket;
    int32_t next_in_task;
    int32_t prev_in_task;
    int32_t bucket_index;
    uint64_t extent_elem_cache;
    uint32_t strides[TM_MAX_DIMS];
    uint32_t _line2_pad[5];
} TmEntry;

_Static_assert(sizeof(TmEntry) == 128, "TmEntry must be 128 bytes");

/* Cache line 1 must mirror Tensor byte-for-byte so tm_copy_tensor_to_entry()
 * can copy it with one 64B memcpy. Keep these in sync with tensor.h. */
_Static_assert(offsetof(TmEntry, base_addr) == 0, "base_addr offset");
_Static_assert(offsetof(TmEntry, producer_id) == 16, "producer_id offset");
_Static_assert(offsetof(TmEntry, start_offset) == 24, "start_offset offset");
_Static_assert(offsetof(TmEntry, version) == 32, "version offset");
_Static_assert(offsetof(TmEntry, ndims) == 36, "ndims offset");
_Static_assert(offsetof(TmEntry, dtype) == 40, "dtype offset");
_Static_assert(offsetof(TmEntry, manual_dep) == 41, "manual_dep offset");
_Static_assert(offsetof(TmEntry, is_contiguous) == 42, "is_contiguous offset");
_Static_assert(offsetof(TmEntry, shapes) == 44, "shapes offset");
_Static_assert(offsetof(TmEntry, base_addr) == offsetof(Tensor, buffer_addr),
               "base_addr must overlay Tensor::buffer_addr");
_Static_assert(offsetof(TmEntry, start_offset) == offsetof(Tensor, start_offset),
               "start_offset must overlay Tensor::start_offset");
_Static_assert(offsetof(TmEntry, shapes) == offsetof(Tensor, shapes),
               "shapes must overlay Tensor::shapes");

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

/* Region pointers are bound once (tm_init / tm_attach) from the header offsets,
 * so hot-path accessors are plain member reads instead of base+offset each call.
 * Rebind via tm_attach() if the backing memory is relocated. */
typedef struct TmTensorMap {
    TmHeader *hdr; /* backing-buffer base; header lives at offset 0 */
    int32_t *buckets;
    TmEntry *pool;
    int32_t *free_list;
    int32_t *task_heads[TM_MAX_RINGS];
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
static inline uint32_t tm_ring_of(uint64_t id) {
    return (uint32_t)(id >> 32);
}
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

/**
 * PTO2-style overlap: L1 element-range, L2 row-major per-dim, L3 OTHER.
 * ndims==2 fast path for qwen3 / paged-attention style 2-D views.
 */
static inline TmOverlap tm_check_overlap(const Tensor *in, const TmEntry *e) {
    if (in->version > e->version) {
        return TM_OVERLAP_OTHER;
    }

    if (in->ndims == 2u && e->ndims == 2u) {
        uint64_t extent_elem;
        if (in->is_contiguous) {
            extent_elem = (uint64_t)in->shapes[0] * in->shapes[1];
        } else {
            extent_elem = in->extent_elem_cache;
        }

        uint64_t ent_extent;
        if (e->is_contiguous) {
            ent_extent = (uint64_t)e->shapes[0] * e->shapes[1];
        } else {
            ent_extent = e->extent_elem_cache;
        }

        const uint64_t in_end = in->start_offset + extent_elem;
        const uint64_t ent_end = e->start_offset + ent_extent;
        if (!(in_end > e->start_offset && ent_end > in->start_offset)) {
            return TM_OVERLAP_NONE;
        }

        if (in->dtype != e->dtype) {
            return TM_OVERLAP_OTHER;
        }
        if (in->strides[0] != e->strides[0] || in->strides[1] != e->strides[1]) {
            return TM_OVERLAP_OTHER;
        }
        if (e->strides[1] != 1u) {
            return TM_OVERLAP_OTHER;
        }
        if (e->strides[0] % e->strides[1] != 0u) {
            return TM_OVERLAP_OTHER;
        }

        const uint32_t ref_shape1 = e->strides[0] / e->strides[1];
        const uint32_t stride0 = e->strides[0];
        /* PTO2-aligned: storage size is read from the consumer tensor (it shares
         * the same buffer as the entry, matched by base_addr), not persisted. */
        const uint64_t numel_storage =
            in->dtype != 0u ? in->buffer_size / (uint64_t)in->dtype : 0u;
        if (stride0 == 0u || numel_storage % stride0 != 0u) {
            return TM_OVERLAP_OTHER;
        }
        const uint32_t ref_shape0 = (uint32_t)(numel_storage / stride0);

        const uint32_t s0 = e->strides[0];
        const uint32_t s1 = e->strides[1];
        uint64_t in_remain = in->start_offset;
        uint64_t ent_remain = e->start_offset;
        const uint32_t in_off0 = (uint32_t)(in_remain / s0);
        in_remain %= s0;
        const uint32_t in_off1 = (uint32_t)(in_remain / s1);
        in_remain %= s1;
        const uint32_t ent_off0 = (uint32_t)(ent_remain / s0);
        ent_remain %= s0;
        const uint32_t ent_off1 = (uint32_t)(ent_remain / s1);
        ent_remain %= s1;
        if (in_remain != 0u || ent_remain != 0u) {
            return TM_OVERLAP_OTHER;
        }

        if ((uint64_t)in_off0 + in->shapes[0] > ref_shape0 ||
            (uint64_t)ent_off0 + e->shapes[0] > ref_shape0) {
            return TM_OVERLAP_OTHER;
        }
        if ((uint64_t)in_off1 + in->shapes[1] > ref_shape1 ||
            (uint64_t)ent_off1 + e->shapes[1] > ref_shape1) {
            return TM_OVERLAP_OTHER;
        }

        const uint64_t in_a1_0 = (uint64_t)in_off0 + in->shapes[0];
        const uint64_t ent_b1_0 = (uint64_t)ent_off0 + e->shapes[0];
        if (!(in_a1_0 > ent_off0 && ent_b1_0 > in_off0)) {
            return TM_OVERLAP_NONE;
        }
        bool input_contains_entry = (in_off0 <= ent_off0 && ent_b1_0 <= in_a1_0);

        const uint64_t in_a1_1 = (uint64_t)in_off1 + in->shapes[1];
        const uint64_t ent_b1_1 = (uint64_t)ent_off1 + e->shapes[1];
        if (!(in_a1_1 > ent_off1 && ent_b1_1 > in_off1)) {
            return TM_OVERLAP_NONE;
        }
        if (!(in_off1 <= ent_off1 && ent_b1_1 <= in_a1_1)) {
            input_contains_entry = false;
        }

        return input_contains_entry ? TM_OVERLAP_COVERED : TM_OVERLAP_OTHER;
    }

    uint64_t extent_elem;
    if (in->is_contiguous) {
        extent_elem = 1;
        for (uint32_t i = 0; i < in->ndims; i++) {
            extent_elem *= in->shapes[i];
        }
    } else {
        extent_elem = in->extent_elem_cache;
    }

    const uint64_t in_begin = in->start_offset;
    const uint64_t in_end = in->start_offset + extent_elem;
    const uint64_t ent_begin = e->start_offset;
    const uint64_t ent_end = e->start_offset + tm_entry_extent_elem(e);
    if (!(in_end > ent_begin && ent_end > in_begin)) {
        return TM_OVERLAP_NONE;
    }

    if (in->dtype != e->dtype || in->ndims != e->ndims ||
        in->ndims == 0) {
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
    const uint64_t numel_storage =
        in->dtype != 0u ? in->buffer_size / (uint64_t)in->dtype : 0u;
    if (stride0 == 0u || numel_storage % stride0 != 0u) {
        return TM_OVERLAP_OTHER;
    }
    ref_shapes[0] = (uint32_t)(numel_storage / stride0);

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
        if ((uint64_t)in_offsets[i] + in->shapes[i] > ref_shapes[i] ||
            (uint64_t)ent_offsets[i] + e->shapes[i] > ref_shapes[i]) {
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
    return self->hdr;
}
static inline int32_t *tm_buckets(const TmTensorMap *self) {
    return self->buckets;
}
static inline TmEntry *tm_pool(const TmTensorMap *self) {
    return self->pool;
}
static inline int32_t *tm_free_list(const TmTensorMap *self) {
    return self->free_list;
}
static inline int32_t *tm_task_heads(const TmTensorMap *self, uint32_t ring) {
    return self->task_heads[ring];
}

/* Bind the cached region pointers from the header's offset table. Call after
 * the header is populated (tm_init) or when attaching to a relocated image. */
static inline void tm_bind(TmTensorMap *self, void *base) {
    uint8_t *b = (uint8_t *)base;
    TmHeader *h = (TmHeader *)b;
    self->hdr = h;
    self->buckets = (int32_t *)(b + h->off_buckets);
    self->pool = (TmEntry *)(b + h->off_pool);
    self->free_list = (int32_t *)(b + h->off_free);
    for (uint32_t r = 0; r < TM_MAX_RINGS; r++) {
        self->task_heads[r] = (int32_t *)(b + h->off_task_heads[r]);
    }
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
    TmHeader *h = (TmHeader *)base;
    h->cfg = *cfg;
    h->next_entry_idx = 0;
    h->free_num = 0;
    for (uint32_t r = 0; r < TM_MAX_RINGS; r++) {
        h->last_alive[r] = 0;
        h->last_cleanup[r] = 0;
        h->off_task_heads[r] = 0;
    }
    tm_layout(cfg, h);
    tm_bind(self, base); /* offsets now populated; bind cached region pointers */

    int32_t *bk = tm_buckets(self);
    for (uint32_t i = 0; i < cfg->num_buckets; i++) {
        bk[i] = -1;
    }
    TmEntry *pl = tm_pool(self);
    for (uint32_t i = 0; i < cfg->pool_size; i++) {
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
    tm_bind(self, base);
}

static inline void tm_copy_tensor_to_entry(const Tensor *t, TmEntry *e) {
    /* Cache line 1 is byte-identical to Tensor (see TmEntry static_asserts), so
     * one 64B copy populates base_addr, start_offset, version, ndims, dtype,
     * manual_dep, is_contiguous and shapes[]. The copy also writes Tensor's
     * buffer_size/owner_task_id over next_in_bucket/_pad_nb/producer_id, which
     * is harmless: tm_link_entry() overwrites those right after. Cache line 2
     * (extent/strides) is derived below, so the producer's line 2 stays cold
     * for canonically-contiguous tensors. */
    memcpy(e, t, 64);
    if (t->is_contiguous && t->start_offset == 0) {
        if (t->ndims == 2u) {
            e->extent_elem_cache = (uint64_t)t->shapes[0] * t->shapes[1];
            e->strides[0] = t->shapes[1];
            e->strides[1] = 1u;
        } else {
            uint64_t numel = 1;
            for (uint32_t i = 0; i < t->ndims; i++) {
                numel *= t->shapes[i];
            }
            e->extent_elem_cache = numel;
            uint32_t s = 1;
            for (int32_t i = (int32_t)t->ndims - 1; i >= 0; i--) {
                e->strides[i] = s;
                s *= t->shapes[i];
            }
        }
    } else {
        e->extent_elem_cache = t->extent_elem_cache;
        memcpy(e->strides, t->strides, sizeof e->strides);
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
    tm_link_entry(self, idx, t->buffer_addr, tm_make_id(0, tid));
}

static inline void tm_lookup_tensor(TmTensorMap *self, const Tensor *t,
    TmMatchFn on_match, void *ctx) {
    const uint32_t b = tm_hash(self, t->buffer_addr);
    int32_t cur = tm_buckets(self)[b];
    TmEntry *pl = tm_pool(self);

    while (cur != -1) {
        const int32_t next = pl[cur].next_in_bucket;
        TmEntry *e = &pl[cur];
        if (tm_entry_valid(self, e) && e->base_addr == t->buffer_addr) {
            const TmOverlap st = tm_check_overlap(t, e);
            if (st != TM_OVERLAP_NONE && !on_match(e, st, ctx)) {
                return;
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

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_TENSORMAP_CORE_H */
