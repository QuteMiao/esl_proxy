/*
 * test_tensormap.c - PTO2-aligned tensormap self-test + micro-benchmarks.
 */

#define _POSIX_C_SOURCE 200809L

#include "tensor.h"
#include "tensormap_core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static _Alignas(128) uint8_t g_buf[1u << 20];
static _Alignas(128) uint8_t g_buf2[1u << 20];

static TmConfig make_config(void) {
  TmConfig cfg = {0};
  cfg.num_buckets = 16;
  cfg.pool_size = 64;
  cfg.num_rings = 1;
  cfg.task_window[0] = 64;
  return cfg;
}

/* 1D FLOAT32 view: `storage` elements in the buffer, slice [off, off+len). */
static Tensor tensor_1d(uint64_t base, uint64_t off, uint64_t len,
                        uint64_t storage) {
  const uint32_t shapes[1] = {(uint32_t)len};
  Tensor t = tensor_from_base_layout(base, shapes, 1, FLOAT32);
  t.buffer_size = storage * (uint64_t)FLOAT32;
  t.start_offset = off;
  return t;
}

/* 1x1 single-element view (elem_size 1) used to model a whole-buffer handle. */
static Tensor tensor_whole_buffer(uint64_t base) {
  const uint32_t shapes[2] = {1, 1};
  return tensor_from_base_layout(base, shapes, 2, (dtype_t)1);
}

/* 2D FLOAT32 tile of [stor_d0, stor_d1], rows [row0, row0+nrows). */
static Tensor tensor_2d_rows(uint64_t base, uint32_t stor_d0, uint32_t stor_d1,
                             uint32_t row0, uint32_t nrows) {
  const uint32_t shapes[2] = {stor_d0, stor_d1};
  Tensor tile = tensor_from_base_layout(base, shapes, 2, FLOAT32);
  return view(tile, row0, 0, nrows, stor_d1);
}

typedef struct {
  uint64_t producers[32];
  TmOverlap statuses[32];
  int count;
} HitSink;

static bool collect_cb(TmEntry *e, TmOverlap st, void *ctx) {
  HitSink *s = (HitSink *)ctx;
  s->producers[s->count] = e->producer_id;
  s->statuses[s->count] = st;
  s->count++;
  return true;
}

static HitSink collect(TmTensorMap *map, Tensor probe) {
  HitSink s = {0};
  tm_lookup_tensor(map, &probe, collect_cb, &s);
  return s;
}

static bool remove_cb(TmEntry *e, TmOverlap st, void *ctx) {
  (void)st;
  tm_remove((TmTensorMap *)ctx, e);
  return true;
}

static bool sink_has_local(const HitSink *s, uint32_t local) {
  for (int i = 0; i < s->count; i++) {
    if (tm_local_of(s->producers[i]) == local) {
      return true;
    }
  }
  return false;
}

static void test_overlap_semantics(void) {
  printf("Test: overlap_semantics\n");
  TmConfig cfg = make_config();
  assert(tm_bytes_required(&cfg) <= sizeof(g_buf));
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  Tensor a = tensor_1d(0x1000, 0, 128, 128);
  tm_insert_tensor(&map, &a, 1);
  HitSink s = collect(&map, tensor_1d(0x1000, 0, 128, 128));
  assert(s.count == 1);
  assert(tm_local_of(s.producers[0]) == 1);
  assert(s.statuses[0] == TM_OVERLAP_COVERED);

  tm_init(&map, g_buf, &cfg);
  Tensor b = tensor_1d(0x2000, 0, 128, 256);
  tm_insert_tensor(&map, &b, 2);
  s = collect(&map, tensor_1d(0x2000, 128, 128, 256));
  assert(s.count == 0);

  tm_init(&map, g_buf, &cfg);
  Tensor c = tensor_1d(0x3000, 0, 128, 256);
  tm_insert_tensor(&map, &c, 3);
  s = collect(&map, tensor_1d(0x3000, 64, 128, 256));
  assert(s.count == 1);
  assert(s.statuses[0] == TM_OVERLAP_OTHER);

  tm_init(&map, g_buf, &cfg);
  Tensor d = tensor_1d(0x4000, 0, 128, 128);
  tm_insert_tensor(&map, &d, 4);
  s = collect(&map, tensor_1d(0x5000, 0, 128, 128));
  assert(s.count == 0);

  printf("  PASSED\n");
}

static void test_l1_fast_reject(void) {
  printf("Test: l1_fast_reject\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  Tensor prod = tensor_1d(0xB000, 0, 64, 256);
  tm_insert_tensor(&map, &prod, 1);
  HitSink s = collect(&map, tensor_1d(0xB000, 200, 32, 256));
  assert(s.count == 0);

  printf("  PASSED\n");
}

static void test_version_guard(void) {
  printf("Test: version_guard\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  Tensor prod = tensor_1d(0xB100, 0, 64, 64);
  prod.version = 0;
  tm_insert_tensor(&map, &prod, 1);

  Tensor probe = tensor_1d(0xB100, 0, 64, 64);
  probe.version = 1;
  HitSink s = collect(&map, probe);
  assert(s.count == 1);
  assert(s.statuses[0] == TM_OVERLAP_OTHER);

  printf("  PASSED\n");
}

static void test_lazy_invalidation_and_reuse(void) {
  printf("Test: lazy_invalidation_and_reuse\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  Tensor r = tensor_1d(0x6000, 0, 128, 128);
  tm_insert_tensor(&map, &r, 5);
  assert(tm_valid_count(&map) == 1);

  tm_sync(&map, 0, 6);
  assert(tm_valid_count(&map) == 0);
  assert(collect(&map, tensor_1d(0x6000, 0, 128, 128)).count == 0);

  tm_init(&map, g_buf, &cfg);
  for (uint32_t local = 0; local < 8; local++) {
    Tensor rr = tensor_1d(0x7000, 0, 16, 128);
    tm_insert_tensor(&map, &rr, (uint32_t)local);
  }
  tm_sync_tensormap(&map, 0, (int32_t)TM_CLEANUP_INTERVAL, TM_CLEANUP_INTERVAL);
  assert(tm_valid_count(&map) == 0);

  Tensor fresh = tensor_1d(0x7000, 0, 16, 128);
  tm_insert_tensor(&map, &fresh, 100);
  HitSink s = collect(&map, tensor_1d(0x7000, 0, 16, 128));
  assert(s.count == 1 && tm_local_of(s.producers[0]) == 100);

  printf("  PASSED\n");
}

static void test_sync_interval_gating(void) {
  printf("Test: sync_interval_gating\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  for (uint32_t i = 0; i < 8; i++) {
    Tensor r = tensor_1d(0xE000, 0, 16, 128);
    tm_insert_tensor(&map, &r, (uint32_t)i);
  }
  assert(tm_hdr(&map)->next_entry_idx == 8);
  assert(tm_hdr(&map)->free_num == 0);

  tm_sync_tensormap(&map, 0, 5, 1);
  assert(tm_hdr(&map)->last_cleanup[0] == 0);
  assert(tm_hdr(&map)->free_num == 0);

  tm_sync_tensormap(&map, 0, (int32_t)TM_CLEANUP_INTERVAL, TM_CLEANUP_INTERVAL);
  assert(tm_hdr(&map)->last_cleanup[0] == (int32_t)TM_CLEANUP_INTERVAL);
  assert(tm_hdr(&map)->free_num >= 8);

  printf("  PASSED\n");
}

static void test_remove_in_callback(void) {
  printf("Test: remove_in_callback\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  Tensor r = tensor_1d(0x8000, 0, 64, 64);
  tm_insert_tensor(&map, &r, 0);
  Tensor probe = tensor_1d(0x8000, 0, 64, 64);
  tm_lookup_tensor(&map, &probe, remove_cb, &map);
  assert(collect(&map, tensor_1d(0x8000, 0, 64, 64)).count == 0);

  printf("  PASSED\n");
}

static void test_attach_relocated_image(void) {
  printf("Test: attach_relocated_image\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  Tensor r = tensor_1d(0x9000, 0, 128, 128);
  tm_insert_tensor(&map, &r, 7);

  uint64_t bytes = tm_bytes_required(&cfg);
  assert(bytes <= sizeof(g_buf2));
  memcpy(g_buf2, g_buf, bytes);
  TmTensorMap map2;
  tm_attach(&map2, g_buf2);

  HitSink s = collect(&map2, tensor_1d(0x9000, 0, 128, 128));
  assert(s.count == 1 && tm_local_of(s.producers[0]) == 7);
  assert(s.statuses[0] == TM_OVERLAP_COVERED);

  printf("  PASSED\n");
}

static void test_multi_producer_same_base(void) {
  printf("Test: multi_producer_same_base\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  Tensor p0 = tensor_1d(0xA000, 0, 64, 256);
  Tensor p1 = tensor_1d(0xA000, 0, 64, 256);
  tm_insert_tensor(&map, &p0, 0);
  tm_insert_tensor(&map, &p1, 1);
  HitSink s = collect(&map, tensor_1d(0xA000, 0, 64, 256));
  assert(s.count == 2);

  printf("  PASSED\n");
}

static void test_qwen3_dim1_column_slice(void) {
  printf("Test: qwen3_dim1_column_slice (gate_tile dim=1 piece overlap)\n");
  Tensor tile = tensor_from_base_layout(0xE0000, (uint32_t[]){16, 17408}, 2,
                                        FLOAT32);
  Tensor prod0 = view(tile, 0, 0, 16, 512);
  Tensor cons1 = view(tile, 0, 512, 16, 512);

  assert(prod0.is_contiguous == 0);
  assert(prod0.strides[0] == 17408u && prod0.strides[1] == 1u);

  TmEntry entry;
  tm_copy_tensor_to_entry(&prod0, &entry);

  assert(tm_check_overlap(&cons1, &entry) == TM_OVERLAP_NONE);

  Tensor cons0 = view(tile, 0, 0, 16, 512);
  assert(tm_check_overlap(&cons0, &entry) == TM_OVERLAP_COVERED);

  printf("  PASSED\n");
}

static void test_2d_row_tile_overlap(void) {
  printf("Test: 2d_row_tile_overlap (qwen3-style tile rows)\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  const uint64_t base = 0xD0000;
  const uint32_t batch = 96, hidden = 5120, tile = 16;

  Tensor prod = tensor_2d_rows(base, batch, hidden, 16, tile);
  tm_insert_tensor(&map, &prod, 3);

  HitSink hit = collect(&map, tensor_2d_rows(base, batch, hidden, 16, tile));
  assert(hit.count == 1 && tm_local_of(hit.producers[0]) == 3);

  HitSink miss = collect(&map, tensor_2d_rows(base, batch, hidden, 0, tile));
  assert(miss.count == 0);

  HitSink part = collect(&map, tensor_2d_rows(base, batch, hidden, 8, 16));
  assert(part.count == 1 && part.statuses[0] == TM_OVERLAP_OTHER);

  printf("  PASSED\n");
}

static void test_coexistence_demo(void) {
  printf("Test: coexistence_demo\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  const uint64_t X = 0xBEEF000;
  const uint64_t Y = 0xCAFE000;

  Tensor rx = tensor_whole_buffer(X);
  Tensor ry = tensor_whole_buffer(Y);
  tm_insert_tensor(&map, &rx, 10);
  tm_insert_tensor(&map, &rx, 11);
  tm_insert_tensor(&map, &ry, 12);

  HitSink cx = collect(&map, tensor_whole_buffer(X));
  assert(cx.count == 2);
  assert(sink_has_local(&cx, 10) && sink_has_local(&cx, 11));

  HitSink cy = collect(&map, tensor_whole_buffer(Y));
  assert(cy.count == 1 && sink_has_local(&cy, 12));

  tm_sync(&map, 0, 12);
  assert(collect(&map, tensor_whole_buffer(X)).count == 0);
  assert(collect(&map, tensor_whole_buffer(Y)).count == 1);

  printf("  PASSED\n");
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint32_t ceil_pow2(uint32_t x) {
  uint32_t p = 1;
  while (p < x) {
    p <<= 1;
  }
  return p;
}

static void *alloc_image(const TmConfig *cfg) {
  uint64_t bytes = tm_bytes_required(cfg);
  bytes = (bytes + 127u) & ~127u;
  void *p = aligned_alloc(128, bytes);
  assert(p != NULL);
  return p;
}

static void report(const char *name, double ops, double secs) {
  const double ns_per = ops > 0.0 ? (secs / ops) * 1e9 : 0.0;
  const double mops = secs > 0.0 ? (ops / secs) / 1e6 : 0.0;
  printf("  %-30s %11.0f ops  %8.2f ms  %8.2f ns/op  %9.2f Mops/s\n", name,
         ops, secs * 1e3, ns_per, mops);
}

static volatile uint64_t g_perf_sink;
static bool perf_count_cb(TmEntry *e, TmOverlap st, void *ctx) {
  (void)st;
  *(uint64_t *)ctx += e->producer_id;
  return true;
}

static uint64_t perf_rng(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

static void bench_insert(uint32_t n) {
  TmConfig cfg = {0};
  cfg.num_buckets = 65536;
  cfg.pool_size = n;
  cfg.num_rings = 1;
  cfg.task_window[0] = 1024;
  void *img = alloc_image(&cfg);
  TmTensorMap map;
  tm_init(&map, img, &cfg);

  const double t0 = now_sec();
  for (uint32_t i = 0; i < n; i++) {
    Tensor r = tensor_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    tm_insert_tensor(&map, &r, (uint32_t)(i & (cfg.task_window[0] - 1)));
  }
  report("insert", (double)n, now_sec() - t0);
  free(img);
}

static void bench_lookup(uint32_t m, uint32_t queries) {
  TmConfig cfg = {0};
  cfg.num_buckets = ceil_pow2(m);
  cfg.pool_size = m;
  cfg.num_rings = 1;
  cfg.task_window[0] = 1024;
  void *img = alloc_image(&cfg);
  TmTensorMap map;
  tm_init(&map, img, &cfg);

  for (uint32_t i = 0; i < m; i++) {
    Tensor r = tensor_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    tm_insert_tensor(&map, &r, (uint32_t)i);
  }

  uint64_t rng = 0x9E3779B97F4A7C15ULL;
  uint64_t acc = 0;
  const double t0 = now_sec();
  for (uint32_t q = 0; q < queries; q++) {
    const uint32_t i = (uint32_t)(perf_rng(&rng) % m);
    Tensor probe = tensor_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    tm_lookup_tensor(&map, &probe, perf_count_cb, &acc);
  }
  g_perf_sink = acc;
  report("lookup (covered hit)", (double)queries, now_sec() - t0);
  free(img);
}

static void bench_submit(uint32_t window, uint32_t per_task, uint32_t iters) {
  TmConfig cfg = {0};
  cfg.num_buckets = 4096;
  cfg.pool_size = (window + 2) * per_task;
  cfg.num_rings = 1;
  cfg.task_window[0] = window;
  void *img = alloc_image(&cfg);
  TmTensorMap map;
  tm_init(&map, img, &cfg);

  uint32_t cur = 0;
  for (uint32_t w = 0; w < window; w++, cur++) {
    for (uint32_t k = 0; k < per_task; k++) {
      Tensor r =
          tensor_1d(0x200000u + (((uint64_t)cur * per_task + k) & 4095u) * 256u,
                    0, 64, 64);
      tm_insert_tensor(&map, &r, (uint32_t)cur);
    }
  }

  const double t0 = now_sec();
  for (uint32_t i = 0; i < iters; i++, cur++) {
    for (uint32_t k = 0; k < per_task; k++) {
      Tensor r =
          tensor_1d(0x200000u + (((uint64_t)cur * per_task + k) & 4095u) * 256u,
                    0, 64, 64);
      tm_insert_tensor(&map, &r, (uint32_t)cur);
    }
    tm_sync_tensormap(&map, 0, (int32_t)(cur - window + 1), cur);
  }
  report("submit (insert+sync+cleanup)", (double)iters, now_sec() - t0);
  free(img);
}

static void run_benchmarks(uint32_t scale) {
  printf("=== Performance (scale=%u) ===\n", scale);
  bench_insert(200000u * scale);
  bench_lookup(100000u, 1000000u * scale);
  bench_submit(1024u, 4u, 200000u * scale);
  printf("\n");
}

int main(int argc, char **argv) {
  printf("=== TensorMap PTO2-aligned Tests ===\n\n");

  test_overlap_semantics();
  test_l1_fast_reject();
  test_version_guard();
  test_lazy_invalidation_and_reuse();
  test_sync_interval_gating();
  test_remove_in_callback();
  test_attach_relocated_image();
  test_multi_producer_same_base();
  test_2d_row_tile_overlap();
  test_qwen3_dim1_column_slice();
  test_coexistence_demo();

  printf("\n=== All Tests Passed ===\n\n");

  uint32_t scale = 1;
  if (argc > 1) {
    unsigned long s = strtoul(argv[1], NULL, 10);
    if (s >= 1 && s <= 1000) {
      scale = (uint32_t)s;
    }
  }
  run_benchmarks(scale);
  return 0;
}
