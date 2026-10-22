// Standalone PR248 board smoke. Build with this revision's EJIT Clang/header.
// Fresh isolated image: core6 test_ejit_period, core16 test_ejit_period,
// core6 test_ejit_global_layout_print. All shell arguments are ignored.
//
// Manifest contract (2026-10-15): the manifest no longer has to cover every
// registered entry — unlisted functions compile through the ordinary paths and
// append after the manifest bodies at the collective point. This smoke
// deliberately lists ALL 26 roots so the strict A..Z address-order assertion
// stays exact; with a complete manifest unlistedReady/unlistedAppended must
// both stay zero. A FAILED layout no longer switches misses to fallback-only:
// every function keeps compiling normally, so poll
// ejit_get_global_layout_status as the health check to notice a lost layout.
#if !__has_attribute(ejit_entry)
#error "Use the matching EJIT Clang; ignoring unknown attributes is not valid"
#endif
#ifdef EJIT_DISABLE
#error "This smoke requires real EJIT annotations and generated wrappers"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#ifndef LAYOUT_SHARED
#define LAYOUT_SHARED __attribute__((section(".mc_shared")))
#endif
#ifndef LAYOUT_RUN_INIT_ARRAY
#define LAYOUT_RUN_INIT_ARRAY 1
#endif
#ifndef LAYOUT_WAIT_ROUNDS
#define LAYOUT_WAIT_ROUNDS 30000u
#endif
#ifndef LAYOUT_DUMP_IR
#define LAYOUT_DUMP_IR 1
#endif

extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

// EJitRuntime.h exposes layout states as uint32_t with a documented 0..9 ABI.
// These local names mirror that public contract, not private C++ structures.
enum {
  LAYOUT_DISABLED,
  LAYOUT_COLLECTING,
  LAYOUT_LINKING,
  LAYOUT_LINKED,
  LAYOUT_SEALING,
  LAYOUT_PUBLISHING,
  LAYOUT_COMPLETE,
  LAYOUT_FAILED,
  LAYOUT_CANCELLED,
  LAYOUT_REFRESHING
};
_Static_assert(LAYOUT_COMPLETE == 6 && LAYOUT_REFRESHING == 9,
               "Update local names if the public layout state ABI changes");
_Static_assert(sizeof(void *) == 8 && sizeof(ejit_config_t) == 56 &&
                   sizeof(ejit_global_layout_status_t) == 64 &&
                   sizeof(ejit_taskpool_stats_t) == 88,
               "Matching 64-bit ABI required");

#define LAYOUT_EACH(APPLY)                                                     \
  APPLY(A, 0)                                                                  \
  APPLY(B, 1) APPLY(C, 2) APPLY(D, 3) APPLY(E, 4) APPLY(F, 5) APPLY(G, 6)      \
      APPLY(H, 7) APPLY(I, 8) APPLY(J, 9) APPLY(K, 10) APPLY(L, 11)            \
          APPLY(M, 12) APPLY(N, 13) APPLY(O, 14) APPLY(P, 15) APPLY(Q, 16)     \
              APPLY(R, 17) APPLY(S, 18) APPLY(T, 19) APPLY(U, 20) APPLY(V, 21) \
                  APPLY(W, 22) APPLY(X, 23) APPLY(Y, 24) APPLY(Z, 25)
#define LAYOUT_COUNT 26u

static __attribute__((noinline)) uint32_t layout_business(uint32_t x,
                                                          uint32_t tag) {
  uint32_t value = x + tag;
  for (uint32_t i = 0; i < 16; ++i)
    if ((value ^ i) & 1u)
      value = value * 3u + i;
    else
      value += 7u + tag;
  return value;
}
#define LAYOUT_DEFINE(NAME, TAG)                                               \
  ejit_entry uint32_t global_layout_##NAME(uint32_t x) {                       \
    return layout_business(x, TAG);                                            \
  }
LAYOUT_EACH(LAYOUT_DEFINE)
typedef uint32_t (*layout_fn_t)(uint32_t);
#define LAYOUT_FN(NAME, TAG) global_layout_##NAME,
// Volatile forces an actual call boundary to the generated AOT wrapper.
static layout_fn_t volatile layout_entries[] = {LAYOUT_EACH(LAYOUT_FN)};
#define LAYOUT_NAME(NAME, TAG) "global_layout_" #NAME,
static const char *const layout_names[] = {LAYOUT_EACH(LAYOUT_NAME)};
static uint32_t layout_indices[LAYOUT_COUNT];

enum {
  DEMO_RESET,
  DEMO_STARTING,
  DEMO_READY,
  DEMO_RUNNING,
  DEMO_DONE,
  DEMO_FAILED
};
LAYOUT_SHARED uint32_t g_layout_stage;
LAYOUT_SHARED uint64_t g_layout_checked, g_layout_deferred;
LAYOUT_SHARED uint32_t g_layout_calls[LAYOUT_COUNT];
LAYOUT_SHARED uintptr_t g_layout_final[LAYOUT_COUNT];
LAYOUT_SHARED ejit_global_layout_status_t g_layout_result;

static int layout_fail(const char *why) {
  __atomic_store_n(&g_layout_stage, DEMO_FAILED, __ATOMIC_RELEASE);
  SRE_printf("[LAYOUT248] FAIL core=%u: %s; reset before rerun\n",
             g_ucLocalCoreID, why);
  return -1;
}
static int layout_delay(void) {
  return SRE_TaskDelay(1u) ? layout_fail("TaskDelay failed") : 0;
}
static int layout_status(ejit_global_layout_status_t *s,
                         ejit_taskpool_stats_t *t) {
  if (ejit_get_global_layout_status(s) != EJIT_OK ||
      ejit_taskpool_get_stats(t) != EJIT_OK)
    return layout_fail("status unavailable");
  if (s->state == LAYOUT_FAILED || s->state == LAYOUT_CANCELLED || s->error ||
      s->required != LAYOUT_COUNT || !s->generation || t->compileFailed ||
      t->publishFailed || t->queueFull)
    return layout_fail(
        "layout/compile/publish/queue failure; core6 print details");
  if (s->activeProfiles > 4u)
    return layout_fail("more than four PGO admissions");
  return 0;
}
static int layout_idle(void) {
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (layout_status(&s, &t))
    return -1;
  if (s.state != LAYOUT_COLLECTING || s.ready || s.published ||
      s.activeProfiles || t.asyncCompiles || t.readyEntries ||
      t.pendingEntries || t.queueApproxSize || ejit_taskpool_pending_count())
    return layout_fail("requires fresh idle image");
  return 0;
}

// A successful lookup MUST execute before release, even if order is wrong.
// Never inspect a T1 pointer and release without its real function return.
static int layout_call(uint32_t rank, uint32_t x, uintptr_t *seen) {
  void *body = 0;
  uint32_t token = 0;
  ejit_status_t rc =
      ejit_taskpool_compile_or_get(layout_indices[rank], 0, 0, &body, &token);
  *seen = 0;
  uint32_t actual;
  if (rc == EJIT_OK && body) {
    actual = ((layout_fn_t)body)(x);
    ejit_taskpool_release_read(token);
    *seen = (uintptr_t)body;
    ++g_layout_calls[rank];
  } else if ((rc == EJIT_PENDING || rc == EJIT_ERR_QUEUE_FULL) && !body) {
    ++g_layout_deferred;
    actual = layout_entries[rank](x);
  } else
    return layout_fail("lookup returned invalid status/pointer");
  if (actual != layout_business(x, rank))
    return layout_fail("business mismatch");
  ++g_layout_checked;
  return 0;
}

static int layout_drive(void) {
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (layout_status(&s, &t))
    return -1;
  uint32_t generation = s.generation;
  for (uint32_t round = 0; round < LAYOUT_WAIT_ROUNDS; ++round) {
    for (uint32_t j = 0; j < LAYOUT_COUNT; ++j) {
      uintptr_t ignored;
      if (layout_call((j + 7u) % LAYOUT_COUNT, round + 11u, &ignored))
        return -1;
    }
    if (layout_status(&s, &t))
      return -1;
    if (s.generation != generation)
      return layout_fail("unexpected generation change");
    if (s.state == LAYOUT_COMPLETE) {
      if (s.ready != LAYOUT_COUNT || s.published != LAYOUT_COUNT ||
          s.activeProfiles || t.asyncCompiles != 2u * LAYOUT_COUNT ||
          t.readyEntries != LAYOUT_COUNT) {
        SRE_printf("[LAYOUT248] final counts ready=%u published=%u active=%u "
                   "asyncCompiles=%llu (expected52) readyEntries=%u (expected26); "
                   "runtime --stats / EJIT_STATS_ENABLE=ON required\n",
                   s.ready, s.published, s.activeProfiles,
                   (unsigned long long)t.asyncCompiles, t.readyEntries);
        return layout_fail("incomplete T1/T2 final set");
      }
      // COMPLETE can precede draining obsolete queue/dedup tail requests.
      // Still require idle acceptance, but let the worker drain within budget.
      if (t.pendingEntries || t.queueApproxSize || ejit_taskpool_pending_count()) {
        if (layout_delay())
          return -1;
        continue;
      }
      uintptr_t previous = 0;
      for (uint32_t rank = 0; rank < LAYOUT_COUNT; ++rank) {
        uintptr_t body;
        if (layout_call(rank, 19u, &body))
          return -1;
        if (!body || body <= previous)
          return layout_fail("final bodies not A..Z");
        previous = g_layout_final[rank] = body;
        SRE_printf("[LAYOUT248] rank=%u name=%s finalBody=%p calls=%u\n", rank,
                   layout_names[rank], (void *)body, g_layout_calls[rank]);
      }
      if (layout_status(&s, &t))
        return -1;
      if (s.state != LAYOUT_COMPLETE || s.generation != generation ||
          s.ready != LAYOUT_COUNT || s.published != LAYOUT_COUNT)
        return layout_fail("layout changed during final execution");
      g_layout_result = s;
      return 0;
    }
    if (round % 100u == 0)
      SRE_printf(
          "[LAYOUT248] round=%u state=%u gen=%u ready=%u/%u published=%u "
          "active=%u/4 checked=%llu\n",
          round, s.state, s.generation, s.ready, s.required, s.published,
          s.activeProfiles, (unsigned long long)g_layout_checked);
    if (layout_delay())
      return -1;
  }
  return layout_fail(
      "bounded T2/idle convergence timeout; core6 print partial state");
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != 6u && g_ucLocalCoreID != 16u)
    return -1;
  uint32_t stage = g_ucLocalCoreID == 6u ? DEMO_RESET : DEMO_READY;
  uint32_t next = g_ucLocalCoreID == 6u ? DEMO_STARTING : DEMO_RUNNING;
  if (!__atomic_compare_exchange_n(&g_layout_stage, &stage, next, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[LAYOUT248] rejected core=%u stage=%u; no reinit/rerun\n",
               g_ucLocalCoreID, stage);
    return -1;
  }
#if LAYOUT_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
  // Core-local registration happens after each core's constructors, before
  // init.
  ejit_layout_target_t manifest[LAYOUT_COUNT] = {{0}};
  for (uint32_t rank = 0; rank < LAYOUT_COUNT; ++rank) {
    layout_indices[rank] = ~0u;
    ejit_register_funcindex(layout_names[rank], &layout_indices[rank]);
    if (layout_indices[rank] == ~0u)
      return layout_fail("unresolved public funcIndex");
    manifest[rank].entryName = layout_names[rank];
  }
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = cfg.forceStaticRegistry = true;
  // Always NORMAL online PGO, not audit-only and not argument-selected
  // Baseline.
  if (ejit_init_global_layout(&cfg, manifest, LAYOUT_COUNT, true) != EJIT_OK)
    return layout_fail("global-layout init/attachment failed");
  if (ejit_taskpool_get_worker_core() != 6u)
    return layout_fail("worker is not core6");
  if (layout_idle())
    return -1;
  if (g_ucLocalCoreID == 6u) {
#if LAYOUT_DUMP_IR
    ejit_dump_func("global_layout_H");
#endif
    __atomic_store_n(&g_layout_stage, DEMO_READY, __ATOMIC_RELEASE);
    SRE_printf("[LAYOUT248] WORKER_READY A..Z=26 normalPGO quota=64 cap=4; "
               "core16 test_ejit_period\n");
    return 0;
  }
  // The FIRST cold request must originate in the generated H wrapper, not a
  // hand-written compile_or_get probe. Volatile prevents bypassing that entry.
  if (layout_entries[7](11u) != layout_business(11u, 7u))
    return layout_fail("cold H wrapper business mismatch");
  ++g_layout_checked;
  if (layout_drive())
    return -1;
  __atomic_store_n(&g_layout_stage, DEMO_DONE, __ATOMIC_RELEASE);
  SRE_printf("[LAYOUT248] OWNER_DONE gen=%u checked=%llu deferred=%llu; "
             "core6 test_ejit_global_layout_print\n",
             g_layout_result.generation, (unsigned long long)g_layout_checked,
             (unsigned long long)g_layout_deferred);
  return 0;
}

// Read-only: no constructor/init/refresh/lookup/activation/shutdown call.
int test_ejit_global_layout_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != 6u)
    return -1;
  uint32_t stage = __atomic_load_n(&g_layout_stage, __ATOMIC_ACQUIRE);
  if (stage != DEMO_DONE && stage != DEMO_FAILED)
    return -1;
  ejit_taskpool_print_stats();
  ejit_taskpool_print_compiled();
  ejit_print_code_pool_stats();
#if LAYOUT_DUMP_IR
  ejit_print_dumped("global_layout_H");
  ejit_print_dumped_module("global_layout_H");
#endif
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (ejit_get_global_layout_status(&s) != EJIT_OK ||
      ejit_taskpool_get_stats(&t) != EJIT_OK)
    return -1;
  SRE_printf("[LAYOUT248] state=%u gen=%u required=%u ready=%u published=%u "
             "error=%u rank=%u active=%u unlistedReady=%u unlistedAppended=%u "
             "near=%llu far=%llu\n",
             s.state, s.generation, s.required, s.ready, s.published, s.error,
             s.errorRank, s.activeProfiles, s.unlistedReady, s.unlistedAppended,
             (unsigned long long)s.retainedNearBytes,
             (unsigned long long)s.retainedFarBytes);
  if (stage != DEMO_DONE || s.state != LAYOUT_COMPLETE || s.error ||
      s.generation != g_layout_result.generation ||
      s.required != LAYOUT_COUNT || s.ready != LAYOUT_COUNT ||
      s.published != LAYOUT_COUNT || s.activeProfiles || s.unlistedReady ||
      s.unlistedAppended || t.compileFailed ||
      t.publishFailed || t.queueFull || t.pendingEntries || t.queueApproxSize ||
      t.asyncCompiles != 2u * LAYOUT_COUNT || t.readyEntries != LAYOUT_COUNT ||
      ejit_taskpool_pending_count())
    return -1;
  for (uint32_t rank = 0; rank < LAYOUT_COUNT; ++rank)
    if (!g_layout_final[rank] || !g_layout_calls[rank] ||
        (rank && g_layout_final[rank] <= g_layout_final[rank - 1]))
      return -1;
  SRE_printf("[LAYOUT248] RUNTIME_CHECKS_PASS business+26 final ordered bodies "
             "T1+T2=52; NOT BOARD_ACCEPTED until worker PGO64/cap4/merge/"
             "preparation evidence is checked\n");
  return 0;
}
