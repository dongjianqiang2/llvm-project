// Standalone bare-metal smoke for the unlisted-functions global-layout
// contract (2026-10-15). Build with this revision's EJIT Clang and the
// matching llvm/ExecutionEngine/EJIT/EJitRuntime.h. No test helper header.
// Fresh isolated image (same shell entry as the other layout demos — never
// link them together):
//   core6:  test_ejit_period          (worker)
//   core16: test_ejit_period          (producer/owner)
//   core6:  test_ejit_unlisted_print  (read-only evidence)
// All shell arguments are ignored.
//
// SPEC (this header is authoritative):
// - PGO ON (quota 64, four admissions), async shared taskpool, worker core 6,
//   producer core 16; override with -DUNLISTED_WORKER_CORE=<n> /
//   -DUNLISTED_PRODUCER_CORE=<n> only before a reset image build.
// - 11 ejit_entry roots: A..H form the manifest (8 members, rank A..H); X, Y,
//   Z are registered but NOT listed. Initialization must succeed with the
//   incomplete manifest; X/Y/Z compile as unlisted functions.
// - The producer calls X, Y, Z FIRST every round, so their armed Tier-2
//   requests enqueue (and park) before any member's Tier-2 is even popped:
//   the collective window deterministically includes them.
// - FALSIFIABLE core criterion: after COMPLETE, final body addresses are
//   strictly increasing A..H AND every unlisted body is ABOVE the highest
//   manifest body — unlisted functions append after the manifest region.
//   Additionally unlistedAppended == 3, unlistedReady was observed non-zero
//   during collection and is zero at the end, and every body executes its
//   business correctly. A regression to "unlisted request fails the layout"
//   turns this red at initialization; a lost append turns it red at the
//   address/unlistedAppended checks.
// - Gates use ONLY API return values (status/stats); serial prints are
//   evidence, never the verdict. A FAILED layout keeps the system running on
//   ordinary fallback — poll ejit_get_global_layout_status as the health
//   check to notice a lost layout.
// - No shutdown anywhere: worker and peer must survive across shell calls.
#if !__has_attribute(ejit_entry)
#error "Use the matching EJIT Clang; ignoring unknown attributes is not valid"
#endif
#ifdef EJIT_DISABLE
#error "This smoke requires real EJIT annotations and generated wrappers"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#ifndef UNLISTED_SHARED
#define UNLISTED_SHARED __attribute__((section(".mc_shared")))
#endif
#ifndef UNLISTED_WORKER_CORE
#define UNLISTED_WORKER_CORE 6u
#endif
#ifndef UNLISTED_PRODUCER_CORE
#define UNLISTED_PRODUCER_CORE 16u
#endif
#ifndef UNLISTED_RUN_INIT_ARRAY
#define UNLISTED_RUN_INIT_ARRAY 1
#endif
#ifndef UNLISTED_WAIT_ROUNDS
// No deadline on board by default; a positive override bounds automated runs.
#define UNLISTED_WAIT_ROUNDS 0u
#endif
#ifndef UNLISTED_DUMP_IR
#define UNLISTED_DUMP_IR 1
#endif

extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

// EJitRuntime.h exposes layout states as uint32_t with a documented 0..9 ABI.
enum {
  UNLISTED_DISABLED,
  UNLISTED_COLLECTING,
  UNLISTED_LINKING,
  UNLISTED_LINKED,
  UNLISTED_SEALING,
  UNLISTED_PUBLISHING,
  UNLISTED_COMPLETE,
  UNLISTED_FAILED,
  UNLISTED_CANCELLED,
  UNLISTED_REFRESHING
};
_Static_assert(UNLISTED_COMPLETE == 6 && UNLISTED_REFRESHING == 9,
               "Update local names if the public layout state ABI changes");
_Static_assert(sizeof(void *) == 8 && sizeof(ejit_config_t) == 56 &&
                   sizeof(ejit_global_layout_status_t) == 64 &&
                   sizeof(ejit_taskpool_stats_t) == 88,
               "Matching 64-bit ABI required");

#define UNLISTED_LISTED(APPLY)                                                 \
  APPLY(A, 0)                                                                  \
  APPLY(B, 1) APPLY(C, 2) APPLY(D, 3) APPLY(E, 4) APPLY(F, 5) APPLY(G, 6)      \
      APPLY(H, 7)
#define UNLISTED_EXTRA(APPLY) APPLY(X, 8) APPLY(Y, 9) APPLY(Z, 10)
#define UNLISTED_EACH(APPLY) UNLISTED_LISTED(APPLY) UNLISTED_EXTRA(APPLY)
#define UNLISTED_MEMBERS 8u
#define UNLISTED_COUNT 11u

static __attribute__((noinline)) uint32_t unlisted_business(uint32_t x,
                                                            uint32_t tag) {
  uint32_t value = x + tag;
  for (uint32_t i = 0; i < 16; ++i)
    if ((value ^ i) & 1u)
      value = value * 3u + i;
    else
      value += 7u + tag;
  return value;
}
#define UNLISTED_DEFINE(NAME, TAG)                                             \
  ejit_entry uint32_t unlisted_##NAME(uint32_t x) {                            \
    return unlisted_business(x, TAG);                                          \
  }
UNLISTED_EACH(UNLISTED_DEFINE)
typedef uint32_t (*unlisted_fn_t)(uint32_t);
#define UNLISTED_FN(NAME, TAG) unlisted_##NAME,
// Volatile forces an actual call boundary to the generated AOT wrapper.
static unlisted_fn_t volatile unlisted_entries[] = {UNLISTED_EACH(UNLISTED_FN)};
#define UNLISTED_NAME(NAME, TAG) "unlisted_" #NAME,
static const char *const unlisted_names[] = {UNLISTED_EACH(UNLISTED_NAME)};
static uint32_t unlisted_indices[UNLISTED_COUNT];

enum {
  DEMO_RESET,
  DEMO_STARTING,
  DEMO_READY,
  DEMO_RUNNING,
  DEMO_DONE,
  DEMO_FAILED
};
UNLISTED_SHARED uint32_t g_unlisted_stage;
UNLISTED_SHARED uint64_t g_unlisted_checked, g_unlisted_deferred;
UNLISTED_SHARED uint32_t g_unlisted_calls[UNLISTED_COUNT];
UNLISTED_SHARED uintptr_t g_unlisted_final[UNLISTED_COUNT];
UNLISTED_SHARED ejit_global_layout_status_t g_unlisted_result;
UNLISTED_SHARED uint32_t g_unlisted_sawParked;

static int unlisted_fail(const char *why) {
  __atomic_store_n(&g_unlisted_stage, DEMO_FAILED, __ATOMIC_RELEASE);
  SRE_printf("[UNLISTED] FAIL core=%u: %s; reset before rerun\n",
             g_ucLocalCoreID, why);
  return -1;
}
static int unlisted_delay(void) {
  return SRE_TaskDelay(1u) ? unlisted_fail("TaskDelay failed") : 0;
}
static int unlisted_status(ejit_global_layout_status_t *s,
                           ejit_taskpool_stats_t *t) {
  if (ejit_get_global_layout_status(s) != EJIT_OK ||
      ejit_taskpool_get_stats(t) != EJIT_OK)
    return unlisted_fail("status unavailable");
  if (s->state == UNLISTED_FAILED || s->state == UNLISTED_CANCELLED ||
      s->error || s->required != UNLISTED_MEMBERS || !s->generation ||
      t->compileFailed || t->publishFailed || t->queueFull)
    return unlisted_fail(
        "layout/compile/publish/queue failure; core6 print details");
  if (s->activeProfiles > 4u)
    return unlisted_fail("more than four PGO admissions");
  return 0;
}
static int unlisted_idle(void) {
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (unlisted_status(&s, &t))
    return -1;
  if (s.state != UNLISTED_COLLECTING || s.ready || s.published ||
      s.activeProfiles || t.asyncCompiles || t.readyEntries ||
      t.pendingEntries || t.queueApproxSize || ejit_taskpool_pending_count())
    return unlisted_fail("requires fresh idle image");
  return 0;
}

// A successful lookup MUST execute before release, even if order is wrong.
// Never inspect a T1 pointer and release without its real function return.
static int unlisted_call(uint32_t slot, uint32_t x, uintptr_t *seen) {
  void *body = 0;
  uint32_t token = 0;
  ejit_status_t rc = ejit_taskpool_compile_or_get(unlisted_indices[slot], 0, 0,
                                                  &body, &token);
  *seen = 0;
  uint32_t actual;
  if (rc == EJIT_OK && body) {
    actual = ((unlisted_fn_t)body)(x);
    ejit_taskpool_release_read(token);
    *seen = (uintptr_t)body;
    ++g_unlisted_calls[slot];
  } else if ((rc == EJIT_PENDING || rc == EJIT_ERR_QUEUE_FULL) && !body) {
    ++g_unlisted_deferred;
    actual = unlisted_entries[slot](x);
  } else
    return unlisted_fail("lookup returned invalid status/pointer");
  if (actual != unlisted_business(x, slot))
    return unlisted_fail("business mismatch");
  ++g_unlisted_checked;
  return 0;
}

static int unlisted_drive(void) {
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (unlisted_status(&s, &t))
    return -1;
  uint32_t generation = s.generation;
  for (uint32_t round = 0;
       !UNLISTED_WAIT_ROUNDS || round < UNLISTED_WAIT_ROUNDS; ++round) {
    // Unlisted first: X/Y/Z hit counts (and therefore Tier-2 arms) lead the
    // members every round, so their parked requests are in place before the
    // last member stages and the collective window includes them.
    for (uint32_t j = 0; j < UNLISTED_COUNT - UNLISTED_MEMBERS; ++j) {
      uintptr_t ignored;
      if (unlisted_call(UNLISTED_MEMBERS + j, round + 11u, &ignored))
        return -1;
    }
    for (uint32_t j = 0; j < UNLISTED_MEMBERS; ++j) {
      uintptr_t ignored;
      if (unlisted_call((j + 5u) % UNLISTED_MEMBERS, round + 11u, &ignored))
        return -1;
    }
    if (unlisted_status(&s, &t))
      return -1;
    if (s.generation != generation)
      return unlisted_fail("unexpected generation change");
    if (s.state == UNLISTED_COLLECTING && s.unlistedReady)
      __atomic_store_n(&g_unlisted_sawParked, 1u, __ATOMIC_RELEASE);
    if (s.state == UNLISTED_COMPLETE) {
      // The manifest published; the appended unlisted bodies publish through
      // the ordinary auto/flush path after COMPLETE. Wait for the queue and
      // dedup tail to drain and every publication to land.
      for (uint32_t wait = 0;
           !UNLISTED_WAIT_ROUNDS || wait < UNLISTED_WAIT_ROUNDS; ++wait) {
        if (unlisted_status(&s, &t))
          return -1;
        if (s.state != UNLISTED_COMPLETE || s.generation != generation)
          return unlisted_fail("layout changed after completion");
        if (!t.pendingEntries && !t.queueApproxSize &&
            !ejit_taskpool_pending_count() &&
            t.asyncCompiles >= 2u * UNLISTED_COUNT &&
            t.readyEntries >= UNLISTED_COUNT)
          break;
        if (wait % 500u == 0u)
          SRE_printf("[UNLISTED] drain wait=%u async=%llu ready=%u\n", wait,
                     (unsigned long long)t.asyncCompiles, t.readyEntries);
        if (unlisted_delay())
          return -1;
      }
      if (s.unlistedReady ||
          s.unlistedAppended != UNLISTED_COUNT - UNLISTED_MEMBERS)
        return unlisted_fail("unlisted park/append accounting mismatch");
      if (!__atomic_load_n(&g_unlisted_sawParked, __ATOMIC_ACQUIRE))
        return unlisted_fail("unlisted misses never parked during collection");
      uintptr_t previous = 0;
      for (uint32_t rank = 0; rank < UNLISTED_MEMBERS; ++rank) {
        uintptr_t body;
        if (unlisted_call(rank, 19u, &body))
          return -1;
        if (!body || body <= previous)
          return unlisted_fail("manifest bodies not A..H");
        previous = g_unlisted_final[rank] = body;
        SRE_printf("[UNLISTED] rank=%u name=%s finalBody=%p calls=%u\n", rank,
                   unlisted_names[rank], (void *)body, g_unlisted_calls[rank]);
      }
      for (uint32_t j = 0; j < UNLISTED_COUNT - UNLISTED_MEMBERS; ++j) {
        uintptr_t body;
        if (unlisted_call(UNLISTED_MEMBERS + j, 19u, &body))
          return -1;
        if (!body || body <= previous)
          return unlisted_fail("unlisted body not appended after the manifest");
        previous = g_unlisted_final[UNLISTED_MEMBERS + j] = body;
        SRE_printf("[UNLISTED] unlisted name=%s finalBody=%p calls=%u\n",
                   unlisted_names[UNLISTED_MEMBERS + j], (void *)body,
                   g_unlisted_calls[UNLISTED_MEMBERS + j]);
      }
      if (unlisted_status(&s, &t))
        return -1;
      if (s.state != UNLISTED_COMPLETE || s.generation != generation ||
          s.ready != UNLISTED_MEMBERS || s.published != UNLISTED_MEMBERS)
        return unlisted_fail("layout changed during final execution");
      g_unlisted_result = s;
      return 0;
    }
    if (round % 100u == 0u)
      SRE_printf("[UNLISTED] round=%u state=%u gen=%u ready=%u/%u "
                 "unlistedReady=%u checked=%llu\n",
                 round, s.state, s.generation, s.ready, s.required,
                 s.unlistedReady, (unsigned long long)g_unlisted_checked);
    if (unlisted_delay())
      return -1;
  }
  return unlisted_fail(
      "bounded convergence timeout; core6 print partial state");
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != UNLISTED_WORKER_CORE &&
      g_ucLocalCoreID != UNLISTED_PRODUCER_CORE)
    return -1;
  uint32_t stage =
      g_ucLocalCoreID == UNLISTED_WORKER_CORE ? DEMO_RESET : DEMO_READY;
  uint32_t next = g_ucLocalCoreID == UNLISTED_WORKER_CORE ? DEMO_STARTING
                                                          : DEMO_RUNNING;
  if (!__atomic_compare_exchange_n(&g_unlisted_stage, &stage, next, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[UNLISTED] rejected core=%u stage=%u; no reinit/rerun\n",
               g_ucLocalCoreID, stage);
    return -1;
  }
#if UNLISTED_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
  // Core-local registration happens after each core's constructors, before
  // init. ALL 11 roots register; the manifest deliberately lists only the 8
  // members A..H — X/Y/Z stay outside it.
  ejit_layout_target_t manifest[UNLISTED_MEMBERS] = {{0}};
  for (uint32_t slot = 0; slot < UNLISTED_COUNT; ++slot) {
    unlisted_indices[slot] = ~0u;
    ejit_register_funcindex(unlisted_names[slot], &unlisted_indices[slot]);
    if (unlisted_indices[slot] == ~0u)
      return unlisted_fail("unresolved public funcIndex");
    if (slot < UNLISTED_MEMBERS)
      manifest[slot].entryName = unlisted_names[slot];
  }
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = cfg.forceStaticRegistry = true;
  // Normal online PGO with the incomplete manifest: this call itself is the
  // headline check — it must NOT be rejected for missing coverage.
  if (ejit_init_global_layout(&cfg, manifest, UNLISTED_MEMBERS, true) !=
      EJIT_OK)
    return unlisted_fail("incomplete-manifest init/attachment failed");
  if (ejit_taskpool_get_worker_core() != UNLISTED_WORKER_CORE)
    return unlisted_fail("worker is not the configured core");
  if (unlisted_idle())
    return -1;
  if (g_ucLocalCoreID == UNLISTED_WORKER_CORE) {
#if UNLISTED_DUMP_IR
    // Capture an UNLISTED function's object-level evidence: its ordinary-path
    // compilation must still produce a real specialized module.
    ejit_dump_func("unlisted_X");
#endif
    __atomic_store_n(&g_unlisted_stage, DEMO_READY, __ATOMIC_RELEASE);
    SRE_printf("[UNLISTED] WORKER_READY members=8 unlisted=3 normalPGO "
               "quota=64 cap=4; producer test_ejit_period\n");
    return 0;
  }
  // The FIRST cold request must originate in a generated wrapper, not a
  // hand-written compile_or_get probe; X is an unlisted function, so its
  // first miss takes the ordinary Tier-1 path.
  if (unlisted_entries[UNLISTED_MEMBERS](11u) !=
      unlisted_business(11u, UNLISTED_MEMBERS))
    return unlisted_fail("cold X wrapper business mismatch");
  ++g_unlisted_checked;
  if (unlisted_drive())
    return -1;
  __atomic_store_n(&g_unlisted_stage, DEMO_DONE, __ATOMIC_RELEASE);
  SRE_printf("[UNLISTED] OWNER_DONE gen=%u unlistedAppended=%u checked=%llu "
             "deferred=%llu; worker core test_ejit_unlisted_print\n",
             g_unlisted_result.generation, g_unlisted_result.unlistedAppended,
             (unsigned long long)g_unlisted_checked,
             (unsigned long long)g_unlisted_deferred);
  return 0;
}

// Read-only: no constructor/init/refresh/lookup/activation/shutdown call.
int test_ejit_unlisted_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != UNLISTED_WORKER_CORE)
    return -1;
  uint32_t stage = __atomic_load_n(&g_unlisted_stage, __ATOMIC_ACQUIRE);
  if (stage != DEMO_DONE && stage != DEMO_FAILED)
    return -1;
  ejit_taskpool_print_stats();
  ejit_taskpool_print_compiled();
  ejit_print_code_pool_stats();
#if UNLISTED_DUMP_IR
  ejit_print_dumped("unlisted_X");
  ejit_print_dumped_module("unlisted_X");
#endif
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (ejit_get_global_layout_status(&s) != EJIT_OK ||
      ejit_taskpool_get_stats(&t) != EJIT_OK)
    return -1;
  SRE_printf("[UNLISTED] state=%u gen=%u required=%u ready=%u published=%u "
             "error=%u rank=%u active=%u unlistedReady=%u unlistedAppended=%u "
             "near=%llu far=%llu\n",
             s.state, s.generation, s.required, s.ready, s.published, s.error,
             s.errorRank, s.activeProfiles, s.unlistedReady, s.unlistedAppended,
             (unsigned long long)s.retainedNearBytes,
             (unsigned long long)s.retainedFarBytes);
  if (stage != DEMO_DONE || s.state != UNLISTED_COMPLETE || s.error ||
      s.generation != g_unlisted_result.generation ||
      s.required != UNLISTED_MEMBERS || s.ready != UNLISTED_MEMBERS ||
      s.published != UNLISTED_MEMBERS || s.activeProfiles ||
      s.unlistedReady ||
      s.unlistedAppended != UNLISTED_COUNT - UNLISTED_MEMBERS ||
      t.compileFailed || t.publishFailed || t.queueFull ||
      t.asyncCompiles < 2u * UNLISTED_COUNT ||
      t.readyEntries < UNLISTED_COUNT || ejit_taskpool_pending_count())
    return -1;
  uintptr_t previous = 0;
  for (uint32_t slot = 0; slot < UNLISTED_COUNT; ++slot) {
    if (!g_unlisted_final[slot] || !g_unlisted_calls[slot] ||
        g_unlisted_final[slot] <= previous)
      return -1;
    previous = g_unlisted_final[slot];
  }
  SRE_printf("[UNLISTED] RUNTIME_CHECKS_PASS 8 ordered manifest bodies + 3 "
             "appended unlisted bodies above them; NOT BOARD_ACCEPTED until "
             "worker PGO64/cap4/merge/preparation evidence is checked\n");
  return 0;
}
