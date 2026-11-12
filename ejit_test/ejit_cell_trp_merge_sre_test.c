// Fresh standalone image: core6 test_ejit_period; core16 test_ejit_period;
// core6 test_ejit_cell_trp_merge_print. No main and no default timeout.
// Compile with matching EJIT Clang -O2 -c -mllvm -ejit-final-version-merge
// and the normal wrapper options (e.g. -mllvm -ejit-inline-cache).
// Runtime: SWITCH_CASE + shared taskpool + NO_RECLAIM + code pool + stats.
#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#if !__has_attribute(ejit_entry) || !__has_attribute(ejit_dim) || \
    !__has_attribute(ejit_period_const) || !__has_attribute(ejit_in_period_array)
#error "Use matching EJIT Clang, not stock Clang"
#endif
#ifdef EJIT_DISABLE
#error "This demo requires real generated wrappers"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;
#define MERGE_SHARED __attribute__((section(".mc_shared"), aligned(64)))
#ifndef MERGE_RUN_INIT_ARRAY
#define MERGE_RUN_INIT_ARRAY 1
#endif
#ifndef MERGE_WAIT_ROUNDS
#define MERGE_WAIT_ROUNDS 0u
#endif
#ifndef MERGE_VERIFY_MODE
#define MERGE_VERIFY_MODE 0
#endif
struct MergeConfig {
  ejit_may_const uint32_t bias;
  uint32_t visits;
};
MERGE_SHARED struct MergeConfig merge_cells[16], merge_trps[16];
MERGE_SHARED ejit_period_arr(cell) struct MergeConfig *merge_cell = merge_cells;
MERGE_SHARED ejit_period_arr(trp) struct MergeConfig *merge_trp = merge_trps;

ejit_entry uint32_t cell_trp_merge_entry(ejit_period_arr_ind(cell) uint8_t cell,
                                        ejit_period_arr_ind(trp) uint8_t trp,
                                        uint32_t live) {
  ++merge_cell[cell].visits;
  uint32_t bias = merge_cell[cell].bias;
  if (bias & 1u)
    return bias + merge_trp[trp].bias + live;
  return bias * 3u + merge_trp[trp].bias + live;
}
typedef uint32_t (*merge_fn)(uint8_t, uint8_t, uint32_t);
static merge_fn volatile merge_wrapper = cell_trp_merge_entry;
static uint32_t merge_index, merge_cell_dim, merge_trp_dim;
enum { RESET, STARTING, READY, RUNNING, DONE, FAILED };
MERGE_SHARED uint32_t merge_stage;
MERGE_SHARED struct {
  uint32_t checked, phases;
  void *first_shared;
  void *late_shared;
  void *renewed_shared;
} merge_result;
static const uint8_t cells[3] = {0, 3, 7}, trps[3] = {2, 5, 9};

static int fail(const char *why) {
  __atomic_store_n(&merge_stage, FAILED, __ATOMIC_RELEASE);
  SRE_printf("[CELLTRP] FAIL core=%u: %s; reset before retry\n",
             (unsigned)g_ucLocalCoreID, why);
  return -1;
}
static int delay(void) {
  return SRE_TaskDelay(1u) ? fail("SRE_TaskDelay") : 0;
}
static uint32_t expected(unsigned cell, unsigned trp, uint32_t live) {
  uint32_t bias = merge_cells[cell].bias;
  return ((bias & 1u) ? bias : bias * 3u) + merge_trps[trp].bias + live;
}
static int wrapper_call(unsigned cell, unsigned trp, uint32_t live) {
  uint32_t before = merge_cells[cell].visits;
  if (merge_wrapper(cell, trp, live) != expected(cell, trp, live) ||
      merge_cells[cell].visits != before + 1u)
    return fail("wrapper result or live-store mismatch");
  ++merge_result.checked;
  return 0;
}
static int query_call(unsigned member, uint32_t live, void **address) {
  *address = 0;
  ejit_dim_pair_t dims[2] = {{merge_cell_dim, cells[member]},
                            {merge_trp_dim, trps[member]}};
  uint32_t bucket = 0;
  void *fn = 0;
  ejit_status_t rc = ejit_taskpool_compile_or_get(merge_index, dims, 2, &fn, &bucket);
  if (rc == EJIT_PENDING || rc == EJIT_ERR_QUEUE_FULL)
    return 0;
  if (rc != EJIT_OK || !fn)
    return fail("runtime lookup failed");
  uint32_t before = merge_cells[cells[member]].visits;
  uint32_t result = ((merge_fn)fn)(cells[member], trps[member], live);
  ejit_taskpool_release_read(bucket);
  if (result != expected(cells[member], trps[member], live) ||
      merge_cells[cells[member]].visits != before + 1u)
    return fail("JIT result or live-store mismatch");
  *address = fn;
  ++merge_result.checked;
  return 1;
}
static int stats(ejit_taskpool_stats_t *s) {
  if (ejit_taskpool_get_stats(s) != EJIT_OK || s->compileFailed ||
      s->publishFailed || s->queueFull)
    return fail("taskpool compile/publication failure");
  return 0;
}
static int phase(unsigned member, unsigned trained, unsigned shared_count,
                 void **shared) {
  // The very first request uses the actual generated wrapper, including AOT
  // dispatch path. No direct-API prewarm substitutes for this step.
  if (wrapper_call(cells[member], trps[member], 13))
    return -1;
  for (unsigned round = 0; !MERGE_WAIT_ROUNDS || round < MERGE_WAIT_ROUNDS;
       ++round) {
    void *fn = 0;
    if (query_call(member, round, &fn) < 0)
      return -1;
    ejit_taskpool_stats_t s = {0};
    if (stats(&s))
      return -1;
    if (fn && s.asyncCompiles == 2u * trained && !s.pendingEntries &&
        !s.queueApproxSize && !ejit_taskpool_pending_count()) {
      int same = 1;
      for (unsigned i = 0; i < shared_count; ++i) {
        void *other = 0;
        if (query_call(i, round + i, &other) < 0)
          return -1;
        same &= other == fn;
      }
      if (same) {
        *shared = fn;
        ++merge_result.phases;
        SRE_printf("[CELLTRP] PHASE=%u trained=%u shared=%u entry=%p\n",
                   merge_result.phases, trained, shared_count, fn);
        return 0;
      }
    }
    if (round % 1000u == 0)
      SRE_printf("[CELLTRP] waiting member=%u compiles=%llu ready=%u pending=%u\n",
                 member, (unsigned long long)s.asyncCompiles, s.readyEntries,
                 s.pendingEntries);
    if (delay())
      return -1;
  }
  return fail("optional wait limit reached; no PASS");
}
int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a; (void)b; (void)c; (void)d;
  unsigned core = g_ucLocalCoreID;
  if (core != 6u && core != 16u)
    return -1;
  uint32_t wanted = core == 6u ? RESET : READY;
  if (!__atomic_compare_exchange_n(&merge_stage, &wanted,
        core == 6u ? STARTING : RUNNING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return -1;
#if MERGE_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
#ifdef MERGE_HOST_SINGLE_IMAGE
  if (core == 6u) {
#endif
    merge_index = merge_cell_dim = merge_trp_dim = ~0u;
    ejit_register_lifecycle("cell", &merge_cell_dim);
    ejit_register_lifecycle("trp", &merge_trp_dim);
    ejit_register_funcindex("cell_trp_merge_entry", &merge_index);
#ifdef MERGE_HOST_SINGLE_IMAGE
  }
#endif
  if (merge_index == ~0u || merge_cell_dim == ~0u || merge_trp_dim == ~0u)
    return fail("registration missing");
  if (core == 6u)
    for (unsigned i = 0; i < 16; ++i) {
      merge_cells[i].bias = 11u + i;
      merge_trps[i].bias = 37u + 2u * i;
    }
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = cfg.forceStaticRegistry = true;
  if (ejit_set_final_version_merge(1) != EJIT_OK)
    return fail("merge init failed");
  ejit_status_t init = MERGE_VERIFY_MODE ? ejit_init_verify_pgo(&cfg)
                                       : ejit_init_pgo(&cfg);
  if (init != EJIT_OK || ejit_taskpool_get_worker_core() != 6u)
    return fail("merge/PGO init or worker core mismatch");
  ejit_taskpool_stats_t s = {0};
  if (stats(&s) || s.asyncCompiles || s.asyncEnqueues || s.readyEntries)
    return fail("fresh standalone image required");
  if (core == 6u) {
    ejit_dump_func("cell_trp_merge_entry");
    __atomic_store_n(&merge_stage, READY, __ATOMIC_RELEASE);
    SRE_printf("[CELLTRP] WORKER_READY; core16 test_ejit_period; wait=unlimited by default\n");
    return 0;
  }
  // Inactive calls must remain correct AOT and must not enqueue compilation.
  if (wrapper_call(0, 2, 1) || stats(&s) || s.asyncEnqueues)
    return fail("inactive call compiled");
  for (unsigned i = 0; i < 3; ++i) {
    ejit_activate("cell", cells[i]);
    ejit_activate("trp", trps[i]);
  }
  void *single = 0;
  if (phase(0, 1, 1, &single) ||
      phase(1, 2, 2, &merge_result.first_shared))
    return -1;
  // Directly exercise the merged function's unvisited-tuple default without
  // training that member. Same original arguments, real live store.
  unsigned before = merge_cells[7].visits;
  if (((merge_fn)merge_result.first_shared)(7, 9, 23) != expected(7, 9, 23) ||
      merge_cells[7].visits != before + 1u)
    return fail("unvisited tuple did not preserve AOT behavior");
  if (phase(2, 3, 3, &merge_result.late_shared))
    return -1;
  // This standalone workload has stopped all callers and finished all member
  // compiles. Product code must use its real configuration synchronization.
  ejit_deactivate("cell", 0);
  if (wrapper_call(0, 2, 29))
    return -1;
  merge_cells[0].bias = 20;
  ejit_activate("cell", 0);
  if (phase(0, 4, 3, &merge_result.renewed_shared))
    return -1;
#if MERGE_VERIFY_MODE
  // Standalone diagnostic negative: no concurrent writer, restore afterwards.
  // Verifier executes live loads, so output stays correct while mismatch rises.
  ejit_verify_stats_t initial = {0}, changed = {0};
  ejit_verify_get_stats(&initial);
  if (!initial.checks || initial.mismatches)
    return fail("verifier has no coverage or unexpected mismatch");
  uint32_t saved = merge_cells[3].bias;
  merge_cells[3].bias = saved + 2u;
  void *probe = 0;
  int probed = query_call(1, 43, &probe);
  merge_cells[3].bias = saved;
  ejit_verify_get_stats(&changed);
  if (probed != 1 || probe != merge_result.renewed_shared ||
      changed.checks <= initial.checks || changed.mismatches <= initial.mismatches)
    return fail("merged arm did not detect deliberate verifier mismatch");
  SRE_printf("[CELLTRP] VERIFY_PASS checks=%llu intentional_mismatches=%llu\n",
             (unsigned long long)changed.checks,
             (unsigned long long)changed.mismatches);
#endif
  __atomic_store_n(&merge_stage, DONE, __ATOMIC_RELEASE);
  SRE_printf("[CELLTRP] OWNER_DONE checks=%u; core6 test_ejit_cell_trp_merge_print\n",
             merge_result.checked);
  return 0;
}
int test_ejit_cell_trp_merge_print(void) {
  if (g_ucLocalCoreID != 6u)
    return -1;
  uint32_t stage = __atomic_load_n(&merge_stage, __ATOMIC_ACQUIRE);
  ejit_taskpool_print_stats();
  ejit_taskpool_print_compiled();
  ejit_print_dumped("cell_trp_merge_entry");
  ejit_print_dumped_module("cell_trp_merge_entry");
  if (stage != DONE || merge_result.phases != 4u)
    return -1;
  SRE_printf("[CELLTRP] FUNCTIONAL_PASS phases=4; inspect one merged entry, different constant arms, AOT default; performance not measured\n");
  return 0;
}
