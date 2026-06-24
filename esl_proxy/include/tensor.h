/*
 * tensor.h - PTO2-aligned Tensor layout (2 x 64B cache lines, 128B total).
 *
 * Field order and offsets match simpler runtime/tensor.h so cache line 1 can be
 * memcpy'd into TmEntry overlap fields (bytes [24,64) from start_offset onward).
 */

#ifndef ESL_PROXY_TENSOR_H
#define ESL_PROXY_TENSOR_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>


typedef enum {
    BFLOAT16 = 2,
    FLOAT32 = 4,
    INT32 = 4,
} dtype_t;

enum { ESL_PROXY_TENSOR_MAX_DIMS = 5 };

struct Tensor {
    /* === Cache line 1 (64B) — hot path === */
    uint64_t buffer_addr;
    uint64_t buffer_size;
    uint64_t owner_task_id;
    uint64_t start_offset;
    int32_t version;
    uint32_t ndims;
    uint8_t dtype;
    uint8_t manual_dep;
    uint8_t is_contiguous;
    uint8_t _pad_cl1;
    uint32_t shapes[ESL_PROXY_TENSOR_MAX_DIMS];

    /* === Cache line 2 (64B) — warm path === */
    uint64_t extent_elem_cache;
    uint32_t strides[ESL_PROXY_TENSOR_MAX_DIMS];
    uint8_t _pad_cl2[36];
} __attribute__((aligned(64)));

typedef struct Tensor Tensor;

_Static_assert(sizeof(Tensor) == 128, "Tensor must be exactly 2 cache lines (128 bytes)");
_Static_assert(_Alignof(Tensor) == 64, "Tensor must be 64-byte aligned");
_Static_assert(offsetof(Tensor, owner_task_id) == 16,
               "owner_task_id must be at bytes 16-23 (cacheline 1)");
_Static_assert(offsetof(Tensor, start_offset) == 24,
               "start_offset must be at bytes 24-31 (cacheline 1)");
_Static_assert(offsetof(Tensor, version) == 32, "version offset");
_Static_assert(offsetof(Tensor, ndims) == 36, "ndims offset");
_Static_assert(offsetof(Tensor, dtype) == 40, "dtype offset");
_Static_assert(offsetof(Tensor, manual_dep) == 41, "manual_dep offset");
_Static_assert(offsetof(Tensor, is_contiguous) == 42, "is_contiguous offset");
_Static_assert(offsetof(Tensor, shapes) == 44, "shapes must start at byte 44 (cacheline 1)");
_Static_assert(offsetof(Tensor, extent_elem_cache) == 64,
               "extent_elem_cache must start at byte 64 (cacheline 2)");
_Static_assert(offsetof(Tensor, strides) == 72, "strides offset");

static inline uint64_t tensor_data_addr(const Tensor *t) {
    return t->buffer_addr + t->start_offset * t->dtype;
}

static inline uint64_t tensor_numel(const Tensor *t)
{
    uint64_t n = 1;
    for (uint32_t i = 0; i < t->ndims; i++) {
        n *= t->shapes[i];
    }
    return n;
}

static inline Tensor tensor_from_base_layout(uint64_t base, const uint32_t shapes[],
                                             uint32_t ndims, dtype_t dtype) {
    Tensor t;
    uint64_t numel = 1;

    t.buffer_addr = base;
    t.buffer_size = 0;
    t.owner_task_id = 0;
    t.start_offset = 0;
    t.version = 0;
    t.ndims = ndims;
    t.dtype = (uint8_t)dtype;
    t.manual_dep = 0;
    for (uint32_t i = 0; i < ndims; i++) {
        t.shapes[i] = shapes[i];
        numel *= shapes[i];
    }
    t.buffer_size = numel * dtype;

    uint32_t stride = 1;
    uint64_t expected = 1;
    uint8_t contig = 1;
    uint64_t extent = 1;
    for (int32_t i = (int32_t)ndims - 1; i >= 0; i--) {
        t.strides[i] = stride;
        contig &= t.strides[i] == expected;
        extent += (t.shapes[i] - 1u) * t.strides[i];
        expected *= t.shapes[i];
        stride *= shapes[i];
    }
    t.is_contiguous = contig;
    t.extent_elem_cache = extent;
    return t;
}

static inline Tensor tensor_from_base(uint64_t base) {
    Tensor t;
    memset(&t, 0, sizeof t);
    t.buffer_addr = base;
    return t;
}

static inline Tensor view_at(const Tensor *t, uint32_t off0, uint32_t off1,
                             uint32_t n0, uint32_t n1)
{
    Tensor v = *t;
    v.ndims = 2;
    v.start_offset += off0 * v.strides[0] + off1 * v.strides[1];
    v.shapes[0] = n0;
    v.shapes[1] = n1;
    v.is_contiguous = v.strides[1] == 1 && v.strides[0] == v.shapes[1];
    v.extent_elem_cache =
        (v.shapes[0] - 1u) * v.strides[0] + (v.shapes[1] - 1u) * v.strides[1];
    return v;
}

/* view() keeps its by-value call interface (returns a Tensor) but avoids
 * copying the source Tensor into a parameter: the macro passes its address to
 * view_at(). All call sites pass an lvalue as the first argument. */
#define view(t, off0, off1, n0, n1) view_at(&(t), (off0), (off1), (n0), (n1))

#endif /* ESL_PROXY_TENSOR_H */
