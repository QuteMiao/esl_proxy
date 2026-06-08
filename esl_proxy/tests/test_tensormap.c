/*
 * test_tensormap.c - PTO2-aligned tensormap self-test + micro-benchmarks.
 */

#define _POSIX_C_SOURCE 200809L

#include "tensormap.h"

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

static TmRegion region_1d(uint64_t base, uint64_t off, uint64_t len,
                          uint64_t storage) {
  TmRegion r = {0};
  r.base_addr = base;
  r.storage_numel = storage;
  r.elem_size = 4;
  r.shapes[0] = 1;
  r.shapes[1] = (uint16_t)len;
  r.offsets[0] = 0;
  r.offsets[1] = (uint16_t)off;
  r.strides[0] = (uint16_t)storage;
  r.strides[1] = 1;
  return r;
}

static TmRegion whole_buffer_region(uint64_t base) {
  TmRegion r = {0};
  r.base_addr = base;
  r.storage_numel = 1;
  r.elem_size = 1;
  r.shapes[0] = 1;
  r.shapes[1] = 1;
  r.offsets[0] = 0;
  r.offsets[1] = 0;
  r.strides[0] = 1;
  r.strides[1] = 1;
  return r;
}

static TmRegion region_2d(uint64_t base, uint32_t stor_d0, uint32_t stor_d1,
                          uint32_t row0, uint32_t nrows, uint32_t d1,
                          uint32_t elem_size) {
  TmRegion r;
  memset(&r, 0, sizeof r);
  r.base_addr = base;
  r.storage_numel = (uint64_t)stor_d0 * stor_d1;
  r.elem_size = (uint16_t)elem_size;
  r.shapes[0] = (uint16_t)nrows;
  r.shapes[1] = (uint16_t)d1;
  r.offsets[0] = (uint16_t)row0;
  r.offsets[1] = 0;
  r.strides[0] = (uint16_t)d1;
  r.strides[1] = 1;
  return r;
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

static HitSink collect(TmTensorMap *map, TmRegion probe) {
  HitSink s = {0};
  tm_lookup(map, &probe, collect_cb, &s);
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

  TmRegion a = region_1d(0x1000, 0, 128, 128);
  tm_insert(&map, &a, tm_make_id(0, 1));
  HitSink s = collect(&map, region_1d(0x1000, 0, 128, 128));
  assert(s.count == 1);
  assert(tm_local_of(s.producers[0]) == 1);
  assert(s.statuses[0] == TM_OVERLAP_COVERED);

  tm_init(&map, g_buf, &cfg);
  TmRegion b = region_1d(0x2000, 0, 128, 256);
  tm_insert(&map, &b, tm_make_id(0, 2));
  s = collect(&map, region_1d(0x2000, 128, 128, 256));
  assert(s.count == 0);

  tm_init(&map, g_buf, &cfg);
  TmRegion c = region_1d(0x3000, 0, 128, 256);
  tm_insert(&map, &c, tm_make_id(0, 3));
  s = collect(&map, region_1d(0x3000, 64, 128, 256));
  assert(s.count == 1);
  assert(s.statuses[0] == TM_OVERLAP_OTHER);

  tm_init(&map, g_buf, &cfg);
  TmRegion d = region_1d(0x4000, 0, 128, 128);
  tm_insert(&map, &d, tm_make_id(0, 4));
  s = collect(&map, region_1d(0x5000, 0, 128, 128));
  assert(s.count == 0);

  printf("  PASSED\n");
}

static void test_l1_fast_reject(void) {
  printf("Test: l1_fast_reject\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion prod = region_1d(0xB000, 0, 64, 256);
  tm_insert(&map, &prod, tm_make_id(0, 1));
  HitSink s = collect(&map, region_1d(0xB000, 200, 32, 256));
  assert(s.count == 0);

  printf("  PASSED\n");
}

static void test_version_guard(void) {
  printf("Test: version_guard\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion prod = region_1d(0xB100, 0, 64, 64);
  prod.version = 0;
  tm_insert(&map, &prod, tm_make_id(0, 1));

  TmRegion probe = region_1d(0xB100, 0, 64, 64);
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

  TmRegion r = region_1d(0x6000, 0, 128, 128);
  tm_insert(&map, &r, tm_make_id(0, 5));
  assert(tm_valid_count(&map) == 1);

  tm_sync(&map, 0, 6);
  assert(tm_valid_count(&map) == 0);
  assert(collect(&map, region_1d(0x6000, 0, 128, 128)).count == 0);

  tm_init(&map, g_buf, &cfg);
  for (uint32_t local = 0; local < 8; local++) {
    TmRegion rr = region_1d(0x7000, 0, 16, 128);
    tm_insert(&map, &rr, tm_make_id(0, local));
  }
  tm_sync_tensormap(&map, 0, (int32_t)TM_CLEANUP_INTERVAL, TM_CLEANUP_INTERVAL);
  assert(tm_valid_count(&map) == 0);

  TmRegion fresh = region_1d(0x7000, 0, 16, 128);
  tm_insert(&map, &fresh, tm_make_id(0, 100));
  HitSink s = collect(&map, region_1d(0x7000, 0, 16, 128));
  assert(s.count == 1 && tm_local_of(s.producers[0]) == 100);

  printf("  PASSED\n");
}

static void test_sync_interval_gating(void) {
  printf("Test: sync_interval_gating\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  for (uint32_t i = 0; i < 8; i++) {
    TmRegion r = region_1d(0xE000, 0, 16, 128);
    tm_insert(&map, &r, tm_make_id(0, i));
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

  TmRegion r = region_1d(0x8000, 0, 64, 64);
  tm_insert(&map, &r, tm_make_id(0, 0));
  TmRegion probe = region_1d(0x8000, 0, 64, 64);
  tm_lookup(&map, &probe, remove_cb, &map);
  assert(collect(&map, region_1d(0x8000, 0, 64, 64)).count == 0);

  printf("  PASSED\n");
}

static void test_attach_relocated_image(void) {
  printf("Test: attach_relocated_image\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion r = region_1d(0x9000, 0, 128, 128);
  tm_insert(&map, &r, tm_make_id(1 % cfg.num_rings, 7));

  uint64_t bytes = tm_bytes_required(&cfg);
  assert(bytes <= sizeof(g_buf2));
  memcpy(g_buf2, g_buf, bytes);
  TmTensorMap map2;
  tm_attach(&map2, g_buf2);

  HitSink s = collect(&map2, region_1d(0x9000, 0, 128, 128));
  assert(s.count == 1 && tm_local_of(s.producers[0]) == 7);
  assert(s.statuses[0] == TM_OVERLAP_COVERED);

  printf("  PASSED\n");
}

static void test_multi_producer_same_base(void) {
  printf("Test: multi_producer_same_base\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion p0 = region_1d(0xA000, 0, 64, 256);
  TmRegion p1 = region_1d(0xA000, 0, 64, 256);
  tm_insert(&map, &p0, tm_make_id(0, 0));
  tm_insert(&map, &p1, tm_make_id(0, 1));
  HitSink s = collect(&map, region_1d(0xA000, 0, 64, 256));
  assert(s.count == 2);

  printf("  PASSED\n");
}

static void test_2d_row_tile_overlap(void) {
  printf("Test: 2d_row_tile_overlap (qwen3-style tile rows)\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  const uint64_t base = 0xD0000;
  const uint32_t batch = 96, hidden = 5120, tile = 16;

  TmRegion prod = region_2d(base, batch, hidden, 16, tile, hidden, 4);
  tm_insert(&map, &prod, tm_make_id(0, 3));

  HitSink hit = collect(&map, region_2d(base, batch, hidden, 16, tile, hidden, 4));
  assert(hit.count == 1 && tm_local_of(hit.producers[0]) == 3);

  HitSink miss = collect(&map, region_2d(base, batch, hidden, 0, tile, hidden, 4));
  assert(miss.count == 0);

  HitSink part = collect(&map, region_2d(base, batch, hidden, 8, 16, hidden, 4));
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

  TmRegion rx = whole_buffer_region(X);
  TmRegion ry = whole_buffer_region(Y);
  tm_insert(&map, &rx, tm_make_id(0, 10));
  tm_insert(&map, &rx, tm_make_id(0, 11));
  tm_insert(&map, &ry, tm_make_id(0, 12));

  HitSink cx = collect(&map, whole_buffer_region(X));
  assert(cx.count == 2);
  assert(sink_has_local(&cx, 10) && sink_has_local(&cx, 11));

  HitSink cy = collect(&map, whole_buffer_region(Y));
  assert(cy.count == 1 && sink_has_local(&cy, 12));

  tm_sync(&map, 0, 12);
  assert(collect(&map, whole_buffer_region(X)).count == 0);
  assert(collect(&map, whole_buffer_region(Y)).count == 1);

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
    TmRegion r = region_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    tm_insert(&map, &r, tm_make_id(0, i & (cfg.task_window[0] - 1)));
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
    TmRegion r = region_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    tm_insert(&map, &r, tm_make_id(0, i));
  }

  uint64_t rng = 0x9E3779B97F4A7C15ULL;
  uint64_t acc = 0;
  const double t0 = now_sec();
  for (uint32_t q = 0; q < queries; q++) {
    const uint32_t i = (uint32_t)(perf_rng(&rng) % m);
    TmRegion probe = region_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    tm_lookup(&map, &probe, perf_count_cb, &acc);
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
      TmRegion r =
          region_1d(0x200000u + (((uint64_t)cur * per_task + k) & 4095u) * 256u,
                    0, 64, 64);
      tm_insert(&map, &r, tm_make_id(0, cur));
    }
  }

  const double t0 = now_sec();
  for (uint32_t i = 0; i < iters; i++, cur++) {
    for (uint32_t k = 0; k < per_task; k++) {
      TmRegion r =
          region_1d(0x200000u + (((uint64_t)cur * per_task + k) & 4095u) * 256u,
                    0, 64, 64);
      tm_insert(&map, &r, tm_make_id(0, cur));
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
