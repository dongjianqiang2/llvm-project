//===-- ejit_fused_verify_sre_test.c - PR260 verifier: core6/16/6 ---------===//
// Fresh standalone image. Default: normal online PGO, no wait deadline.
// core6: test_ejit_period; core16: test_ejit_period;
// core6: test_ejit_fused_verify_print.
// This intentionally violates may_const after T2 publication to prove that
// the verifier detects it. Never run this mutation workload in production.
// Build runtime with EJIT_VERIFY_SUBSTITUTION=ON, EJIT_STATS_ENABLE=ON and
// EJIT_SRE_PGO_BRANCH_AUDIT=ON. No added SDK bindings. See the companion doc.

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#if !__has_attribute(ejit_entry) || !__has_attribute(ejit_dim) || \
    !__has_attribute(ejit_period_const) || !__has_attribute(ejit_in_period_array) || \
    !__has_attribute(ejit_free_dim)
#error "Use the matching EJIT Clang"
#endif
#ifdef EJIT_DISABLE
#error "This demo requires EJIT annotations"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include <stddef.h>

extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef VERIFY260_RUN_INIT_ARRAY
#define VERIFY260_RUN_INIT_ARRAY 1
#endif
#ifndef VERIFY260_ONLINE_PGO
#define VERIFY260_ONLINE_PGO 1
#endif
#define V_SHARED __attribute__((section(".mc_shared"), aligned(64)))
#define V_CELL 2u
#define V_SLOTS 5u
#define V_COUNT (16u * V_SLOTS)

struct Verify260Record {
  ejit_may_const uint64_t bits;
  uint32_t payload;
  uint32_t pad;
};
_Static_assert(sizeof(struct Verify260Record) == 16 &&
                   sizeof(ejit_config_t) == 56 &&
                   sizeof(ejit_taskpool_stats_t) == 88,
               "Use matching spec5 headers and 64-bit target");
V_SHARED struct Verify260Record g_verify260_data[V_COUNT];
V_SHARED struct Verify260Record g_verify260_alternate[V_COUNT];
V_SHARED ejit_period_arr(cell)
struct Verify260Record *g_verify260_root = g_verify260_data;

ejit_entry uint64_t
verify260_probe(ejit_period_arr_ind(cell) uint8_t cell,
                ejit_free_dim uint16_t slot) {
  struct Verify260Record *p =
      g_verify260_root + (cell * V_SLOTS + slot % V_SLOTS);
  return p->bits + p->payload;
}

enum { V_RESET, V_STARTING, V_READY, V_RUNNING, V_DONE, V_FAILED };
V_SHARED uint32_t g_verify260_stage;
V_SHARED uint32_t g_verify260_pass;
static uint32_t v_dim, v_func;

static int v_fail(const char *Why) {
  __atomic_store_n(&g_verify260_stage, V_FAILED, __ATOMIC_RELEASE);
  SRE_printf("[VERIFY260] FAIL core=%u: %s; core6 may print; reset to retry\n",
             (unsigned)g_ucLocalCoreID, Why);
  return -1;
}
static int v_stats(ejit_taskpool_stats_t *S) {
  if (ejit_taskpool_get_stats(S) != EJIT_OK || S->compileFailed ||
      S->publishFailed || S->queueFull)
    return v_fail("taskpool unavailable or compile/publish/queue failure");
  return 0;
}
static int v_idle(const ejit_taskpool_stats_t *S) {
  return !S->pendingEntries && !S->queueApproxSize &&
         !ejit_taskpool_pending_count();
}
// A pending/AOT call is not a verifier execution or a PGO sample.
static int v_call(uint16_t Slot, uint64_t Expected) {
  ejit_dim_pair_t D = {v_dim, V_CELL};
  void *Fn = NULL;
  uint32_t Bucket = 0;
  ejit_status_t Status =
      ejit_taskpool_compile_or_get(v_func, &D, 1, &Fn, &Bucket);
  if (Status == EJIT_PENDING)
    return 0;
  if (Status != EJIT_OK || !Fn)
    return v_fail("compile_or_get failed");
  uint64_t Actual = ((uint64_t (*)(uint8_t, uint16_t))Fn)(V_CELL, Slot);
  ejit_taskpool_release_read(Bucket);
  if (Actual != Expected)
    return v_fail("live value incorrect (load may have been frozen)");
  return 1;
}

static int v_owner(void) {
  if (ejit_activate("cell", V_CELL) != EJIT_OK)
    return v_fail("activate failed");
  const unsigned TargetCompiles = VERIFY260_ONLINE_PGO ? 2u : 1u;
  const unsigned TargetExecutions = VERIFY260_ONLINE_PGO ? 65u : 5u;
  unsigned Executions = 0;
  for (uint64_t Round = 0;; ++Round) {
    uint16_t Slot = (uint16_t)(Round % V_SLOTS);
    int Result = v_call(Slot, 7u + Slot);
    if (Result < 0)
      return -1;
    Executions += Result != 0;
    ejit_taskpool_stats_t S = {0};
    if (v_stats(&S))
      return -1;
    if (Executions >= TargetExecutions && S.asyncCompiles == TargetCompiles &&
        S.readyEntries == 1u && v_idle(&S))
      break;
    if (Round % 1000u == 0)
      SRE_printf("[VERIFY260] waiting executed=%u compiles=%llu "
                 "pending=%u queue=%u (no deadline)\n", Executions,
                 (unsigned long long)S.asyncCompiles, S.pendingEntries,
                 S.queueApproxSize);
    if (SRE_TaskDelay(1u))
      return v_fail("delay failed");
  }
  ejit_verify_stats_t Before = {0}, After = {0};
  ejit_verify_get_stats(&Before);
  if (!Before.sites || Before.checks < Executions || Before.mismatches)
    return v_fail("positive phase lacks coverage or has a mismatch");

  // Sole caller stopped and final code published. No compile or call overlaps
  // these deliberate contract violations; real product updates must drain.
  ejit_set_log_level(0); // no verifier mismatch printing on the business core
  g_verify260_data[V_CELL * V_SLOTS + 4u].bits = 9u;
  if (v_call(4u, 13u) != 1)
    return v_fail("non-witness-slot check failed");
  ejit_verify_get_stats(&After);
  if (After.checks != Before.checks + 1u ||
      After.mismatches != Before.mismatches + 1u)
    return v_fail("non-witness-slot mismatch was not detected exactly once");
  g_verify260_root = g_verify260_alternate;
  if (v_call(3u, 14u) != 1)
    return v_fail("live pointer swap check failed");
  ejit_verify_get_stats(&After);
  if (After.checks != Before.checks + 2u ||
      After.mismatches != Before.mismatches + 2u)
    return v_fail("pointer swap mismatch was not detected exactly once");
  if (ejit_deactivate("cell", V_CELL) != EJIT_OK)
    return v_fail("deactivate failed");
  g_verify260_pass = 1u;
  __atomic_store_n(&g_verify260_stage, V_DONE, __ATOMIC_RELEASE);
  SRE_printf("[VERIFY260] OWNER_DONE positive checks=%llu; two deliberate "
             "mismatches detected; core6: test_ejit_fused_verify_print\n",
             (unsigned long long)Before.checks);
  return 0;
}

int test_ejit_period(uint8_t A, uint8_t B, uint8_t C, uint8_t D) {
  (void)A; (void)B; (void)C; (void)D;
  unsigned Core = g_ucLocalCoreID;
  if (Core != 6u && Core != 16u)
    return -1;
  uint32_t Expected = Core == 6u ? V_RESET : V_READY;
  if (!__atomic_compare_exchange_n(&g_verify260_stage, &Expected,
                                   Core == 6u ? V_STARTING : V_RUNNING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[VERIFY260] rejected core=%u stage=%u; fresh image required\n",
               Core, Expected);
    return -1;
  }
#if VERIFY260_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
  v_dim = v_func = ~0u;
  ejit_register_lifecycle("cell", &v_dim);
  ejit_register_funcindex("verify260_probe", &v_func);
  if (v_dim == ~0u || v_func == ~0u)
    return v_fail("registration unresolved");
  if (Core == 6u) {
    g_verify260_root = g_verify260_data;
    for (unsigned I = 0; I < V_COUNT; ++I) {
      g_verify260_data[I].bits = I / V_SLOTS == V_CELL ? 7u : 31u;
      g_verify260_data[I].payload = I % V_SLOTS;
      g_verify260_alternate[I].bits = 11u;
      g_verify260_alternate[I].payload = I % V_SLOTS;
    }
    ejit_verify_reset_stats();
  }
  ejit_config_t Cfg = {0};
  Cfg.compileMode = EJIT_COMPILE_ASYNC;
  Cfg.optLevel = EJIT_OPT_L2;
  Cfg.enableLogger = Cfg.forceStaticRegistry = true;
  ejit_status_t Status = VERIFY260_ONLINE_PGO ? ejit_init_verify_pgo(&Cfg)
                                            : ejit_init_verify(&Cfg);
  if (Status != EJIT_OK || ejit_taskpool_get_worker_core() != 6u)
    return v_fail("verifier init failed or worker is not core6");
  ejit_set_log_level(Core == 6u ? 1 : 0);
  ejit_taskpool_stats_t S = {0};
  if (v_stats(&S))
    return -1;
  if (S.asyncCompiles || S.asyncEnqueues || S.readyEntries || !v_idle(&S) ||
      ejit_is_active("cell", V_CELL))
    return v_fail("fresh standalone runtime required");
  if (Core == 6u) {
    ejit_dump_func("verify260_probe");
    __atomic_store_n(&g_verify260_stage, V_READY, __ATOMIC_RELEASE);
    SRE_printf("[VERIFY260] WORKER_READY pgo=%u; core16: test_ejit_period\n",
               (unsigned)VERIFY260_ONLINE_PGO);
    return 0;
  }
  return v_owner();
}

int test_ejit_fused_verify_print(uint8_t A, uint8_t B, uint8_t C, uint8_t D) {
  (void)A; (void)B; (void)C; (void)D;
  if (g_ucLocalCoreID != 6u)
    return -1;
  uint32_t Stage = __atomic_load_n(&g_verify260_stage, __ATOMIC_ACQUIRE);
  ejit_verify_dump();
  ejit_taskpool_print_stats();
  ejit_print_dumped("verify260_probe");
  int Pass = Stage == V_DONE && g_verify260_pass == 1u;
  SRE_printf("[VERIFY260] %s (stage=%u); deliberate mismatches are expected\n",
             Pass ? "PASS" : "NOT_PASS", Stage);
  return Pass ? 0 : -1;
}
