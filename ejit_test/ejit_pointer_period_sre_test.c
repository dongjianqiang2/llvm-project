//===-- ejit_pointer_period_sre_test.c - PR251 worker6 / owner16 demo -----===//
// Matching spec5 EJitRuntime.h + EJIT Clang (including PR249 pointer layouts).
// Fresh standalone image: core6 test_ejit_period; core16 test_ejit_period;
// core6 test_ejit_pointer_period_print. All four shell arguments are ignored.
// Runtime: normal online PGO, branch audit, diagnostics and stats enabled.
// Default waits are unlimited. See EJIT_POINTER_PERIOD_BOARD.md.

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#if !__has_attribute(ejit_entry) || !__has_attribute(ejit_dim) ||              \
    !__has_attribute(ejit_period_const) ||                                     \
    !__has_attribute(ejit_in_period_array) ||                                  \
    !__has_attribute(ejit_bound_ptr) || !__has_attribute(ejit_free_dim)
#error "Use the matching EJIT Clang; unknown annotations are not a valid test"
#endif
#ifdef EJIT_DISABLE
#error "This demo requires EJIT annotations and generated wrappers"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

_Static_assert(sizeof(void *) == 8 && sizeof(ejit_config_t) == 56 &&
                   sizeof(ejit_taskpool_stats_t) == 88,
               "Use the matching 64-bit spec5 runtime header");
extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef PR251_RUN_INIT_ARRAY
#define PR251_RUN_INIT_ARRAY 1
#endif
#ifndef PR251_WAIT_ROUNDS
#define PR251_WAIT_ROUNDS 0u
#endif
#define PP_SHARED __attribute__((section(".mc_shared"), aligned(64)))
#define PP_SLOTS 10u
#define PP_CASES 3u
#define PP_PHASES 6u

struct PointerSlot {
  ejit_may_const uint32_t mode;
  uint32_t payload;
};
struct PointerCell {
  struct PointerSlot slot[PP_SLOTS];
};
struct BoundCell {
  ejit_may_const uint32_t count;
  ejit_may_const uint32_t weights[4];
  uint32_t payload[PP_SLOTS];
};
PP_SHARED struct PointerCell g_pr251_cells[2];
PP_SHARED ejit_period_arr(cell)
struct PointerCell *g_pr251_pointer = g_pr251_cells;
PP_SHARED struct BoundCell g_pr251_bound[2];
PP_SHARED uint32_t g_pr251_lookup_queries;

// An exact extent for this demo-owned object, not a general malloc adapter.
// Unknown/interior pointers authorize nothing. Both cores install the same
// callback before init, using the existing PR249 API.
static uint64_t pp_allocation_size(const void *base, void *context) {
  (void)context;
  __atomic_fetch_add(&g_pr251_lookup_queries, 1u, __ATOMIC_RELAXED);
  return base == (const void *)g_pr251_cells ? sizeof(g_pr251_cells) : 0u;
}

ejit_entry uint32_t pr251_pointer_slots(ejit_period_arr_ind(cell) uint8_t cell,
                                        ejit_free_dim uint16_t slot) {
  const struct PointerCell *p = &g_pr251_pointer[cell];
  return (p->slot[slot % 5u].mode + p->slot[slot % 10u].mode) * 100u +
         p->slot[slot % 10u].payload;
}

ejit_entry uint32_t pr251_pointer_constant(ejit_period_arr_ind(cell)
                                               uint8_t cell,
                                           uint16_t slot) {
  const struct PointerCell *p = &g_pr251_pointer[cell];
  return p->slot[3].mode * 100u + p->slot[slot % 10u].payload;
}

ejit_entry uint32_t pr251_bound_loop(ejit_period_arr_ind(cell) uint8_t cell,
                                     uint16_t slot,
                                     EJIT_BOUND_PTR(cell)
                                         const struct BoundCell *p) {
  (void)cell;
  uint32_t sum = 0;
  // The AOT loop bound is dynamic. Specializing count exposes full unrolling;
  // the post-unroll field pass must retain the bound-pointer context.
#pragma clang loop unroll(full) vectorize(disable) interleave(disable)
  for (uint32_t i = 0; i < p->count; ++i)
    sum += p->weights[i];
  return sum * 100u + p->payload[slot % 10u];
}

// Keep real wrapper calls, even when the surrounding demo is built with O2.
static uint32_t (*volatile pp_slots_call)(uint8_t,
                                          uint16_t) = pr251_pointer_slots;
static uint32_t (*volatile pp_constant_call)(uint8_t,
                                             uint16_t) = pr251_pointer_constant;
static uint32_t (*volatile pp_bound_call)(
    uint8_t, uint16_t, const struct BoundCell *) = pr251_bound_loop;
static const char *const pp_names[PP_CASES] = {
    "pr251_pointer_slots", "pr251_pointer_constant", "pr251_bound_loop"};
static uint32_t pp_indices[PP_CASES], pp_cell_dim;
enum { PP_RESET, PP_STARTING, PP_READY, PP_RUNNING, PP_DONE, PP_FAILED };
PP_SHARED uint32_t g_pr251_stage;
PP_SHARED struct {
  uint32_t checked, deferred, phases;
  uint32_t executed[PP_PHASES];
  void *first[PP_PHASES], *final[PP_PHASES];
} g_pr251_result;

static int pp_fail(const char *reason) {
  __atomic_store_n(&g_pr251_stage, PP_FAILED, __ATOMIC_RELEASE);
  SRE_printf("[PTR251] FAIL core=%u: %s; reset to retry; core6 may print\n",
             (unsigned)g_ucLocalCoreID, reason);
  return -1;
}
static int pp_delay(void) {
  return SRE_TaskDelay(1u) ? pp_fail("SRE_TaskDelay failed") : 0;
}
static int pp_stats(ejit_taskpool_stats_t *s) {
  if (ejit_taskpool_get_stats(s) != EJIT_OK)
    return pp_fail("taskpool stats unavailable");
  if (s->compileFailed || s->publishFailed || s->queueFull)
    return pp_fail("compile/publish/queue failure");
  return 0;
}
static int pp_idle(const ejit_taskpool_stats_t *s) {
  return !s->pendingEntries && !s->queueApproxSize &&
         !ejit_taskpool_pending_count();
}
// A drain check, not a general compiler-borrow fence. The update below first
// completes all three T2 publications and stops every caller in this image.
static int pp_wait_idle(void) {
  for (unsigned round = 0; !PR251_WAIT_ROUNDS || round < PR251_WAIT_ROUNDS;
       ++round) {
    ejit_taskpool_stats_t s = {0};
    if (pp_stats(&s))
      return -1;
    if (pp_idle(&s))
      return 0;
    if (round % 1000u == 0)
      SRE_printf("[PTR251] waiting drain pending=%u queue=%u\n",
                 s.pendingEntries, s.queueApproxSize);
    if (pp_delay())
      return -1;
  }
  return pp_fail("drain timeout; configuration NOT modified");
}

static int pp_call(unsigned which, unsigned slot, unsigned mode, unsigned extra,
                   int wrapper, void **seen) {
  void *fn = 0;
  uint32_t bucket = 0;
  if (seen)
    *seen = 0;
  if (!wrapper) {
    const ejit_dim_pair_t dim = {pp_cell_dim, 1u};
    ejit_status_t rc =
        which == 2u ? ejit_taskpool_compile_or_get_bound(
                          pp_indices[which], &dim, 1u, &g_pr251_bound[1],
                          sizeof(g_pr251_bound[1]), 2u, &fn, &bucket)
                    : ejit_taskpool_compile_or_get(pp_indices[which], &dim, 1u,
                                                   &fn, &bucket);
    if (rc == EJIT_PENDING || rc == EJIT_ERR_QUEUE_FULL) {
      if (fn)
        return pp_fail("fallback returned unexpected pointer");
      ++g_pr251_result.deferred;
      return 0;
    }
    if (rc != EJIT_OK || !fn)
      return pp_fail("lookup failed");
  }
  uint32_t actual;
  if (which == 2u)
    actual =
        wrapper
            ? pp_bound_call(1u, slot, &g_pr251_bound[1])
            : ((uint32_t (*)(uint8_t, uint16_t, const struct BoundCell *))fn)(
                  1u, slot, &g_pr251_bound[1]);
  else if (wrapper)
    actual = which ? pp_constant_call(1u, slot) : pp_slots_call(1u, slot);
  else
    actual = ((uint32_t (*)(uint8_t, uint16_t))fn)(1u, slot);
  if (!wrapper)
    ejit_taskpool_release_read(bucket);
  const uint32_t folded =
      which == 2u ? 4u * mode + 12u : (which ? mode : 2u * mode);
  const uint32_t expected = folded * 100u + 1000u + slot % PP_SLOTS + extra;
  if (actual != expected) {
    SRE_printf("[PTR251] case=%u slot=%u got=%u want=%u\n", which, slot, actual,
               expected);
    return pp_fail("functional/live-slot mismatch");
  }
  ++g_pr251_result.checked;
  if (seen)
    *seen = fn;
  return 1;
}

static int pp_generation(unsigned phase, unsigned mode) {
  unsigned which = phase % PP_CASES, executed = 0, warned = 0;
  void *first = 0, *last = 0;
  // Cold wrapper first: no manual lookup or prewarming before this call.
  if (pp_call(which, 9u, mode, 0u, 1, 0) != 1)
    return -1;
  for (unsigned round = 0; !PR251_WAIT_ROUNDS || round < PR251_WAIT_ROUNDS;
       ++round) {
    void *fn = 0;
    int rc = pp_call(which, round % PP_SLOTS, mode, 0u, 0, &fn);
    if (rc < 0)
      return -1;
    if (rc) {
      if (!first)
        first = fn;
      last = fn;
      ++executed;
    }
    ejit_taskpool_stats_t s = {0};
    if (pp_stats(&s))
      return -1;
    unsigned ready = phase < PP_CASES ? phase + 1u : PP_CASES;
    // More than the normal 64-grant T1 quota must really execute. Pending
    // polls are not samples. Addresses may legally be reused between tiers.
    if (executed >= 65u && first && last &&
        s.asyncCompiles == 2u * (phase + 1u) && s.readyEntries == ready &&
        pp_idle(&s)) {
      g_pr251_result.executed[phase] = executed;
      g_pr251_result.first[phase] = first;
      g_pr251_result.final[phase] = last;
      ++g_pr251_result.phases;
      SRE_printf(
          "[PTR251] PHASE=%u case=%s mode=%u executed=%u first=%p final=%p\n",
          phase, pp_names[which], mode, executed, first, last);
      return 0;
    }
    if (executed >= 65u && !s.asyncCompiles && !warned) {
      warned = 1;
      SRE_printf("[PTR251] runtime counters missing: rebuild RUNTIME with "
                 "build.sh --stats / EJIT_STATS_ENABLE=ON; NOT acceptance\n");
    }
    if (round % 1000u == 0)
      SRE_printf("[PTR251] waiting phase=%u calls=%u compiles=%llu "
                 "ready=%u pending=%u queue=%u\n",
                 phase, executed, (unsigned long long)s.asyncCompiles,
                 s.readyEntries, s.pendingEntries, s.queueApproxSize);
    if (pp_delay())
      return -1;
  }
  return pp_fail("PGO publication timeout");
}

static void pp_set_data(unsigned mode) {
  for (unsigned cell = 0; cell < 2u; ++cell) {
    g_pr251_bound[cell].count = 4u;
    for (unsigned i = 0; i < 4u; ++i)
      g_pr251_bound[cell].weights[i] = mode + i * 2u;
    for (unsigned slot = 0; slot < PP_SLOTS; ++slot) {
      g_pr251_cells[cell].slot[slot].mode = mode;
      g_pr251_cells[cell].slot[slot].payload = cell * 1000u + slot;
      g_pr251_bound[cell].payload[slot] = cell * 1000u + slot;
    }
  }
}
static int pp_live_payload(unsigned mode) {
  if (pp_wait_idle())
    return -1;
  for (unsigned slot = 0; slot < PP_SLOTS; ++slot) {
    g_pr251_cells[1].slot[slot].payload += 123u;
    g_pr251_bound[1].payload[slot] += 123u;
  }
  for (unsigned which = 0; which < PP_CASES; ++which)
    for (unsigned slot = 0; slot < PP_SLOTS; ++slot)
      if (pp_call(which, slot, mode, 123u, 1, 0) != 1)
        return -1;
  for (unsigned slot = 0; slot < PP_SLOTS; ++slot) {
    g_pr251_cells[1].slot[slot].payload -= 123u;
    g_pr251_bound[1].payload[slot] -= 123u;
  }
  return 0;
}
static int pp_owner_workload(void) {
  if (ejit_activate("cell", 1u) != EJIT_OK)
    return pp_fail("activate failed");
  for (unsigned phase = 0; phase < PP_CASES; ++phase)
    if (pp_generation(phase, 7u))
      return -1;
  if (pp_live_payload(7u) || pp_wait_idle())
    return -1;
  if (ejit_deactivate("cell", 1u) != EJIT_OK)
    return pp_fail("deactivate failed; configuration NOT modified");
  if (pp_wait_idle())
    return -1;
  pp_set_data(19u);
  if (ejit_activate("cell", 1u) != EJIT_OK)
    return pp_fail("reactivate failed");
  for (unsigned phase = PP_CASES; phase < PP_PHASES; ++phase)
    if (pp_generation(phase, 19u))
      return -1;
  if (pp_live_payload(19u))
    return -1;
  if (!__atomic_load_n(&g_pr251_lookup_queries, __ATOMIC_RELAXED))
    return pp_fail("allocation lookup never queried; check compiler/layouts");
  __atomic_store_n(&g_pr251_stage, PP_DONE, __ATOMIC_RELEASE);
  SRE_printf("[PTR251] OWNER_DONE checked=%u deferred=%u; core6: "
             "test_ejit_pointer_period_print; IR acceptance still required\n",
             g_pr251_result.checked, g_pr251_result.deferred);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  const unsigned core = g_ucLocalCoreID;
  if (core != 6u && core != 16u)
    return -1;
  uint32_t stage = core == 6u ? PP_RESET : PP_READY;
  if (!__atomic_compare_exchange_n(&g_pr251_stage, &stage,
                                   core == 6u ? PP_STARTING : PP_RUNNING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[PTR251] rejected core=%u stage=%u; no re-init/retry\n", core,
               stage);
    return -1;
  }
#if PR251_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
  pp_cell_dim = ~0u;
  ejit_register_lifecycle("cell", &pp_cell_dim);
  for (unsigned i = 0; i < PP_CASES; ++i) {
    pp_indices[i] = ~0u;
    ejit_register_funcindex(pp_names[i], &pp_indices[i]);
    if (pp_indices[i] == ~0u)
      return pp_fail("function registration unresolved");
  }
  if (pp_cell_dim == ~0u)
    return pp_fail("cell registration unresolved");
  if (core == 6u)
    pp_set_data(7u);
  if (ejit_set_allocation_size_lookup(pp_allocation_size, 0) != EJIT_OK)
    return pp_fail("allocation lookup registration failed");
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = cfg.forceStaticRegistry = true;
  if (ejit_init_pgo(&cfg) != EJIT_OK)
    return pp_fail("online PGO init failed");
  if (ejit_taskpool_get_worker_core() != 6u)
    return pp_fail("worker is not core6");
  ejit_taskpool_stats_t s = {0};
  if (pp_stats(&s))
    return -1;
  if (s.asyncCompiles || s.asyncEnqueues || s.cacheHits || s.readyEntries ||
      !pp_idle(&s) || ejit_is_active("cell", 1u))
    return pp_fail("fresh idle standalone image required, no other callers");
  if (core == 6u) {
    ejit_dump_func("*");
    __atomic_store_n(&g_pr251_stage, PP_READY, __ATOMIC_RELEASE);
    SRE_printf("[PTR251] WORKER_READY core6 waitRounds=%u (0=unlimited); "
               "core16: test_ejit_period\n",
               (unsigned)PR251_WAIT_ROUNDS);
    return 0;
  }
  return pp_owner_workload();
}

// Read-only. Never reinitialize or turn a partial/failed run into acceptance.
int test_ejit_pointer_period_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != 6u)
    return -1;
  uint32_t stage = __atomic_load_n(&g_pr251_stage, __ATOMIC_ACQUIRE);
  if (stage != PP_DONE && stage != PP_FAILED) {
    SRE_printf("[PTR251] NOT_READY stage=%u; wait for OWNER_DONE or FAIL\n",
               stage);
    return -1;
  }
  ejit_taskpool_print_stats();
  ejit_taskpool_print_compiled();
  for (unsigned i = 0; i < PP_CASES; ++i) {
    ejit_print_dumped(pp_names[i]);
    ejit_print_dumped_module(pp_names[i]);
  }
  ejit_taskpool_stats_t s = {0};
  int ok = stage == PP_DONE && ejit_taskpool_get_stats(&s) == EJIT_OK &&
           g_pr251_result.phases == PP_PHASES && s.asyncCompiles == 12u &&
           s.readyEntries == PP_CASES && !s.compileFailed && !s.publishFailed &&
           !s.queueFull && pp_idle(&s) && ejit_taskpool_get_worker_core() == 6u;
  for (unsigned i = 0; i < PP_PHASES; ++i)
    ok &= g_pr251_result.executed[i] >= 65u && g_pr251_result.first[i] &&
          g_pr251_result.final[i];
  if (!ok) {
    SRE_printf("[PTR251] NOT_ACCEPTED stage=%u; retain partial diagnostics\n",
               stage);
    return -1;
  }
  SRE_printf("[PTR251] RUNTIME_CHECKS_PASS; FINAL_ACCEPTANCE_PENDING: "
             "pgo=1, six real 64-entry audits, three published T2 modules with "
             "entry_count=64, baked mode/weights and LIVE slot/payload. "
             "OWNER_DONE alone is not acceptance. Reset to rerun.\n");
  return 0;
}
