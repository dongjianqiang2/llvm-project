//===-- ejit_verify_subst_sre_multicore_test.c - SRE verifier example ------===//
//
// Board-only SRE example for ejit_init_verify / may_const substitution
// diagnostics. It models exactly ONE compile worker on core 6 and exactly ONE
// workload/owner test on core 16. It is not six workers or sixteen owners.
//
// Run after a board reset, with the same image on both cores:
//   core[6]  -> test_ejit_period(0, 0, 0, 0)   // positive case
//   core[16] -> test_ejit_period(0, 0, 0, 0)   // positive case
//
// For the deliberate cross-core mismatch, reset the board and use mode 1 on
// both sessions:
//   core[6]  -> test_ejit_period(1, 0, 0, 0)   // worker stable=0
//   core[16] -> test_ejit_period(1, 0, 0, 0)   // workload stable=4
//
// The runtime must be built with EJIT_VERIFY_SUBSTITUTION. Keep
// EJIT_SRE_PGO_BRANCH_AUDIT=ON for the validation image. This source prints
// the private configuration address and values on each role; equal addresses
// alone are not proof of equal backing storage.
//
// The positive case expects one cold compile, two emitted sites, six
// instrumented calls, and twelve checks: stableField has zero mismatches and
// volatileField has five after the intentional post-compile write to 7. The
// warm-cache phase after reset deliberately expects checks with zero new
// emissions. The exact totals do not apply to polling, extra probes, or other
// tiers.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#define VERIFY_WORKER_CORE 6u
#define VERIFY_OWNER_CORE 16u
#define VERIFY_NEGATIVE_MODE 1u
#define MAX_SITES 16u
#define ENQUEUE_SETTLE_TICKS 1000u
#define WAIT_ROUNDS 600u
#define WAIT_TICKS 100u
#define IDLE_TICKS 40000u

struct Cfg {
  ejit_may_const uint32_t stableField;
  ejit_may_const uint32_t volatileField;
  uint32_t pad;
};

// This object is intentionally core-private. The test initializes every
// participating copy before init/role selection; it does not turn a private
// application object into a product-wide snapshot.
ejit_period_arr(cell) struct Cfg g_cfg[1];

ejit_entry __attribute__((noinline)) uint32_t
probe(ejit_period_arr_ind(cell) uint8_t ci) {
  const struct Cfg *p = &g_cfg[ci];
  return p->stableField * 100u + p->volatileField;
}

static uint32_t g_fail = 0;

#define T(cond, fmt, ...)                                                      \
  do {                                                                         \
    if (cond)                                                                  \
      SRE_printf("  OK   " fmt "\n", ##__VA_ARGS__);                         \
    else {                                                                     \
      SRE_printf("  FAIL " fmt "\n", ##__VA_ARGS__);                         \
      ++g_fail;                                                                \
    }                                                                          \
  } while (0)

static void idle_forever(uint32_t core, const char *role) {
  for (;;) {
    SRE_TaskDelay(IDLE_TICKS);
    SRE_printf("[VERIFY][core=%u] idle role=%s\n", core, role);
  }
}

static void init_private_cfg(uint32_t core, bool deliberateMismatch) {
  g_cfg[0].stableField =
      deliberateMismatch && core == VERIFY_WORKER_CORE ? 0u : 4u;
  g_cfg[0].volatileField = 0u;
  g_cfg[0].pad = 0u;
  SRE_printf("[VERIFY][core=%u] cfg=%p stable=%u volatile=%u\n", core,
             (void *)&g_cfg[0], g_cfg[0].stableField,
             g_cfg[0].volatileField);
}

static void snapshot(ejit_verify_stats_t *stats) {
  memset(stats, 0, sizeof(*stats));
  ejit_verify_get_stats(stats);
}

static bool site_ends_with(const ejit_verify_site_t *site,
                           const char *suffix) {
  for (size_t i = 0; i < EJIT_VERIFY_SITE_NAME_MAX; ++i) {
    if (site->site[i] == '\0')
      return false;
    size_t j = 0;
    while (i + j < EJIT_VERIFY_SITE_NAME_MAX && suffix[j] != '\0' &&
           site->site[i + j] == suffix[j])
      ++j;
    if (suffix[j] == '\0' && i + j < EJIT_VERIFY_SITE_NAME_MAX &&
        site->site[i + j] == '\0')
      return true;
  }
  return false;
}

static const ejit_verify_site_t *find_site(const ejit_verify_site_t *sites,
                                           size_t count,
                                           const char *suffix) {
  for (size_t i = 0; i < count; ++i)
    if (site_ends_with(&sites[i], suffix))
      return &sites[i];
  return NULL;
}

static void dump_sites(uint32_t core, const ejit_verify_site_t *sites,
                       size_t count) {
  for (size_t i = 0; i < count; ++i)
    SRE_printf("    [core=%u] site[%u] %s checks=%llu mismatches=%llu "
               "lastFrozen=0x%llx lastActual=0x%llx\n",
               core, (unsigned)i, sites[i].site,
               (unsigned long long)sites[i].checks,
               (unsigned long long)sites[i].mismatches,
               (unsigned long long)sites[i].lastFrozen,
               (unsigned long long)sites[i].lastActual);
}

static bool wait_for_published_compile(uint32_t core, const char *phase,
                                       uint64_t compileBefore,
                                       uint32_t readyBefore) {
  SRE_TaskDelay(ENQUEUE_SETTLE_TICKS);
  uint32_t zeroRounds = 0;
  for (uint32_t round = 0; round < WAIT_ROUNDS; ++round) {
    const uint32_t pending = ejit_taskpool_pending_count();
    ejit_taskpool_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ejit_taskpool_get_stats(&stats);
    const bool published = stats.asyncCompiles > compileBefore &&
                           stats.readyEntries > readyBefore;
    if (pending == 0 && published && stats.compileFailed == 0) {
      if (++zeroRounds == 3) {
        SRE_printf("[VERIFY][core=%u] %s published round=%u ready=%u "
                   "compiles=%llu failed=%llu\n",
                   core, phase, round, stats.readyEntries,
                   (unsigned long long)stats.asyncCompiles,
                   (unsigned long long)stats.compileFailed);
        return true;
      }
    } else {
      zeroRounds = 0;
    }
    if ((round % 50u) == 0u)
      SRE_printf("[VERIFY][core=%u] %s waiting round=%u pending=%u "
                 "ready=%u compiles=%llu\n",
                 core, phase, round, pending, stats.readyEntries,
                 (unsigned long long)stats.asyncCompiles);
    SRE_TaskDelay(WAIT_TICKS);
  }
  SRE_printf("[VERIFY][core=%u] %s FAIL: published compile timeout\n", core,
             phase);
  return false;
}

static bool prepare_cold_compile(uint32_t core, const char *phase,
                                 uint64_t *compileBefore,
                                 uint32_t *readyBefore) {
  ejit_taskpool_stats_t before;
  memset(&before, 0, sizeof(before));
  ejit_taskpool_get_stats(&before);
  *compileBefore = before.asyncCompiles;
  *readyBefore = before.readyEntries;
  // The board command starts from a reset, and this explicit clear makes the
  // cold-compile premise visible even when the test is rerun in one image.
  ejit_clear_cache();
  ejit_verify_reset_stats();
  if (ejit_activate("cell", 0) != EJIT_OK)
    return false;
  // This call is the AOT fallback that enqueues the specialization. The wait
  // helper never probes the function, so the exact call count stays explicit.
  (void)probe(0);
  return wait_for_published_compile(core, phase, *compileBefore, *readyBefore);
}

static void run_positive(uint32_t core) {
  uint64_t compileBefore = 0;
  uint32_t readyBefore = 0;

  SRE_printf("\n[VERIFY][core=%u] --- A: cold compile, values untouched ---\n",
             core);
  T(prepare_cold_compile(core, "A", &compileBefore, &readyBefore),
    "published specialization before first instrumented call");
  uint32_t result = probe(0);
  T(result == 400u, "probe = %u (expected 400)", result);

  ejit_verify_stats_t stats;
  snapshot(&stats);
  T(stats.mismatches == 0, "no mismatch while memory is untouched (%llu)",
    (unsigned long long)stats.mismatches);
  T(stats.sites == 2, "emitted %llu may_const site(s) on worker core",
    (unsigned long long)stats.sites);
  T(stats.checks == 2, "executed %llu checks after one instrumented call",
    (unsigned long long)stats.checks);

  SRE_printf("\n[VERIFY][core=%u] --- B: volatileField rewritten ---\n", core);
  const uint64_t before = stats.mismatches;
  g_cfg[0].volatileField = 7u;
  result = probe(0);
  T(result == 407u, "still tracks live memory: %u (expected 407)", result);
  snapshot(&stats);
  T(stats.mismatches == before + 1,
    "verifier flagged the change: %llu mismatch(es)",
    (unsigned long long)stats.mismatches);

  SRE_printf("\n[VERIFY][core=%u] --- C: stableField repeated ---\n", core);
  const uint64_t mid = stats.mismatches;
  for (uint32_t i = 0; i < 4u; ++i)
    (void)probe(0);
  snapshot(&stats);
  T(stats.mismatches == mid + 4,
    "only volatileField diverges: %llu new over 4 calls",
    (unsigned long long)(stats.mismatches - mid));
  T(stats.checks == 12,
    "one cold compile plus exactly six instrumented calls gives 12 checks");

  SRE_printf("\n[VERIFY][core=%u] --- D: per-site classification ---\n", core);
  ejit_verify_site_t sites[MAX_SITES];
  memset(sites, 0, sizeof(sites));
  size_t count = ejit_verify_get_sites(sites, MAX_SITES);
  dump_sites(core, sites, count);
  T(count == 2, "two sites recorded (actual %u)", (unsigned)count);
  const ejit_verify_site_t *stable = find_site(sites, count, "g_cfg+0");
  const ejit_verify_site_t *moving = find_site(sites, count, "g_cfg+4");
  T(stable != NULL, "stableField site recorded (...g_cfg+0)");
  T(moving != NULL, "volatileField site recorded (...g_cfg+4)");
  if (stable) {
    T(stable->checks == 6, "stableField checked %llu time(s)",
      (unsigned long long)stable->checks);
    T(stable->mismatches == 0,
      "stableField has no frozen/live disagreement (%llu)",
      (unsigned long long)stable->mismatches);
    // lastFrozen/lastActual are last-mismatch evidence; zero is valid here.
    T(stable->lastFrozen == 0 && stable->lastActual == 0,
      "stableField has no last-mismatch evidence (zero is valid)");
  }
  if (moving) {
    T(moving->checks == 6, "volatileField checked %llu time(s)",
      (unsigned long long)moving->checks);
    T(moving->mismatches == 5,
      "volatileField diverged %llu time(s)",
      (unsigned long long)moving->mismatches);
    T(moving->lastFrozen == 0 && moving->lastActual == 7,
      "last mismatch evidence frozen=0x%llx actual=0x%llx",
      (unsigned long long)moving->lastFrozen,
      (unsigned long long)moving->lastActual);
  }

  SRE_printf("\n[VERIFY][core=%u] --- E: warm cache after reset ---\n", core);
  ejit_verify_reset_stats();
  result = probe(0);
  snapshot(&stats);
  T(result == 407u, "warm cached probe = %u (expected 407)", result);
  T(stats.sites == 0 && stats.checks == 2,
    "warm cache has checks=%llu and no new emissions=%llu",
    (unsigned long long)stats.checks, (unsigned long long)stats.sites);
  memset(sites, 0, sizeof(sites));
  count = ejit_verify_get_sites(sites, MAX_SITES);
  T(count == 2, "warm cache records are still attributable (actual %u)",
    (unsigned)count);

  SRE_printf("\n[VERIFY][core=%u] --- F: final reset ---\n", core);
  ejit_verify_reset_stats();
  snapshot(&stats);
  T(stats.sites == 0 && stats.checks == 0 && stats.mismatches == 0,
    "counters cleared (sites=%llu checks=%llu mismatches=%llu)",
    (unsigned long long)stats.sites, (unsigned long long)stats.checks,
    (unsigned long long)stats.mismatches);
  count = ejit_verify_get_sites(sites, MAX_SITES);
  T(count == 0, "site records cleared (%u)", (unsigned)count);
}

static void run_deliberate_mismatch(uint32_t core) {
  uint64_t compileBefore = 0;
  uint32_t readyBefore = 0;
  SRE_printf("\n[VERIFY][core=%u] --- NEGATIVE: worker stable=0, owner stable=4 ---\n",
             core);
  T(prepare_cold_compile(core, "negative", &compileBefore, &readyBefore),
    "published specialization for deliberate cross-core mismatch");
  const uint32_t result = probe(0);
  T(result == 400u, "workload live result = %u (expected 400)", result);

  ejit_verify_stats_t stats;
  snapshot(&stats);
  T(stats.sites == 2 && stats.checks == 2,
    "negative case has two emitted sites and two checks");
  T(stats.mismatches > 0,
    "worker/workload frozen-live disagreement remains visible (%llu)",
    (unsigned long long)stats.mismatches);
  ejit_verify_site_t sites[MAX_SITES];
  memset(sites, 0, sizeof(sites));
  size_t count = ejit_verify_get_sites(sites, MAX_SITES);
  dump_sites(core, sites, count);
  const ejit_verify_site_t *stable = find_site(sites, count, "g_cfg+0");
  const ejit_verify_site_t *moving = find_site(sites, count, "g_cfg+4");
  T(stable && stable->mismatches == 1,
    "deliberate stableField disagreement is attributed to g_cfg+0");
  T(moving && moving->mismatches == 0,
    "unchanged volatileField is not blamed in the negative case");
}

int test_ejit_period(uint8_t mode, uint8_t trpIdxArg, uint8_t sliceIdxArg,
                     uint8_t carrierIdxArg) {
  (void)trpIdxArg;
  (void)sliceIdxArg;
  (void)carrierIdxArg;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  const bool deliberateMismatch = mode == VERIFY_NEGATIVE_MODE;
  SRE_printf("\n=== EJIT SRE Substitution Verifier Test ===\n");
  SRE_printf("[VERIFY][core=%u] enter mode=%u worker=6 owner=16\n", core,
             (unsigned)mode);

  // This must precede role divergence and any init that can enqueue work.
  init_private_cfg(core, deliberateMismatch);
  call_init_array_functions();

  ejit_config_t config;
  memset(&config, 0, sizeof(config));
  config.compileMode = EJIT_COMPILE_ASYNC;
  config.optLevel = EJIT_OPT_L2;
  config.enableLogger = false;
  config.forceStaticRegistry = true;
  ejit_status_t initRc = ejit_init_verify(&config);
  const uint32_t workerCore = ejit_taskpool_get_worker_core();
  SRE_printf("[VERIFY][core=%u] init rc=%d workerCore=%u ownerCore=%u\n", core,
             (int)initRc, workerCore, VERIFY_OWNER_CORE);
  if (initRc != EJIT_OK)
    idle_forever(core, "init-failed");
  T(workerCore == VERIFY_WORKER_CORE,
    "exactly one compile worker is core[6] (reported %u)", workerCore);
  if (workerCore != VERIFY_WORKER_CORE)
    idle_forever(core, "wrong-worker");
  if (core == workerCore)
    idle_forever(core, "compile-worker");
  if (core != VERIFY_OWNER_CORE)
    idle_forever(core, "bystander");

  T(ejit_verify_available() == 1,
    "runtime built with EJIT_VERIFY_SUBSTITUTION");
  if (deliberateMismatch)
    run_deliberate_mismatch(core);
  else
    run_positive(core);

  SRE_printf("\n[VERIFY][core=%u] RESULT: %s (%u failure(s))\n", core,
             g_fail == 0 ? "PASS" : "FAIL", g_fail);
  idle_forever(core, g_fail == 0 ? "verify-complete" : "verify-failed");
  return 0;
}
