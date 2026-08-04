//===-- ejit_icache_multiverify_test.c ------------------------------------===//
//
// EJIT multi-version inline-cache board test (multi-core, async shared pool).
//
// Purpose:
//   1. Functional correctness: for each numDims N=0..4, verify the per-function
//      [D]^numDims icache serves the CORRECT specialization per dim identity
//      (the v2 monomorphic-scalar bug -- one slot served the first identity for
//      all calls -- is caught: warming identity A then calling identity B must
//      return B's result, not A's).
//   2. Per-dimension hit overhead: built with -ejit-wrapper-timing, the wrapper
//      auto-prints (via EJIT_DIAG -> SRE_printf on the board) one
//      `wrapper_timing_agg func=<N> status=254 ... get_fn_avg=... fn_call_avg=...
//      total_avg=...` report per 100000 icache hits. `get_fn_avg` is the pure
//      pointer-fetch cost (GEP + ldar), independent of the specialization body
//      and growing with N; `total_avg` is wrapper + specialization body. The
//      test only generates hits -- no manual timing/statistics.
//
// Build (add to ejit_test/build.sh by the integrator; NOT done here):
//   COMPILE_FLAGS[ejit_icache_multiverify_test]="-mllvm -ejit-inline-cache \
//       -mllvm -ejit-wrapper-timing -mllvm -enable-ejit-global-ctors=false"
//   LINKER_SCRIPT[ejit_icache_multiverify_test]=ejit_baremetal.ld
// Requires the aarch64_be preset (async + shared pool + cross-code-ptrs +
// EJIT_DIAG_ENABLE + EJIT_SRE_DIAG) and the multi-version icache runtime.
//
// Entry: int test_ejit_icache_multiverify(uint8_t,uint8_t,uint8_t,uint8_t)
// (args ignored), called by the RTOS on EVERY core. The first core to reach
// ejit_init is elected the compile worker and spins idle; every other core
// (non-worker) runs the correctness checks + the hit loop. Each non-worker
// core owns its private icache and its private wrapper-timing slots, so each
// prints its own per-f_N report.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "ejit_test_helpers.h" /* API + ejit_default_config + ejit_drain_taskpool */

//===-- SRE bare-metal platform externs (host libc does not provide) ------===//
extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

//===-- EJIT_SHARED_SECTION_ATTR (.mc_shared cross-core shared) -----------===//
// The compile worker runs on a DIFFERENT core than the non-worker verifier
// that runs init_data; default-BSS globals are per-core-private, so the worker
// would read BSS-zero and fold stale values. ALL data/period arrays here are
// marked shared. (CMake passes the real -DEJIT_SHARED_SECTION_ATTR; this is the
// host fallback and MUST default to the real section, not empty.)
#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

//===-- Constants ---------------------------------------------------------===//
#define COMPILE_WAIT_ROUNDS 200u
#define COMPILE_WAIT_TICKS 10u
#define IDLE_DELAY_TICKS 40000u
#define WRAPPER_TIMING_HITS 100000u /* = EJIT_WRAPPER_TIMING_REPORT_EVERY */

//===-- Helpers -----------------------------------------------------------===//

static inline uint32_t local_core_id(void) { return (uint32_t)g_ucLocalCoreID; }

// Poll until the worker has published (pending==0 && readyEntries!=0), bounded.
static inline int wait_for_compile(uint32_t core) {
  for (uint32_t round = 0; round < COMPILE_WAIT_ROUNDS; ++round) {
    if (ejit_taskpool_pending_count() == 0) {
      ejit_taskpool_stats_t st;
      ejit_taskpool_get_stats(&st);
      if (st.readyEntries != 0)
        return 1;
    }
    SRE_TaskDelay(COMPILE_WAIT_TICKS);
  }
  SRE_printf("[ICACHE][core=%u] FAIL: compile wait timeout\n", core);
  return 0;
}

static inline void idle_forever(uint32_t core, const char *role) {
  for (;;) {
    SRE_TaskDelay(IDLE_DELAY_TICKS);
    SRE_printf("[ICACHE][core=%u] idle role=%s\n", core, role);
  }
}

#define VERIFY(core, label, cond, fmt, ...)                                     \
  do {                                                                          \
    if (cond)                                                                    \
      SRE_printf("[ICACHE][core=%u] OK:   %s " fmt "\n", core, label,           \
                 ##__VA_ARGS__);                                                 \
    else                                                                         \
      SRE_printf("[ICACHE][core=%u] FAIL: %s " fmt "\n", core, label,           \
                 ##__VA_ARGS__);                                                 \
  } while (0)

// Warm one identity: first call dispatches the async compile (AOT fallback),
// wait_for_compile lets the worker publish, second call is a taskpool cache hit
// that fills this core's icache cell and returns the JIT (baked) result.
#define WARM(core, label, call_expr, expected)                                  \
  do {                                                                          \
    (void)(call_expr); /* PENDING -> AOT fallback */                            \
    wait_for_compile(core);                                                      \
    uint32_t _r = (call_expr); /* taskpool hit -> fill icache -> JIT result */  \
    VERIFY(core, label, _r == (expected), "got=%u", _r);                        \
  } while (0)

//===-- Data (all .mc_shared) ---------------------------------------------===//

struct Cfg {
  ejit_may_const uint32_t cellType;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(p0) struct Cfg g_p0[2];
EJIT_SHARED_SECTION_ATTR ejit_period_arr(p1) struct Cfg g_p1[2];
EJIT_SHARED_SECTION_ATTR ejit_period_arr(p2) struct Cfg g_p2[2];
EJIT_SHARED_SECTION_ATTR ejit_period_arr(p3) struct Cfg g_p3[2];
EJIT_SHARED_SECTION_ATTR ejit_may_const uint32_t g_c0 = 0xFDu;

static void init_data(void) {
  /* instance 0 -> 0xFD, instance 1 -> 0xEC, for every period array. */
  g_p0[0].cellType = 0xFDu;
  g_p0[1].cellType = 0xECu;
  g_p1[0].cellType = 0xFDu;
  g_p1[1].cellType = 0xECu;
  g_p2[0].cellType = 0xFDu;
  g_p2[1].cellType = 0xECu;
  g_p3[0].cellType = 0xFDu;
  g_p3[1].cellType = 0xECu;
}

//===-- ejit_entry functions (numDims 0..4) -------------------------------===//
// f_N packs N "is instance 0 (0xFD)" bits. Identity A=(0,..,0) -> 2^N - 1;
// identity B=(1,..,1) -> 0. The result depends on ALL N dims, so the icache is
// exercised per full identity (multi-version keying). f_0 is monomorphic
// (0-dim scalar slot = v2 baseline).
//
// __attribute__((noinline)) prevents the wrapper dispatch logic from being
// inlined into callers.

ejit_entry __attribute__((noinline)) uint32_t f_0(void) {
  return (g_c0 == 0xFDu) ? 1u : 0u;
}

ejit_entry __attribute__((noinline)) uint32_t
f_1(ejit_period_arr_ind(p0) uint8_t i0) {
  return (g_p0[i0].cellType == 0xFDu) ? 1u : 0u;
}

ejit_entry __attribute__((noinline)) uint32_t
f_2(ejit_period_arr_ind(p0) uint8_t i0, ejit_period_arr_ind(p1) uint8_t i1) {
  uint32_t r = 0;
  r = (r << 1) | (g_p0[i0].cellType == 0xFDu);
  r = (r << 1) | (g_p1[i1].cellType == 0xFDu);
  return r;
}

ejit_entry __attribute__((noinline)) uint32_t
f_3(ejit_period_arr_ind(p0) uint8_t i0, ejit_period_arr_ind(p1) uint8_t i1,
    ejit_period_arr_ind(p2) uint8_t i2) {
  uint32_t r = 0;
  r = (r << 1) | (g_p0[i0].cellType == 0xFDu);
  r = (r << 1) | (g_p1[i1].cellType == 0xFDu);
  r = (r << 1) | (g_p2[i2].cellType == 0xFDu);
  return r;
}

ejit_entry __attribute__((noinline)) uint32_t
f_4(ejit_period_arr_ind(p0) uint8_t i0, ejit_period_arr_ind(p1) uint8_t i1,
    ejit_period_arr_ind(p2) uint8_t i2, ejit_period_arr_ind(p3) uint8_t i3) {
  uint32_t r = 0;
  r = (r << 1) | (g_p0[i0].cellType == 0xFDu);
  r = (r << 1) | (g_p1[i1].cellType == 0xFDu);
  r = (r << 1) | (g_p2[i2].cellType == 0xFDu);
  r = (r << 1) | (g_p3[i3].cellType == 0xFDu);
  return r;
}

//===-- Entry: called by the RTOS on every core ---------------------------===//
int test_ejit_icache_multiverify(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = local_core_id();
  SRE_printf("\n=== EJIT icache multi-version verify (core=%u) ===\n", core);

  call_init_array_functions();

  ejit_config_t cfg = {.compileMode = EJIT_COMPILE_ASYNC,
                       .optLevel = EJIT_OPT_L2,
                       .enableLogger = false,
                       .forceStaticRegistry = true};
  ejit_status_t rc = ejit_init(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[ICACHE][core=%u] init rc=%d worker=%u\n", core, (int)rc, worker);
  if (rc != EJIT_OK)
    idle_forever(core, "init-failed");

  /* The elected worker core only serves compiles. */
  if (core == worker)
    idle_forever(core, "worker");

  /*--- Non-worker core: set up shared data, activate, verify, measure. ---*/
  init_data();
  ejit_activate("p0", 0);
  ejit_activate("p0", 1);
  ejit_activate("p1", 0);
  ejit_activate("p1", 1);
  ejit_activate("p2", 0);
  ejit_activate("p2", 1);
  ejit_activate("p3", 0);
  ejit_activate("p3", 1);

  /*=== f_0: 0-dim monomorphic baseline (no A/B) ===*/
  WARM(core, "f_0 warm==1", f_0(), 1u);
  SRE_printf("[ICACHE][core=%u] f_0 hit loop (%u hits)\n", core,
             WRAPPER_TIMING_HITS);
  for (uint32_t k = 0; k < WRAPPER_TIMING_HITS; ++k)
    (void)f_0();

  /*=== f_1: 1-dim. A=(0)->1, B=(1)->0. warm(B)==0 is the v2-bug catcher. ===*/
  WARM(core, "f_1 warm(A)==1", f_1(0), 1u);
  WARM(core, "f_1 warm(B)==0", f_1(1), 0u);
  {
    uint32_t rA = f_1(0);
    VERIFY(core, "f_1 hit(A)==1", rA == 1u, "got=%u", rA);
    uint32_t rB = f_1(1);
    VERIFY(core, "f_1 hit(B)==0", rB == 0u, "got=%u", rB);
  }
  SRE_printf("[ICACHE][core=%u] f_1 hit loop (%u hits)\n", core,
             WRAPPER_TIMING_HITS);
  for (uint32_t k = 0; k < WRAPPER_TIMING_HITS; ++k)
    (void)f_1(0);

  /*=== f_2: 2-dim. A=(0,0)->3, B=(1,1)->0. ===*/
  WARM(core, "f_2 warm(A)==3", f_2(0, 0), 3u);
  WARM(core, "f_2 warm(B)==0", f_2(1, 1), 0u);
  {
    uint32_t rA = f_2(0, 0);
    VERIFY(core, "f_2 hit(A)==3", rA == 3u, "got=%u", rA);
    uint32_t rB = f_2(1, 1);
    VERIFY(core, "f_2 hit(B)==0", rB == 0u, "got=%u", rB);
  }
  SRE_printf("[ICACHE][core=%u] f_2 hit loop (%u hits)\n", core,
             WRAPPER_TIMING_HITS);
  for (uint32_t k = 0; k < WRAPPER_TIMING_HITS; ++k)
    (void)f_2(0, 0);

  /*=== f_3: 3-dim. A=(0,0,0)->7, B=(1,1,1)->0. ===*/
  WARM(core, "f_3 warm(A)==7", f_3(0, 0, 0), 7u);
  WARM(core, "f_3 warm(B)==0", f_3(1, 1, 1), 0u);
  {
    uint32_t rA = f_3(0, 0, 0);
    VERIFY(core, "f_3 hit(A)==7", rA == 7u, "got=%u", rA);
    uint32_t rB = f_3(1, 1, 1);
    VERIFY(core, "f_3 hit(B)==0", rB == 0u, "got=%u", rB);
  }
  SRE_printf("[ICACHE][core=%u] f_3 hit loop (%u hits)\n", core,
             WRAPPER_TIMING_HITS);
  for (uint32_t k = 0; k < WRAPPER_TIMING_HITS; ++k)
    (void)f_3(0, 0, 0);

  /*=== f_4: 4-dim. A=(0,0,0,0)->15, B=(1,1,1,1)->0. ===*/
  WARM(core, "f_4 warm(A)==15", f_4(0, 0, 0, 0), 15u);
  WARM(core, "f_4 warm(B)==0", f_4(1, 1, 1, 1), 0u);
  {
    uint32_t rA = f_4(0, 0, 0, 0);
    VERIFY(core, "f_4 hit(A)==15", rA == 15u, "got=%u", rA);
    uint32_t rB = f_4(1, 1, 1, 1);
    VERIFY(core, "f_4 hit(B)==0", rB == 0u, "got=%u", rB);
  }
  SRE_printf("[ICACHE][core=%u] f_4 hit loop (%u hits)\n", core,
             WRAPPER_TIMING_HITS);
  for (uint32_t k = 0; k < WRAPPER_TIMING_HITS; ++k)
    (void)f_4(0, 0, 0, 0);

  /*=== stats ===*/
  ejit_taskpool_stats_t st;
  memset(&st, 0, sizeof(st));
  ejit_taskpool_get_stats(&st);
  VERIFY(core, "compileFailed==0", st.compileFailed == 0, "compileFailed=%llu",
         (unsigned long long)st.compileFailed);
  SRE_printf("[ICACHE][core=%u] stats: ready=%u compiles=%llu hits=%llu\n",
             core, st.readyEntries, (unsigned long long)st.asyncCompiles,
             (unsigned long long)st.cacheHits);

  idle_forever(core, "benchmark-complete");
  return 0;
}
