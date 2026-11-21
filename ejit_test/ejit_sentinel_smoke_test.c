//===-- ejit_sentinel_smoke_test.c ----------------------------------------===//
//
// EJIT sentinel-wrapper host smoke (x86, async shared pool, icache ON).
//
// Built with -mllvm -ejit-inline-cache. For NumDims <= 2 the AOT emits the
// BRANCHLESS wrapper: the cell table is DEFINED pre-filled with &MissFn and
// the hot path is one load + musttail BLR with no null guard. This test
// walks that wrapper through its whole lifecycle and checks the observable
// contract end to end:
//
//   1. Warm: the first call finds the cell still holding &MissFn -> the BLR
//      lands in MissFn -> taskpool resolve (async compile enqueued, AOT
//      fallback answers). ejit_drain_taskpool() lets the worker publish; the
//      NEXT resolve is a taskpool cache hit which icacheFill's the cell.
//   2. Frozen: mutating the period_const source WITHOUT invalidating keeps the
//      old (compile-time) answer -- the BLR served the cached
//      specialization; an AOT fallback execution would answer the new value.
//   3. Re-resolve: a period toggle drains the cells -- to the &MissFn
//      sentinel for the branchless tables, 0 for the guarded one -- bumps
//      the instance version, and retires the taskpool entry, so the next
//      call misses, resolves fresh, and re-freezes the NEW values.
//
// The lifecycle runs on a 1-dim entry ([16] table, sentinel form) and a
// 3-dim entry ([16]^3 table, GUARDED form: the wrapper keeps its null
// guard and drain writes 0 -- the only shape the sentinel does not cover).
//
// The 0-dim entry runs a REDUCED lifecycle: its specialization key has no
// dims, so no lifecycle event can retire its taskpool entry -- once
// compiled it is frozen by design (a static period_const source has no time
// window in which it may change; give the entry a dim if it must
// re-specialize). ejit_clear_cache() still drains the scalar cell back to
// &MissFn, so the observable contract there is: the next call re-enters
// MissFn (branchless round-trip through the sentinel) and the taskpool
// cache-hit re-serves the SAME frozen body.
//
// TU layout: each entry lives in its OWN translation unit with the period
// arrays it specializes on. A TU's registered bitcode is the JIT's
// specialization unit: compiling an entry JITs that whole blob under the
// entry's OWN dim context, so a sibling entry with a different dim
// signature would keep an unfoldable period-indexed load in the blob and
// the module could not link (on a host whose JIT slab sits >2GB from the
// AOT data, any leftover AOT-global reference is unrepresentable in a
// rip-relative Delta32). One entry shape per TU is also the embedded
// deployment shape. The 0-dim source is a plain (non-period) struct, not a
// bare scalar: EJIT_PERIOD_CONST applies to struct FIELDS only - Sema drops
// it on a VarDecl - and ejit_group(static) registers the struct so the
// JIT can resolve its address when freezing the field.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "ejit_test_helpers.h"

extern void ejit_clear_cache(void);

// Entry functions from the sibling TUs (plain externs; the wrappers, their
// dim attributes, and their icache cells live with the defining TUs).
extern uint32_t sentinel_1d(uint8_t i0);
extern uint32_t guarded_3d(uint8_t i1, uint8_t i2, uint8_t i3);
// period_const setters exported by the sibling TUs (the period arrays are
// private to their TUs; the harness mutates them through these).
extern void sentinel_smoke_set_p0(uint8_t i, uint32_t kind);
extern void sentinel_smoke_set_3d(uint8_t i, uint32_t kind);

//===-- Data ---------------------------------------------------------------===//

struct ScalarCfg {
  EJIT_PERIOD_CONST uint32_t kind; // 1 -> "on" branch, 0 -> "off"
};
ejit_group(static) struct ScalarCfg g_scalar = {1};

//===-- Entry (0-dim) -------------------------------------------------------===//
// noinline keeps the wrapper dispatch out of the callers.

// 0-dim: scalar sentinel cell. on -> 10, off -> 20.
ejit_entry __attribute__((noinline)) uint32_t sentinel_0d(void) {
  return (g_scalar.kind == 1u) ? 10u : 20u;
}

//===-- Harness ------------------------------------------------------------===//

static int g_fail = 0;
#define T(cond, fmt, ...)                                                      \
  do {                                                                         \
    if (cond)                                                                  \
      printf("  OK   " fmt "\n", ##__VA_ARGS__);                               \
    else {                                                                     \
      printf("  FAIL " fmt "\n", ##__VA_ARGS__);                               \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

static uint8_t cellIdx = 1;

static void set_scalar(uint32_t kind) { g_scalar.kind = kind; }
static void set_p0(uint32_t kind) { sentinel_smoke_set_p0(cellIdx, kind); }
static void set_all3(uint32_t kind) { sentinel_smoke_set_3d(cellIdx, kind); }

// One lifecycle, parametrized by the call, its two answers (source "on" /
// "off"), the invalidation to run between them, and the source setter.
#define LIFECYCLE(label, call_expr, v_on, v_off, invalidate_expr, set_kind)    \
  do {                                                                         \
    printf("--- %s: warm (cell=&MissFn -> BLR -> MissFn -> resolve) ---\n",    \
           label);                                                             \
    uint32_t _a = (call_expr); /* PENDING -> AOT fallback */                   \
    T(_a == (v_on), "%s warm = %u (expected %u)", label, _a, v_on);            \
    ejit_drain_taskpool();                                                     \
    uint32_t _b = (call_expr); /* taskpool hit -> icacheFill */                \
    T(_b == (v_on), "%s fill = %u (expected %u)", label, _b, v_on);            \
                                                                               \
    printf("--- %s: hot hit serves the frozen specialization ---\n", label);   \
    set_kind(0u); /* no invalidation: the cell keeps the old code */           \
    uint32_t _c = (call_expr); /* icache hit: BLR the cached body */           \
    T(_c == (v_on), "%s frozen = %u (expected %u; AOT fallback would answer "  \
                  "%u)", label, _c, v_on, v_off);                              \
                                                                               \
    printf("--- %s: invalidation drains -> miss -> re-resolve ---\n", label);  \
    set_kind(1u); /* restore, then flip the values the NEXT compile freezes */ \
    invalidate_expr; /* drains every cell to its empty value */                \
    set_kind(0u);                                                              \
    uint32_t _d = (call_expr); /* empty cell -> MissFn -> fresh resolve */     \
    T(_d == (v_off), "%s post-drain resolve = %u (expected %u)", label, _d,    \
      v_off);                                                                  \
    ejit_drain_taskpool();                                                     \
    uint32_t _e = (call_expr); /* new specialization cached + filled */        \
    T(_e == (v_off), "%s refilled = %u (expected %u)", label, _e, v_off);      \
                                                                               \
    set_kind(1u); /* restore for the next lifecycle */                         \
  } while (0)

int main(int argc, char **argv) {
  if (argc >= 2)
    cellIdx = (uint8_t)atoi(argv[1]);
  if (cellIdx >= 4) {
    printf("cellIdx must be < 4\n");
    return 2;
  }

  printf("=== EJIT Sentinel Wrapper Smoke (cellIdx=%u) ===\n\n", cellIdx);

  sentinel_smoke_set_p0(cellIdx, 1);
  sentinel_smoke_set_3d(cellIdx, 1);

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  if (ejit_init(&cfg) != EJIT_OK) {
    printf("FAIL: ejit_init\n");
    return 2;
  }

  // The dimensioned entries resolve only while their period instance is
  // active. The 0-dim entry has no period and needs no activation.
  T(ejit_activate("p0", cellIdx) == EJIT_OK, "activate p0[%u]", cellIdx);
  T(ejit_activate("p1", cellIdx) == EJIT_OK, "activate p1[%u]", cellIdx);
  T(ejit_activate("p2", cellIdx) == EJIT_OK, "activate p2[%u]", cellIdx);
  T(ejit_activate("p3", cellIdx) == EJIT_OK, "activate p3[%u]", cellIdx);
  ejit_drain_taskpool();

  // 0-dim: scalar cell, reduced lifecycle. icacheFill declines 0-dim cells
  // on platforms that prepare execute permission per core (one cell shared
  // by every core), so every call resolves through MissFn; the cell's
  // &MissFn initializer is still what makes that safe. ejit_clear_cache
  // drains the cell back to &MissFn but cannot retire the dimension-less
  // taskpool entry, so the post-drain resolve re-serves the frozen body.
  printf("--- sentinel_0d: warm (cell=&MissFn -> BLR -> MissFn -> resolve) ---\n");
  {
    uint32_t a = sentinel_0d(); /* cell=&MissFn -> MissFn -> fallback */
    T(a == 10u, "sentinel_0d warm = %u (expected 10)", a);
    ejit_drain_taskpool();
    uint32_t b = sentinel_0d(); /* taskpool hit (fill declined) */
    T(b == 10u, "sentinel_0d fill = %u (expected 10)", b);

    printf("--- sentinel_0d: hot hit serves the frozen specialization ---\n");
    set_scalar(0u);
    uint32_t c = sentinel_0d();
    T(c == 10u,
      "sentinel_0d frozen = %u (expected 10; AOT fallback would answer 20)",
      c);
    set_scalar(1u); /* restore */

    printf("--- sentinel_0d: clear_cache drains the cell; the dimension-less "
           "entry re-serves the frozen body ---\n");
    ejit_clear_cache(); /* scalar cell -> &MissFn */
    set_scalar(0u);
    uint32_t d = sentinel_0d(); /* &MissFn -> MissFn -> taskpool hit */
    T(d == 10u,
      "sentinel_0d post-drain resolve = %u (expected 10: no dimension to "
      "version, so the frozen body re-serves; fresh re-compile would answer "
      "20)",
      d);
    ejit_drain_taskpool();
    uint32_t e = sentinel_0d();
    T(e == 10u, "sentinel_0d refilled = %u (expected 10)", e);
    set_scalar(1u); /* restore */
  }

  // 1-dim: p0 toggle drains (the [16] table's cell goes back to &MissFn).
  LIFECYCLE("sentinel_1d", sentinel_1d(cellIdx), 100u + cellIdx,
            200u + cellIdx,
            (ejit_deactivate("p0", cellIdx), ejit_activate("p0", cellIdx)),
            set_p0);

  // 3-dim: guarded form - same observable lifecycle, drain writes 0 and the
  // wrapper null-guards it.
  LIFECYCLE("guarded_3d", guarded_3d(cellIdx, cellIdx, cellIdx), 7u, 0u,
            (ejit_deactivate("p1", cellIdx), ejit_activate("p1", cellIdx)),
            set_all3);

  ejit_shutdown();
  printf("\n%s (%d failures)\n", g_fail ? "SMOKE FAILED" : "SMOKE PASSED",
         g_fail);
  return g_fail ? 1 : 0;
}
