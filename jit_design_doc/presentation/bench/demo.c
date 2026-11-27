// Talk benchmark: same kernels as ejit_test/ejit_perf_bench.c plus a realistic
// stream kernel, a per-call probe-cost kernel, a compile-latency probe and a
// dump of the real JIT machine code. Host-only measurement (x86_64). Not a
// board number.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ejit_test_helpers.h"

//===-- 1. official micro kernel (ejit_perf_bench.c::compute_cell) ---------===
struct CellCfg {
  EJIT_PERIOD_CONST uint32_t cellType;
  EJIT_PERIOD_CONST uint32_t priority;
  EJIT_PERIOD_CONST uint32_t maxPower;
  EJIT_PERIOD_CONST uint32_t minPower;
  EJIT_PERIOD_CONST uint32_t bandWidth;
  EJIT_PERIOD_CONST uint32_t timeSlot;
  uint32_t result;
};
#define N 16
EJIT_GROUP(cell) struct CellCfg g_cfg[N];
#define LOOP_ITERS 20000000ULL

EJIT_ENTRY uint64_t compute_cell(EJIT_DIM(cell) uint8_t ci) {
  uint64_t sum = 0;
  struct CellCfg *p = &g_cfg[ci];
  for (uint64_t i = 0; i < LOOP_ITERS; i++) {
    if (p->cellType == 0xFD) {
      if (p->priority > 10)
        sum += p->maxPower * p->bandWidth / (p->timeSlot + 1);
      else
        sum += p->minPower * p->bandWidth / (p->timeSlot + 1);
    } else if (p->cellType == 0xEC) {
      sum += (p->maxPower + p->minPower) * p->bandWidth / (p->timeSlot + 1);
    }
  }
  return sum;
}
__attribute__((noinline)) uint64_t compute_cell_nojit(uint8_t ci) {
  uint64_t sum = 0;
  struct CellCfg *p = &g_cfg[ci];
  for (uint64_t i = 0; i < LOOP_ITERS; i++) {
    if (p->cellType == 0xFD) {
      if (p->priority > 10)
        sum += p->maxPower * p->bandWidth / (p->timeSlot + 1);
      else
        sum += p->minPower * p->bandWidth / (p->timeSlot + 1);
    } else if (p->cellType == 0xEC) {
      sum += (p->maxPower + p->minPower) * p->bandWidth / (p->timeSlot + 1);
    }
  }
  return sum;
}

//===-- 2. interprocedural: noinline callee reads period_const fields -------===
__attribute__((noinline)) static uint64_t cell_step(uint8_t ci, uint64_t sum) {
  struct CellCfg *p = &g_cfg[ci];
  if (p->cellType == 0xFD) {
    if (p->priority > 10)
      return sum + p->maxPower * p->bandWidth / (p->timeSlot + 1);
    return sum + p->minPower * p->bandWidth / (p->timeSlot + 1);
  } else if (p->cellType == 0xEC) {
    return sum + (p->maxPower + p->minPower) * p->bandWidth / (p->timeSlot + 1);
  }
  return sum;
}
EJIT_ENTRY uint64_t compute_cell_ip(EJIT_DIM(cell) uint8_t ci) {
  uint64_t sum = 0;
  for (uint64_t i = 0; i < LOOP_ITERS; i++)
    sum = cell_step(ci, sum);
  return sum;
}
__attribute__((noinline)) uint64_t compute_cell_ip_nojit(uint8_t ci) {
  uint64_t sum = 0;
  for (uint64_t i = 0; i < LOOP_ITERS; i++)
    sum = cell_step(ci, sum);
  return sum;
}

//===-- 3. realistic stream kernel: data-dependent work cannot be folded ======
#define STREAM_N 4096
static int16_t g_samples[STREAM_N + 8];
struct StreamCfg {
  EJIT_PERIOD_CONST int mode;
  EJIT_PERIOD_CONST int gain_q;
  EJIT_PERIOD_CONST int thresh;
  int junk;
};
EJIT_GROUP(cell) struct StreamCfg g_scfg[N];
#define STREAM_REPEAT 4000

EJIT_ENTRY int64_t stream_kernel(EJIT_DIM(cell) uint8_t ci, const int16_t *x,
                                 int n) {
  const struct StreamCfg *c = &g_scfg[ci];
  int64_t acc = 0;
  for (int i = 0; i < n; i++) {
    int v = x[i];
    if (c->mode == 0) {
      if (v > c->thresh)
        acc += (int64_t)v * c->gain_q;
      else
        acc -= v;
    } else {
      acc += (int64_t)v * v;
    }
  }
  return acc;
}
__attribute__((noinline)) int64_t stream_kernel_nojit(uint8_t ci,
                                                      const int16_t *x, int n) {
  const struct StreamCfg *c = &g_scfg[ci];
  int64_t acc = 0;
  for (int i = 0; i < n; i++) {
    int v = x[i];
    if (c->mode == 0) {
      if (v > c->thresh)
        acc += (int64_t)v * c->gain_q;
      else
        acc -= v;
    } else {
      acc += (int64_t)v * v;
    }
  }
  return acc;
}

//===-- 4. tiny entry: per-call probe cost -----------------------------------===
struct Tiny { EJIT_PERIOD_CONST int a; int b; };
EJIT_GROUP(cell) struct Tiny g_tiny[N];
EJIT_ENTRY int tiny(EJIT_DIM(cell) uint8_t ci) { return g_tiny[ci].a + g_tiny[ci].b; }
__attribute__((noinline)) int tiny_nojit(uint8_t ci) { return g_tiny[ci].a + g_tiny[ci].b; }
#define TINY_CALLS 20000000ULL

//===-- timing ---------------------------------------------------------------===
static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}
static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}
static double median(double *v, int n) {
  qsort(v, n, sizeof(double), cmpd);
  return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

typedef int64_t (*stream_fn)(uint8_t, const int16_t *, int);

static uint32_t idx_micro, idx_ip, idx_stream, idx_tiny, dt_cell;

// Look the specialization up through the public C API (no wrapper involved).
static void *lookup(uint32_t idx, uint8_t cell, uint32_t *bucket) {
  ejit_dim_pair_t dim = {dt_cell, cell};
  void *fn = NULL;
  for (int poll = 0; poll < 200000; ++poll) {
    ejit_status_t st = ejit_taskpool_compile_or_get(idx, &dim, 1, &fn, bucket);
    if (st == EJIT_OK && fn)
      return fn;
    if (st != EJIT_PENDING && st != EJIT_OK) {
      printf("lookup status=%d\n", (int)st);
      return NULL;
    }
    usleep(50);
  }
  return NULL;
}

// Emit the first bytes of the real JIT code so objdump can show it. Stops at
// the first run of int3/zero padding after a ret.
static void dump_code(const char *name, uint8_t cell) {
  uint32_t idx;
  uint32_t bucket = 0;
  if (!strcmp(name, "compute_cell")) idx = idx_micro;
  else if (!strcmp(name, "compute_cell_ip")) idx = idx_ip;
  else if (!strcmp(name, "stream_kernel")) idx = idx_stream;
  else idx = idx_tiny;
  void *fn = lookup(idx, cell, &bucket);
  if (!fn) { printf("no code for %s cell %u\n", name, cell); return; }
  const uint8_t *p = (const uint8_t *)fn;
  size_t n = 0, limit = 4096;
  int ret_seen = 0;
  while (n < limit) {
    uint8_t b = p[n];
    if (b == 0xC3 || b == 0xC2) ret_seen = 1;          // ret / ret imm16
    else if (ret_seen && (b == 0xCC || b == 0x00)) {    // padding
      size_t k = n;
      while (k < limit && (p[k] == 0xCC || p[k] == 0x00)) ++k;
      if (k - n >= 1) break;
    } else ret_seen = 0;
    ++n;
  }
  char path[256];
  snprintf(path, sizeof path, "jit_%s_cell%u.bin", name, cell);
  FILE *f = fopen(path, "wb");
  fwrite(p, 1, n, f);
  fclose(f);
  printf("code %-18s cell=%u bytes=%zu addr=%p\n", name, cell, n, fn);
  ejit_taskpool_release_read(bucket);
}

int main(int argc, char **argv) {
  int mode_mem = argc > 1 && !strcmp(argv[1], "mem");
  uint8_t ci = 3;
  const int REPS = mode_mem ? 1 : 9;
  double t[16];

  g_cfg[ci] = (struct CellCfg){0xFD, 20, 100, 50, 4, 3, 0};
  g_scfg[ci] = (struct StreamCfg){0, 7, 100, 0};
  g_tiny[ci] = (struct Tiny){1000, 1};
  unsigned seed = 12345;
  for (int i = 0; i < STREAM_N + 8; i++) {
    seed = seed * 1664525u + 1013904223u;
    g_samples[i] = (int16_t)(seed >> 16);
  }

  ejit_register_lifecycle("cell", &dt_cell);
  ejit_register_funcindex("compute_cell", &idx_micro);
  ejit_register_funcindex("compute_cell_ip", &idx_ip);
  ejit_register_funcindex("stream_kernel", &idx_stream);
  ejit_register_funcindex("tiny", &idx_tiny);

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  // EJIT_DUMP_DIR=<dir>: runtime dumps each entry's pre/opt IR there
  const char *dumpdir = getenv("EJIT_DUMP_DIR");
  if (dumpdir) { mkdir(dumpdir, 0700); cfg.dumpJITDir = dumpdir; }
  if (ejit_init(&cfg) != EJIT_OK) { printf("init failed\n"); return 1; }
  if (ejit_activate("cell", ci) != EJIT_OK) { printf("activate failed\n"); return 1; }

  // ---- compile latency: first call (enqueue) -> first successful lookup ----
  // Only kernels whose AOT fallback body is short give a clean number.
  double lat_first[2];
  uint32_t bucket = 0;
  { double a = now_ms(); stream_kernel(ci, g_samples, STREAM_N);
    void *fn = lookup(idx_stream, ci, &bucket); lat_first[0] = now_ms() - a; (void)fn;
    ejit_taskpool_release_read(bucket); }
  { double a = now_ms(); tiny(ci);
    void *fn = lookup(idx_tiny, ci, &bucket); lat_first[1] = now_ms() - a; (void)fn;
    ejit_taskpool_release_read(bucket); }
  ejit_drain_taskpool();

  if (mode_mem) {
    dump_code("compute_cell", ci);
    dump_code("compute_cell_ip", ci);
    dump_code("stream_kernel", ci);
    dump_code("tiny", ci);
    ejit_shutdown();
    return 0;
  }

  // warm specializations: more cells of the same entries, after the first.
  double lat_warm_stream[8], lat_warm_tiny[8];
  for (int k = 0; k < 8; k++) {
    uint8_t c = (uint8_t)(5 + k);
    g_scfg[c] = (struct StreamCfg){k & 1, 3 + k, 50, 0};
    g_tiny[c] = (struct Tiny){2000 + k, 2};
    if (ejit_activate("cell", c) != EJIT_OK) return 1;
    double a = now_ms(); stream_kernel(c, g_samples, STREAM_N);
    void *fn = lookup(idx_stream, c, &bucket); lat_warm_stream[k] = now_ms() - a; (void)fn;
    ejit_taskpool_release_read(bucket);
    a = now_ms(); tiny(c);
    fn = lookup(idx_tiny, c, &bucket); lat_warm_tiny[k] = now_ms() - a; (void)fn;
    ejit_taskpool_release_read(bucket);
  }

  // ---- 1. official micro kernel -------------------------------------------
  uint64_t r_pure = 0, r_jit = 0, r_fb = 0;
  double m_pure, m_jit, m_fb;
  for (int r = 0; r < REPS; r++) { double a = now_ms(); r_pure = compute_cell_nojit(ci); t[r] = now_ms() - a; }
  m_pure = median(t, REPS);
  for (int r = 0; r < REPS; r++) { double a = now_ms(); r_jit = compute_cell(ci); t[r] = now_ms() - a; }
  m_jit = median(t, REPS);
  ejit_deactivate("cell", ci);
  for (int r = 0; r < REPS; r++) { double a = now_ms(); r_fb = compute_cell(ci); t[r] = now_ms() - a; }
  m_fb = median(t, REPS);
  ejit_activate("cell", ci); ejit_drain_taskpool();

  // ---- 2. interprocedural ---------------------------------------------------
  uint64_t ip_pure = 0, ip_jit = 0;
  double mip_pure, mip_jit;
  for (int r = 0; r < REPS; r++) { double a = now_ms(); ip_pure = compute_cell_ip_nojit(ci); t[r] = now_ms() - a; }
  mip_pure = median(t, REPS);
  compute_cell_ip(ci); ejit_drain_taskpool();
  for (int r = 0; r < REPS; r++) { double a = now_ms(); ip_jit = compute_cell_ip(ci); t[r] = now_ms() - a; }
  mip_jit = median(t, REPS);

  // ---- 3. stream kernel (pointer varies so the compiler cannot hoist) ------
  int64_t s_pure = 0, s_jit = 0, s_fb = 0;
  double ms_pure, ms_jit, ms_fb;
  for (int r = 0; r < REPS; r++) {
    double a = now_ms();
    for (int k = 0; k < STREAM_REPEAT; k++) s_pure += stream_kernel_nojit(ci, g_samples + (k & 3), STREAM_N);
    t[r] = now_ms() - a;
  }
  ms_pure = median(t, REPS);
  stream_kernel(ci, g_samples, STREAM_N); ejit_drain_taskpool();
  for (int r = 0; r < REPS; r++) {
    double a = now_ms();
    for (int k = 0; k < STREAM_REPEAT; k++) s_jit += stream_kernel(ci, g_samples + (k & 3), STREAM_N);
    t[r] = now_ms() - a;
  }
  ms_jit = median(t, REPS);
  ejit_deactivate("cell", ci);
  for (int r = 0; r < REPS; r++) {
    double a = now_ms();
    for (int k = 0; k < STREAM_REPEAT; k++) s_fb += stream_kernel(ci, g_samples + (k & 3), STREAM_N);
    t[r] = now_ms() - a;
  }
  ms_fb = median(t, REPS);
  ejit_activate("cell", ci); ejit_drain_taskpool();

  // ---- 4. tiny per-call probe cost -----------------------------------------
  // Three populations: every call hits (all 16 cells specialized), every call
  // misses (nothing activated), and 1-in-8 hits.
  volatile int tsink = 0;
  for (int r = 0; r < REPS; r++) {
    double a = now_ms();
    for (uint64_t k = 0; k < TINY_CALLS; k++) tsink += tiny_nojit((uint8_t)(k & 15));
    t[r] = now_ms() - a;
  }
  double mt_pure = median(t, REPS);
  if (ejit_activate_all("cell") != EJIT_OK) return 1;
  for (int k = 0; k < N; k++) tiny((uint8_t)k);
  ejit_drain_taskpool();
  for (int k = 0; k < N; k++) tiny((uint8_t)k);   // second round: fill any missed
  ejit_drain_taskpool();
  for (int r = 0; r < REPS; r++) {
    double a = now_ms();
    for (uint64_t k = 0; k < TINY_CALLS; k++) tsink += tiny((uint8_t)(k & 15));
    t[r] = now_ms() - a;
  }
  double mt_hit = median(t, REPS);
  // one hot cell only: the indirect branch has a single target to learn
  ejit_deactivate_all("cell");
  if (ejit_activate("cell", ci) != EJIT_OK) return 1;
  tiny(ci); ejit_drain_taskpool(); tiny(ci); ejit_drain_taskpool();
  for (int r = 0; r < REPS; r++) {
    double a = now_ms();
    for (uint64_t k = 0; k < TINY_CALLS; k++) tsink += tiny(ci);
    t[r] = now_ms() - a;
  }
  double mt_hit1 = median(t, REPS);
  ejit_deactivate_all("cell");
  if (ejit_activate("cell", ci) != EJIT_OK) return 1;
  for (int r = 0; r < REPS; r++) {
    double a = now_ms();
    for (uint64_t k = 0; k < TINY_CALLS; k++) tsink += tiny((uint8_t)(k & 7));
    t[r] = now_ms() - a;
  }
  double mt_mixed = median(t, REPS);
  ejit_deactivate_all("cell");
  for (int r = 0; r < REPS; r++) {
    double a = now_ms();
    for (uint64_t k = 0; k < TINY_CALLS; k++) tsink += tiny((uint8_t)(k & 15));
    t[r] = now_ms() - a;
  }
  double mt_fb = median(t, REPS);

  ejit_taskpool_stats_t ts;
  memset(&ts, 0, sizeof ts);
  ejit_taskpool_get_stats(&ts);

  printf("RESULT micro  pure=%llu jit=%llu fb=%llu match=%d\n",
         (unsigned long long)r_pure, (unsigned long long)r_jit,
         (unsigned long long)r_fb, r_pure == r_jit && r_jit == r_fb);
  printf("RESULT ip     pure=%llu jit=%llu match=%d\n",
         (unsigned long long)ip_pure, (unsigned long long)ip_jit, ip_pure == ip_jit);
  printf("RESULT stream pure=%lld jit=%lld fb=%lld match=%d\n",
         (long long)s_pure, (long long)s_jit, (long long)s_fb,
         s_pure == s_jit && s_jit == s_fb);
  printf("TIME  micro_ms  pure=%.3f jit=%.5f fb=%.3f  reps=%d\n", m_pure, m_jit, m_fb, REPS);
  printf("TIME  ip_ms     pure=%.3f jit=%.3f\n", mip_pure, mip_jit);
  printf("TIME  stream_ms pure=%.3f jit=%.3f fb=%.3f  (ns/sample: pure=%.3f jit=%.3f fb=%.3f)\n",
         ms_pure, ms_jit, ms_fb,
         ms_pure * 1e6 / ((double)STREAM_REPEAT * STREAM_N),
         ms_jit * 1e6 / ((double)STREAM_REPEAT * STREAM_N),
         ms_fb * 1e6 / ((double)STREAM_REPEAT * STREAM_N));
  printf("TIME  tiny_ns   pure=%.3f hit1cell=%.3f hit16cells=%.3f mixed1of8=%.3f allmiss=%.3f\n",
         mt_pure * 1e6 / TINY_CALLS, mt_hit1 * 1e6 / TINY_CALLS, mt_hit * 1e6 / TINY_CALLS, mt_mixed * 1e6 / TINY_CALLS, mt_fb * 1e6 / TINY_CALLS);
  qsort(lat_warm_stream, 8, sizeof(double), cmpd); qsort(lat_warm_tiny, 8, sizeof(double), cmpd);
  printf("LATENCY_ms enqueue->published (poll 50us): first-in-process stream=%.2f tiny=%.2f | warm median stream=%.2f tiny=%.2f (min %.2f/%.2f max %.2f/%.2f)\n",
         lat_first[0], lat_first[1], lat_warm_stream[3], lat_warm_tiny[3], lat_warm_stream[0], lat_warm_tiny[0], lat_warm_stream[7], lat_warm_tiny[7]);
  printf("STATS compiles=%llu hits=%llu enq=%llu dedup=%llu failed=%llu\n",
         (unsigned long long)ts.asyncCompiles, (unsigned long long)ts.cacheHits,
         (unsigned long long)ts.asyncEnqueues,
         (unsigned long long)ts.alreadyPending,
         (unsigned long long)ts.compileFailed);
  (void)tsink;
  ejit_shutdown();
  return 0;
}
