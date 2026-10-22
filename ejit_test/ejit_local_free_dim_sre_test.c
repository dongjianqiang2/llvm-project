//===-- ejit_local_free_dim_sre_test.c - worker6 / owner16 demo ------------===//
//
// Single C board artifact; see jit_design_doc/EJIT_LOCAL_FREE_DIM_BOARD.md.
// Fresh standalone image: core6 test_ejit_period, core16 test_ejit_period,
// then core6 test_ejit_local_free_dim_print. All four arguments are ignored.
// Runtime: existing build.sh --stats or -DEJIT_STATS_ENABLE=ON; audit ON.
// A macro on this demo alone cannot enable counters in the runtime library.
// Board waits have no round limit by default; real errors still fail immediately.
//
//===----------------------------------------------------------------------===//

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#if !__has_attribute(ejit_entry) || !__has_attribute(ejit_dim) || \
    !__has_attribute(ejit_period_const) || \
    !__has_attribute(ejit_in_period_array) || !__has_attribute(ejit_free_dim)
#error "Use the matching EJIT Clang; unknown attributes are not a valid demo"
#endif
#ifdef EJIT_DISABLE
#error "This demo requires real EJIT annotations and generated wrappers"
#endif
#if !defined(EJIT_SRE_SHARED_TASKPOOL) || \
    !defined(EJIT_SRE_PGO_BRANCH_AUDIT) || !defined(EJIT_DIAG_ENABLE)
#error "Build demo and runtime with shared taskpool, branch audit and diagnostics"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

_Static_assert(sizeof(void *) == 8 && sizeof(ejit_config_t) == 56 &&
               sizeof(ejit_taskpool_stats_t) == 88,
               "Use the matching 64-bit spec5 public runtime header");
extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef LFD_RUN_INIT_ARRAY
#define LFD_RUN_INIT_ARRAY 1
#endif
#ifndef LFD_WAIT_ROUNDS
#define LFD_WAIT_ROUNDS 0u
#endif
#define LFD_SHARED __attribute__((section(".mc_shared"), aligned(64)))
#define LFD_SLOTS 10u
#define LFD_PHASES 3u

struct LocalSlot {
  ejit_may_const uint32_t mode;
  uint32_t payload;
};
struct LocalCell { struct LocalSlot slots[LFD_SLOTS]; };
LFD_SHARED ejit_period_arr(cell) struct LocalCell g_local_free_cells[2];
LFD_SHARED volatile struct { uint32_t slot; } g_local_free_runtime;
LFD_SHARED volatile uint32_t g_local_free_initializer_calls;

static __attribute__((always_inline)) unsigned next_local_slot(void) {
  ++g_local_free_initializer_calls;
  return g_local_free_runtime.slot;
}

ejit_entry uint32_t
local_free_work(ejit_period_arr_ind(cell) uint32_t cell) {
  const size_t offset ejit_free_dim =
      (size_t)next_local_slot() * sizeof(struct LocalSlot);
  const struct LocalSlot *p = (const struct LocalSlot *)(
      (const char *)&g_local_free_cells[cell].slots[0] + offset);
  return p->mode * 100u + p->payload;
}

// Never replace this with a direct call: O2 must still call the AOT wrapper.
static uint32_t (*volatile g_local_free_call)(uint32_t) = local_free_work;
static uint32_t local_free_index, local_cell_dim;

enum { LFD_RESET, LFD_STARTING, LFD_READY, LFD_RUNNING, LFD_DONE, LFD_FAILED };
LFD_SHARED uint32_t g_local_free_stage;
LFD_SHARED struct {
  uint32_t checked, deferred, phases, initializer_calls;
  uint32_t executed[LFD_PHASES];
  void *first[LFD_PHASES], *final[LFD_PHASES];
} g_local_free_result;

static int lfd_fail(const char *reason) {
  __atomic_store_n(&g_local_free_stage, LFD_FAILED, __ATOMIC_RELEASE);
  SRE_printf("[LFD247] FAIL core=%u: %s; reset before retry; core6 may print\n",
             (unsigned)g_ucLocalCoreID, reason);
  return -1;
}

static int lfd_delay(void) {
  return SRE_TaskDelay(1u) ? lfd_fail("SRE_TaskDelay failed") : 0;
}

static int lfd_stats(ejit_taskpool_stats_t *s) {
  if (ejit_taskpool_get_stats(s) != EJIT_OK)
    return lfd_fail("taskpool stats unavailable");
  if (s->compileFailed || s->publishFailed || s->queueFull)
    return lfd_fail("compile/publish/queue failure");
  return 0;
}

static int lfd_idle(const ejit_taskpool_stats_t *s) {
  return !s->pendingEntries && !s->queueApproxSize &&
         !ejit_taskpool_pending_count();
}

// Only a drain check, NEVER a compiler-source borrow fence by itself.
// Callers must first observe both selected identities' completed T2 publication,
// stop all lookups/business calls, and exclude every other producer/writer.
static int lfd_wait_idle(void) {
  for (unsigned round = 0;
       LFD_WAIT_ROUNDS == 0u || round < LFD_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t s = {0};
    if (lfd_stats(&s)) return -1;
    if (lfd_idle(&s)) return 0;
    if (round == 0u || (round + 1u) % 1000u == 0u)
      SRE_printf("[LFD247] waiting drain round=%u pending=%u queue=%u\n",
                 round + 1u, s.pendingEntries, s.queueApproxSize);
    if (lfd_delay()) return -1;
  }
  return lfd_fail("drain timeout; configuration NOT modified");
}

static int lfd_call(unsigned cell, unsigned slot, unsigned mode,
                    unsigned payload_extra, int wrapper, void **seen) {
  void *fn = 0;
  uint32_t bucket = 0;
  if (seen) *seen = 0;
  if (!wrapper) {
    ejit_dim_pair_t dim = {local_cell_dim, cell};
    ejit_status_t rc = ejit_taskpool_compile_or_get(
        local_free_index, &dim, 1u, &fn, &bucket);
    if (rc == EJIT_PENDING || rc == EJIT_ERR_QUEUE_FULL) {
      // Admission deferral also uses QUEUE_FULL. Real queue errors are checked
      // separately in lfd_stats. No pointer means no sample or initializer.
      if (fn) return lfd_fail("fallback returned unexpected pointer");
      ++g_local_free_result.deferred;
      return 0;
    }
    if (rc != EJIT_OK || !fn) return lfd_fail("lookup failed");
  }
  g_local_free_runtime.slot = slot;
  uint32_t before = g_local_free_initializer_calls;
  uint32_t actual = wrapper ? g_local_free_call(cell)
                           : ((uint32_t (*)(uint32_t))fn)(cell);
  // Even a wrong result must release the successfully acquired read token.
  if (!wrapper) ejit_taskpool_release_read(bucket);
  uint32_t expected = mode * 100u + cell * 1000u + slot + payload_extra;
  if (actual != expected || g_local_free_initializer_calls != before + 1u) {
    SRE_printf("[LFD247] call cell=%u slot=%u got=%u want=%u init=%u/%u\n",
               cell, slot, actual, expected, g_local_free_initializer_calls,
               before + 1u);
    return lfd_fail("runtime offset/payload/initializer mismatch");
  }
  ++g_local_free_result.checked;
  if (seen) *seen = fn;
  return 1;
}

static int lfd_generation(unsigned phase, unsigned cell, unsigned mode) {
  // Exercise the real cold wrapper once, before any manual lookup/prewarming.
  if (lfd_call(cell, 9u, mode, 0u, 1, 0) != 1) return -1;
  unsigned executed = 0, warned_stats = 0;
  void *first = 0, *last = 0;
  for (unsigned round = 0;
       LFD_WAIT_ROUNDS == 0u || round < LFD_WAIT_ROUNDS; ++round) {
    void *fn = 0;
    int rc = lfd_call(cell, round % LFD_SLOTS, mode, 0u, 0, &fn);
    if (rc < 0) return -1;
    if (rc) {
      if (!first) first = fn;
      last = fn;
      ++executed;
    }
    ejit_taskpool_stats_t s = {0};
    if (lfd_stats(&s)) return -1;
    // Matching ejit_init_pgo enforces a 64-dispatch window. Require >=65
    // real grants executed/released AND two successful publications per
    // isolated generation. Neither pending polls nor aggregate counters alone
    // suffice. Addresses may be reused across tiers; they are diagnostics only.
    // Worker tier/audit/IR evidence is still required for final acceptance.
    if (executed >= 65u && first && last &&
        s.asyncCompiles == 2u * (phase + 1u) &&
        s.readyEntries == (phase ? 2u : 1u) && lfd_idle(&s)) {
      g_local_free_result.first[phase] = first;
      g_local_free_result.final[phase] = last;
      g_local_free_result.executed[phase] = executed;
      ++g_local_free_result.phases;
      SRE_printf("[LFD247] PHASE=%u cell=%u mode=%u executed=%u "
                 "first=%p final=%p checked=%u\n", phase, cell, mode, executed,
                 first, last, g_local_free_result.checked);
      return 0;
    }
    if (executed >= 65u && !s.asyncCompiles && !warned_stats) {
      warned_stats = 1;
      SRE_printf("[LFD247] real grants executed but compile counters remain zero; "
                 "verify RUNTIME build.sh --stats or -DEJIT_STATS_ENABLE=ON; "
                 "a demo-only macro is insufficient. NOT acceptance.\n");
    }
    if (round == 0u || (round + 1u) % 1000u == 0u)
      SRE_printf("[LFD247] waiting phase=%u round=%u executed=%u compiles=%llu "
                 "ready=%u pending=%u queue=%u first=%p last=%p\n",
                 phase, round + 1u, executed, (unsigned long long)s.asyncCompiles,
                 s.readyEntries, s.pendingEntries, s.queueApproxSize, first, last);
    // Yield after successful T1 dispatches too; the real worker must progress.
    if (lfd_delay()) return -1;
  }
  SRE_printf("[LFD247] timeout phase=%u cell=%u executed=%u first=%p last=%p\n",
             phase, cell, executed, first, last);
  return lfd_fail("normal PGO publication/real-dispatch timeout");
}

// Run only after all admitted compilations finished; ordinary payload must
// remain live even after T2. All ten offsets differ, including offset zero.
static int lfd_live_payload(unsigned cell, unsigned mode) {
  if (lfd_wait_idle()) return -1;
  for (unsigned slot = 0; slot != LFD_SLOTS; ++slot)
    g_local_free_cells[cell].slots[slot].payload = cell * 1000u + slot + 123u;
  for (unsigned slot = 0; slot != LFD_SLOTS; ++slot)
    if (lfd_call(cell, slot, mode, 123u, 1, 0) != 1) return -1;
  for (unsigned slot = 0; slot != LFD_SLOTS; ++slot)
    g_local_free_cells[cell].slots[slot].payload = cell * 1000u + slot;
  return 0;
}

static int lfd_owner_workload(void) {
  for (unsigned cell = 0; cell != 2u; ++cell) {
    if (ejit_activate("cell", cell) != EJIT_OK)
      return lfd_fail("activate failed");
    if (lfd_generation(cell, cell, cell ? 11u : 7u)) return -1;
  }
  if (lfd_live_payload(0u, 7u) || lfd_live_payload(1u, 11u)) return -1;

  // This is deliberately NOT a concurrent lifecycle/borrow-fence demo.
  // Both identities have executed their published post-T1 code; the synchronous
  // compiler callback finished before publication. All grants were released.
  // Stop calls, drain, deactivate, drain again, then update. A timeout must
  // never authorize a write. The standalone-image exclusion is essential.
  if (lfd_wait_idle()) return -1;
  if (ejit_deactivate("cell", 1u) != EJIT_OK)
    return lfd_fail("deactivate failed; configuration NOT modified");
  if (lfd_wait_idle()) return -1;
  for (unsigned slot = 0; slot != LFD_SLOTS; ++slot)
    g_local_free_cells[1].slots[slot].mode = 19u;
  if (ejit_activate("cell", 1u) != EJIT_OK)
    return lfd_fail("reactivate failed");
  if (lfd_generation(2u, 1u, 19u) || lfd_live_payload(1u, 19u) ||
      lfd_live_payload(0u, 7u)) return -1;
  if (g_local_free_initializer_calls != g_local_free_result.checked)
    return lfd_fail("initializer executed outside a checked business call");
  g_local_free_result.initializer_calls = g_local_free_initializer_calls;
  __atomic_store_n(&g_local_free_stage, LFD_DONE, __ATOMIC_RELEASE);
  SRE_printf("[LFD247] OWNER_DONE calls=%u init=%u deferred=%u; "
             "NOT final acceptance; core6: test_ejit_local_free_dim_print\n",
             g_local_free_result.checked, g_local_free_result.initializer_calls,
             g_local_free_result.deferred);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a; (void)b; (void)c; (void)d;
  unsigned core = g_ucLocalCoreID;
  if (core != 6u && core != 16u) return -1;
  uint32_t stage = core == 6u ? LFD_RESET : LFD_READY;
  if (!__atomic_compare_exchange_n(
          &g_local_free_stage, &stage,
          core == 6u ? LFD_STARTING : LFD_RUNNING,
          0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[LFD247] rejected core=%u stage=%u; no re-init/retry\n",
               core, stage);
    return -1;
  }
#if LFD_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
  // Constructors and these public fixups must run on EACH core before its
  // registration freezes. Do not reference private generated index globals.
  local_free_index = local_cell_dim = ~0u;
  ejit_register_funcindex("local_free_work", &local_free_index);
  ejit_register_lifecycle("cell", &local_cell_dim);
  if (local_free_index == ~0u || local_cell_dim == ~0u)
    return lfd_fail("registration unresolved/already frozen; fresh image required");
  if (core == 6u)
    for (unsigned cell = 0; cell != 2u; ++cell)
      for (unsigned slot = 0; slot != LFD_SLOTS; ++slot) {
        g_local_free_cells[cell].slots[slot].mode = cell ? 11u : 7u;
        g_local_free_cells[cell].slots[slot].payload = cell * 1000u + slot;
      }
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = cfg.forceStaticRegistry = true;
  if (ejit_init_pgo(&cfg) != EJIT_OK)
    return lfd_fail("normal online PGO initialization failed");
  if (ejit_taskpool_get_worker_core() != 6u)
    return lfd_fail("runtime worker is not core6");
  ejit_taskpool_stats_t s = {0};
  if (lfd_stats(&s)) return -1;
  if (s.asyncCompiles || s.asyncEnqueues || s.cacheHits || s.readyEntries ||
      !lfd_idle(&s) || ejit_is_active("cell", 0u) || ejit_is_active("cell", 1u))
    return lfd_fail("requires fresh idle standalone image, no other callers");
  if (core == 6u) {
    ejit_dump_func("local_free_work");
    __atomic_store_n(&g_local_free_stage, LFD_READY, __ATOMIC_RELEASE);
    SRE_printf("[LFD247] WORKER_READY core6 waitRounds=%u (0=unlimited); "
               "core16: test_ejit_period\n", (unsigned)LFD_WAIT_ROUNDS);
    return 0;
  }
  return lfd_owner_workload();
}

// Read-only: no initialization, registration, activation, lookup or compile.
// Failed/partial runs stay failed. No stage reset and no retry from this entry.
int test_ejit_local_free_dim_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a; (void)b; (void)c; (void)d;
  if (g_ucLocalCoreID != 6u) return -1;
  uint32_t stage = __atomic_load_n(&g_local_free_stage, __ATOMIC_ACQUIRE);
  if (stage != LFD_DONE && stage != LFD_FAILED) {
    SRE_printf("[LFD247] NOT_READY stage=%u; wait for OWNER_DONE or FAIL\n", stage);
    return -1;
  }
  ejit_taskpool_print_stats();
  ejit_taskpool_print_compiled();
  ejit_print_dumped("local_free_work");
  ejit_print_dumped_module("local_free_work");
  ejit_taskpool_stats_t s = {0};
  int ok = stage == LFD_DONE &&
           ejit_taskpool_get_stats(&s) == EJIT_OK &&
           ejit_taskpool_get_worker_core() == 6u &&
           g_local_free_result.phases == LFD_PHASES &&
           g_local_free_result.checked == g_local_free_result.initializer_calls &&
           g_local_free_initializer_calls == g_local_free_result.checked &&
           s.asyncCompiles == 6u && s.readyEntries == 2u &&
           !s.compileFailed && !s.publishFailed && !s.queueFull && lfd_idle(&s);
  for (unsigned phase = 0; phase != LFD_PHASES; ++phase)
    ok &= g_local_free_result.executed[phase] >= 65u &&
          g_local_free_result.first[phase] &&
          g_local_free_result.final[phase];
  if (!ok) {
    SRE_printf("[LFD247] NOT_ACCEPTED stage=%u; retain partial diagnostics\n", stage);
    return -1;
  }
  SRE_printf("[LFD247] RUNTIME_CHECKS_PASS; FINAL_ACCEPTANCE_PENDING: verify "
             "worker init pgo=1, published entry_count=64, tier2=2, post_publish_seen=yes, 3 real 64-entry PGO audits, "
             "baked mode and live offset/payload in IR/ASM. OWNER_DONE alone "
             "is not acceptance. Reset image to rerun.\n");
  return 0;
}
