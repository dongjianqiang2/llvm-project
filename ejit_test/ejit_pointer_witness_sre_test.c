//===-- ejit_pointer_witness_sre_test.c - PR256 worker6 / owner16 demo ----===//
// Matching PR256 Clang/runtime, including PR247 local free-dim support.
// Fresh image: core6 test_ejit_period; core16 test_ejit_period;
// core6 test_ejit_pointer_witness_print. Shell arguments are ignored.
// Normal online PGO, audit, diagnostics and runtime stats must be enabled.
// No allocation-size lookup/bound registration. Default waits are unlimited.
// See jit_design_doc/EJIT_POINTER_WITNESS_BOARD.md for IR acceptance.

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#if !__has_attribute(ejit_entry) || !__has_attribute(ejit_dim) ||              \
    !__has_attribute(ejit_period_const) ||                                     \
    !__has_attribute(ejit_in_period_array) || !__has_attribute(ejit_free_dim)
#error "Use the matching EJIT Clang; unknown annotations invalidate this test"
#endif
#ifdef EJIT_DISABLE
#error "This demo requires EJIT annotations and generated wrappers"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include <stddef.h>

_Static_assert(sizeof(void *) == 8 && sizeof(ejit_config_t) == 56 &&
                   sizeof(ejit_taskpool_stats_t) == 88,
               "Use the matching 64-bit spec5 runtime header");
extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef PR256_RUN_INIT_ARRAY
#define PR256_RUN_INIT_ARRAY 1
#endif
#ifndef PR256_WAIT_ROUNDS
#define PR256_WAIT_ROUNDS 0u
#endif
#define PW_SHARED __attribute__((section(".mc_shared"), aligned(64)))
#define PW_CASES 5u
#define PW_PHASES (2u * PW_CASES)

struct WitnessNestedSlot {
  uint32_t payload, visits;
  uint8_t pad8;
  ejit_may_const uint8_t field9;
  uint8_t pad10[334];
  ejit_may_const uint8_t field344;
  uint8_t pad345[7];
};
struct WitnessWideSlot {
  ejit_may_const uint64_t bits;
  uint32_t payload, visits;
  uint8_t pad16[48];
};
struct WitnessFlatSlot {
  uint32_t payload, visits;
  uint8_t pad8;
  ejit_may_const uint8_t field9;
  uint8_t pad10[21];
  ejit_may_const uint8_t field31;
  uint8_t pad32[96];
};
struct WitnessLargeSlot {
  uint32_t payload, visits;
  uint8_t pad8;
  ejit_may_const uint8_t field9;
  uint8_t pad10[1230];
};
struct WitnessNestedCell {
  struct WitnessNestedSlot slot[10];
};
struct WitnessWideCell {
  struct WitnessWideSlot slot[5];
};
struct WitnessFlatCell {
  struct WitnessFlatSlot slot[5];
};
struct WitnessLargeCell {
  struct WitnessLargeSlot slot[5];
};
struct WitnessLocalSlot {
  uint32_t payload, visits;
  uint8_t pad8[6];
  ejit_may_const uint8_t field14;
  uint8_t pad15[169];
};
struct WitnessLocalCell {
  struct WitnessLocalSlot slot[5];
};
struct WitnessInput {
  uint8_t pad[20];
  uint16_t slot;
};
_Static_assert(sizeof(struct WitnessLocalSlot) == 184 &&
                   offsetof(struct WitnessLocalSlot, field14) == 14 &&
                   offsetof(struct WitnessInput, slot) == 20,
               "Local slot witness must use load +20, stride184, field14");
_Static_assert(sizeof(struct WitnessNestedSlot) == 352 &&
                   offsetof(struct WitnessNestedSlot, field9) == 9 &&
                   offsetof(struct WitnessNestedSlot, field344) == 344 &&
                   sizeof(struct WitnessWideSlot) == 64 &&
                   sizeof(struct WitnessFlatSlot) == 128 &&
                   offsetof(struct WitnessFlatSlot, field9) == 9 &&
                   offsetof(struct WitnessFlatSlot, field31) == 31 &&
                   sizeof(struct WitnessLargeSlot) == 1240 &&
                   offsetof(struct WitnessLargeSlot, field9) == 9,
               "Witness strides and field offsets must match the test");

PW_SHARED struct WitnessNestedCell g_pr256_nested_cells[2];
PW_SHARED struct WitnessWideCell g_pr256_wide_cells[2];
PW_SHARED struct WitnessFlatCell g_pr256_flat_cells[2];
PW_SHARED struct WitnessLargeCell g_pr256_large_cells[2];
PW_SHARED struct WitnessLocalCell g_pr256_local_cells[2];
PW_SHARED ejit_period_arr(cell)
struct WitnessNestedCell *g_pr256_nested = g_pr256_nested_cells;
PW_SHARED ejit_period_arr(cell)
struct WitnessWideCell *g_pr256_wide = g_pr256_wide_cells;
PW_SHARED ejit_period_arr(cell)
struct WitnessFlatCell *g_pr256_flat = g_pr256_flat_cells;
PW_SHARED ejit_period_arr(cell)
struct WitnessLargeCell *g_pr256_large = g_pr256_large_cells;
PW_SHARED ejit_period_arr(cell)
struct WitnessLocalCell *g_pr256_local = g_pr256_local_cells;

ejit_entry uint64_t pr256_witness_nested(ejit_period_arr_ind(cell) uint8_t cell,
                                         ejit_free_dim uint16_t slot) {
  struct WitnessNestedCell *p = &g_pr256_nested[cell];
  struct WitnessNestedSlot *live = &p->slot[slot % 10u];
  ++live->visits;
  return p->slot[slot % 5u].field9 * 100u + live->field344 * 10000u +
         live->payload;
}
ejit_entry uint64_t pr256_witness_wide(ejit_period_arr_ind(cell) uint8_t cell,
                                       ejit_free_dim uint16_t slot) {
  struct WitnessWideSlot *p = &g_pr256_wide[cell].slot[slot % 5u];
  ++p->visits;
  uint64_t bits = p->bits;
  return (bits ^ ((bits & 131072u) ? 16777216u : 0u)) + p->payload;
}
ejit_entry uint64_t pr256_witness_flat128(ejit_period_arr_ind(cell)
                                              uint8_t cell,
                                          ejit_free_dim uint16_t slot) {
  struct WitnessFlatSlot *p = &g_pr256_flat[cell].slot[slot % 5u];
  ++p->visits;
  return p->field9 * 100u + p->field31 * 10000u + p->payload;
}
ejit_entry uint64_t pr256_witness_mul1240(ejit_period_arr_ind(cell)
                                              uint8_t cell,
                                          ejit_free_dim uint16_t slot) {
  struct WitnessLargeCell *cellp = &g_pr256_large[cell];
  uint16_t offset = (uint16_t)((slot % 5u) * 1240u);
  struct WitnessLargeSlot *p =
      (struct WitnessLargeSlot *)((unsigned char *)cellp + offset);
  ++p->visits;
  return p->field9 * 100u + p->payload;
}

ejit_entry uint64_t pr256_witness_local184(ejit_period_arr_ind(cell)
                                               uint8_t cell,
                                           const struct WitnessInput *input) {
  const uint16_t slot ejit_free_dim = input->slot;
  struct WitnessLocalCell *cellp = &g_pr256_local[cell];
  uint16_t offset = (uint16_t)((slot % 5u) * 184u);
  struct WitnessLocalSlot *p =
      (struct WitnessLocalSlot *)((unsigned char *)cellp + offset);
  ++p->visits;
  return p->field14 * 100u + p->payload;
}

typedef uint64_t (*pw_entry_t)(uint8_t, uint16_t);
typedef uint64_t (*pw_local_entry_t)(uint8_t, const struct WitnessInput *);
// Volatile indirection preserves actual generated wrapper calls at AOT O2.
static pw_entry_t volatile pw_wrappers[4] = {
    pr256_witness_nested, pr256_witness_wide, pr256_witness_flat128,
    pr256_witness_mul1240};
static pw_local_entry_t volatile pw_local_wrapper = pr256_witness_local184;
static const char *const pw_names[PW_CASES] = {
    "pr256_witness_nested", "pr256_witness_wide", "pr256_witness_flat128",
    "pr256_witness_mul1240", "pr256_witness_local184"};
static uint32_t pw_indices[PW_CASES], pw_cell_dim;
static uint32_t pw_expected_visits[PW_CASES][10];
enum { PW_RESET, PW_STARTING, PW_READY, PW_RUNNING, PW_DONE, PW_FAILED };
PW_SHARED uint32_t g_pr256_stage;
PW_SHARED struct {
  uint32_t checked, deferred, phases;
  uint32_t executed[PW_PHASES];
  void *first[PW_PHASES], *final[PW_PHASES];
} g_pr256_result;

static int pw_fail(const char *reason) {
  __atomic_store_n(&g_pr256_stage, PW_FAILED, __ATOMIC_RELEASE);
  SRE_printf("[PTR256] FAIL core=%u: %s; reset to retry; core6 may print\n",
             (unsigned)g_ucLocalCoreID, reason);
  return -1;
}
static int pw_delay(void) {
  return SRE_TaskDelay(1u) ? pw_fail("SRE_TaskDelay failed") : 0;
}
static int pw_stats(ejit_taskpool_stats_t *s) {
  if (ejit_taskpool_get_stats(s) != EJIT_OK)
    return pw_fail("taskpool stats unavailable");
  if (s->compileFailed || s->publishFailed || s->queueFull)
    return pw_fail("compile/publish/queue failure");
  return 0;
}
static int pw_idle(const ejit_taskpool_stats_t *s) {
  return !s->pendingEntries && !s->queueApproxSize &&
         !ejit_taskpool_pending_count();
}
// This standalone image completes all T2 work and stops its sole caller
// before updating. Queue drain alone is not a general compiler-borrow fence.
static int pw_wait_idle(void) {
  for (unsigned round = 0; !PR256_WAIT_ROUNDS || round < PR256_WAIT_ROUNDS;
       ++round) {
    ejit_taskpool_stats_t s = {0};
    if (pw_stats(&s))
      return -1;
    if (pw_idle(&s))
      return 0;
    if (round % 1000u == 0)
      SRE_printf("[PTR256] waiting drain pending=%u queue=%u\n",
                 s.pendingEntries, s.queueApproxSize);
    if (pw_delay())
      return -1;
  }
  return pw_fail("drain timeout; configuration NOT modified");
}
static unsigned pw_slots(unsigned which) { return which ? 5u : 10u; }
static uint32_t *pw_visits(unsigned which, unsigned cell, unsigned slot) {
  switch (which) {
  case 0:
    return &g_pr256_nested_cells[cell].slot[slot].visits;
  case 1:
    return &g_pr256_wide_cells[cell].slot[slot].visits;
  case 2:
    return &g_pr256_flat_cells[cell].slot[slot].visits;
  case 3:
    return &g_pr256_large_cells[cell].slot[slot].visits;
  default:
    return &g_pr256_local_cells[cell].slot[slot].visits;
  }
}
static uint32_t *pw_payload(unsigned which, unsigned cell, unsigned slot) {
  switch (which) {
  case 0:
    return &g_pr256_nested_cells[cell].slot[slot].payload;
  case 1:
    return &g_pr256_wide_cells[cell].slot[slot].payload;
  case 2:
    return &g_pr256_flat_cells[cell].slot[slot].payload;
  case 3:
    return &g_pr256_large_cells[cell].slot[slot].payload;
  default:
    return &g_pr256_local_cells[cell].slot[slot].payload;
  }
}
static uint64_t pw_bits(unsigned mode) {
  return UINT64_C(0x1234567800000000) | mode |
         (mode == 7u ? UINT64_C(131072) : 0u);
}
static uint64_t pw_expected(unsigned which, unsigned slot, unsigned mode,
                            unsigned extra) {
  uint64_t baked;
  if (which == 1u) {
    uint64_t bits = pw_bits(mode);
    baked = bits ^ ((mode == 7u) ? UINT64_C(16777216) : 0u);
  } else {
    baked = mode * 100u;
    if (which == 0u)
      baked += (mode + 1u) * 10000u;
    if (which == 2u)
      baked += (mode + 2u) * 10000u;
  }
  return baked + 1000u + which * 100u + slot % pw_slots(which) + extra;
}
static int pw_call(unsigned which, unsigned slot, unsigned mode, unsigned extra,
                   int wrapper, void **seen) {
  void *fn = 0;
  uint32_t bucket = 0;
  if (seen)
    *seen = 0;
  if (!wrapper) {
    const ejit_dim_pair_t dim = {pw_cell_dim, 1u};
    ejit_status_t rc =
        ejit_taskpool_compile_or_get(pw_indices[which], &dim, 1u, &fn, &bucket);
    if (rc == EJIT_PENDING || rc == EJIT_ERR_QUEUE_FULL) {
      if (fn)
        return pw_fail("fallback returned unexpected pointer");
      ++g_pr256_result.deferred;
      return 0;
    }
    if (rc != EJIT_OK || !fn)
      return pw_fail("lookup failed");
  }
  uint64_t actual;
  if (which == 4u) {
    const struct WitnessInput input = {{0}, (uint16_t)slot};
    actual = wrapper ? pw_local_wrapper(1u, &input)
                     : ((pw_local_entry_t)fn)(1u, &input);
  } else {
    actual = wrapper ? pw_wrappers[which](1u, (uint16_t)slot)
                     : ((pw_entry_t)fn)(1u, (uint16_t)slot);
  }
  if (!wrapper)
    ejit_taskpool_release_read(bucket);
  uint64_t expected = pw_expected(which, slot, mode, extra);
  if (actual != expected) {
    SRE_printf("[PTR256] case=%u slot=%u got=%llu want=%llu\n", which, slot,
               (unsigned long long)actual, (unsigned long long)expected);
    return pw_fail("functional/live-slot/full-width mismatch");
  }
  ++pw_expected_visits[which][slot % pw_slots(which)];
  for (unsigned i = 0; i < pw_slots(which); ++i)
    if (*pw_visits(which, 1u, i) != pw_expected_visits[which][i] ||
        *pw_visits(which, 0u, i) != 0u)
      return pw_fail("live store redirected to witness slot/cell");
  ++g_pr256_result.checked;
  if (seen)
    *seen = fn;
  return 1;
}
static int pw_generation(unsigned phase, unsigned mode) {
  unsigned which = phase % PW_CASES, executed = 0, warned = 0;
  void *first = 0, *last = 0;
  // No manual lookup/prewarm before the first real wrapper invocation.
  if (pw_call(which, 9u, mode, 0u, 1, 0) != 1)
    return -1;
  for (unsigned round = 0; !PR256_WAIT_ROUNDS || round < PR256_WAIT_ROUNDS;
       ++round) {
    void *fn = 0;
    int rc = pw_call(which, round % 10u, mode, 0u, 0, &fn);
    if (rc < 0)
      return -1;
    if (rc) {
      if (!first)
        first = fn;
      last = fn;
      ++executed;
    }
    ejit_taskpool_stats_t s = {0};
    if (pw_stats(&s))
      return -1;
    unsigned ready = phase < PW_CASES ? phase + 1u : PW_CASES;
    // Pending polls are not samples; T1/T2 addresses may legally be reused.
    if (executed >= 65u && first && last &&
        s.asyncCompiles == 2u * (phase + 1u) && s.readyEntries == ready &&
        pw_idle(&s)) {
      g_pr256_result.executed[phase] = executed;
      g_pr256_result.first[phase] = first;
      g_pr256_result.final[phase] = last;
      ++g_pr256_result.phases;
      SRE_printf("[PTR256] PHASE=%u case=%s mode=%u executed=%u "
                 "first=%p final=%p\n",
                 phase, pw_names[which], mode, executed, first, last);
      return 0;
    }
    if (executed >= 65u && !s.asyncCompiles && !warned) {
      warned = 1;
      SRE_printf("[PTR256] runtime counters missing: rebuild RUNTIME with "
                 "EJIT_STATS_ENABLE=ON; NOT acceptance\n");
    }
    if (round % 1000u == 0)
      SRE_printf("[PTR256] waiting phase=%u calls=%u compiles=%llu "
                 "ready=%u pending=%u queue=%u\n",
                 phase, executed, (unsigned long long)s.asyncCompiles,
                 s.readyEntries, s.pendingEntries, s.queueApproxSize);
    if (pw_delay())
      return -1;
  }
  return pw_fail("PGO publication timeout");
}
static void pw_set_data(unsigned mode) {
  for (unsigned cell = 0; cell < 2u; ++cell) {
    // Cell0 deliberately differs; every marked field is uniform over slots.
    unsigned value = cell ? mode : mode + 40u;
    for (unsigned slot = 0; slot < 10u; ++slot) {
      g_pr256_nested_cells[cell].slot[slot].field9 = value;
      g_pr256_nested_cells[cell].slot[slot].field344 = value + 1u;
      if (slot < 5u) {
        g_pr256_wide_cells[cell].slot[slot].bits = pw_bits(value);
        g_pr256_flat_cells[cell].slot[slot].field9 = value;
        g_pr256_flat_cells[cell].slot[slot].field31 = value + 2u;
        g_pr256_large_cells[cell].slot[slot].field9 = value;
        g_pr256_local_cells[cell].slot[slot].field14 = value;
      }
    }
    for (unsigned which = 0; which < PW_CASES; ++which)
      for (unsigned slot = 0; slot < pw_slots(which); ++slot) {
        *pw_payload(which, cell, slot) = cell * 1000u + which * 100u + slot;
        *pw_visits(which, cell, slot) = 0u;
        pw_expected_visits[which][slot] = 0u;
      }
  }
}
static int pw_live_payload(unsigned mode) {
  if (pw_wait_idle())
    return -1;
  for (unsigned which = 0; which < PW_CASES; ++which) {
    for (unsigned slot = 0; slot < pw_slots(which); ++slot)
      *pw_payload(which, 1u, slot) += 123u;
    for (unsigned slot = 0; slot < 10u; ++slot)
      if (pw_call(which, slot, mode, 123u, 1, 0) != 1)
        return -1;
    if (pw_call(which, 10u, mode, 123u, 1, 0) != 1 ||
        pw_call(which, 65535u, mode, 123u, 1, 0) != 1)
      return -1;
    for (unsigned slot = 0; slot < pw_slots(which); ++slot)
      *pw_payload(which, 1u, slot) -= 123u;
  }
  return 0;
}
static int pw_owner_workload(void) {
  if (ejit_activate("cell", 1u) != EJIT_OK)
    return pw_fail("activate failed");
  for (unsigned phase = 0; phase < PW_CASES; ++phase)
    if (pw_generation(phase, 7u))
      return -1;
  if (pw_live_payload(7u) || pw_wait_idle())
    return -1;
  if (ejit_deactivate("cell", 1u) != EJIT_OK)
    return pw_fail("deactivate failed; configuration NOT modified");
  if (pw_wait_idle())
    return -1;
  pw_set_data(19u);
  if (ejit_activate("cell", 1u) != EJIT_OK)
    return pw_fail("reactivate failed");
  for (unsigned phase = PW_CASES; phase < PW_PHASES; ++phase)
    if (pw_generation(phase, 19u))
      return -1;
  if (pw_live_payload(19u))
    return -1;
  __atomic_store_n(&g_pr256_stage, PW_DONE, __ATOMIC_RELEASE);
  SRE_printf("[PTR256] OWNER_DONE checked=%u deferred=%u; core6: "
             "test_ejit_pointer_witness_print; IR acceptance still required\n",
             g_pr256_result.checked, g_pr256_result.deferred);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  unsigned core = g_ucLocalCoreID;
  if (core != 6u && core != 16u)
    return -1;
  uint32_t stage = core == 6u ? PW_RESET : PW_READY;
  if (!__atomic_compare_exchange_n(&g_pr256_stage, &stage,
                                   core == 6u ? PW_STARTING : PW_RUNNING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[PTR256] rejected core=%u stage=%u; no re-init/retry\n", core,
               stage);
    return -1;
  }
#if PR256_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
  pw_cell_dim = ~0u;
  ejit_register_lifecycle("cell", &pw_cell_dim);
  for (unsigned i = 0; i < PW_CASES; ++i) {
    pw_indices[i] = ~0u;
    ejit_register_funcindex(pw_names[i], &pw_indices[i]);
    if (pw_indices[i] == ~0u)
      return pw_fail("function registration unresolved");
  }
  if (pw_cell_dim == ~0u)
    return pw_fail("cell registration unresolved");
  if (core == 6u)
    pw_set_data(7u);
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = cfg.forceStaticRegistry = true;
  if (ejit_init_pgo(&cfg) != EJIT_OK)
    return pw_fail("online PGO init failed");
  if (ejit_taskpool_get_worker_core() != 6u)
    return pw_fail("worker is not core6");
  ejit_taskpool_stats_t s = {0};
  if (pw_stats(&s))
    return -1;
  if (s.asyncCompiles || s.asyncEnqueues || s.cacheHits || s.readyEntries ||
      !pw_idle(&s) || ejit_is_active("cell", 1u))
    return pw_fail("fresh standalone image required, no other callers");
  if (core == 6u) {
    ejit_dump_func("*");
    __atomic_store_n(&g_pr256_stage, PW_READY, __ATOMIC_RELEASE);
    SRE_printf("[PTR256] WORKER_READY core6 cell=1 bounds=unknown "
               "waitRounds=%u (0=unlimited); core16: test_ejit_period\n",
               (unsigned)PR256_WAIT_ROUNDS);
    return 0;
  }
  return pw_owner_workload();
}

// Read-only: no reinitialization, registration, activation or failed-run PASS.
int test_ejit_pointer_witness_print(uint8_t a, uint8_t b, uint8_t c,
                                    uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != 6u)
    return -1;
  uint32_t stage = __atomic_load_n(&g_pr256_stage, __ATOMIC_ACQUIRE);
  if (stage != PW_DONE && stage != PW_FAILED) {
    SRE_printf("[PTR256] NOT_READY stage=%u; wait for OWNER_DONE or FAIL\n",
               stage);
    return -1;
  }
  ejit_taskpool_print_stats();
  ejit_taskpool_print_compiled();
  ejit_print_mayconst_ranking();
  for (unsigned i = 0; i < PW_CASES; ++i) {
    ejit_print_dumped(pw_names[i]);
    ejit_print_dumped_module(pw_names[i]);
  }
  ejit_taskpool_stats_t s = {0};
  int ok = stage == PW_DONE && ejit_taskpool_get_stats(&s) == EJIT_OK &&
           g_pr256_result.phases == PW_PHASES &&
           s.asyncCompiles == 2u * PW_PHASES && s.readyEntries == PW_CASES &&
           !s.compileFailed && !s.publishFailed && !s.queueFull &&
           pw_idle(&s) && ejit_taskpool_get_worker_core() == 6u;
  for (unsigned i = 0; i < PW_PHASES; ++i)
    ok &= g_pr256_result.executed[i] >= 65u && g_pr256_result.first[i] &&
          g_pr256_result.final[i];
  if (!ok) {
    SRE_printf("[PTR256] NOT_ACCEPTED stage=%u; retain partial diagnostics\n",
               stage);
    return -1;
  }
  SRE_printf("[PTR256] RUNTIME_CHECKS_PASS; FINAL_ACCEPTANCE_PENDING: "
             "pgo=1, ten real 64-entry audits, five published T2 modules "
             "entry_count=64; seven marked fields folded, LIVE payload/visits. "
             "OWNER_DONE alone is not acceptance. Reset to rerun.\n");
  return 0;
}
