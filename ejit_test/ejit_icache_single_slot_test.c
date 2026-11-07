//===-- ejit_icache_single_slot_test.c -------------------------------------===//
//
// EJIT single-slot-mode host smoke (x86, async shared pool, icache ON +
// -mllvm -ejit-icache-single-slot).
//
// The 3-dim entry below marks three ejit_dim params but its cell table is ONE
// scalar ptr (registered numDims=0): 8 bytes instead of 32 KB in the shared
// section, one drain store instead of 4096, one load instead of a 3-deep GEP
// chain. Its MissFn still resolves through the taskpool with the REAL
// (3-dim) tuple, so specialization identity and lifecycle retirement are
// unchanged from the dimensioned form.
//
// The observable contract this test pins:
//
//   1. Warm + fill: the first call finds the scalar cell still holding
//      &MissFn (the sentinel initializer) -> MissFn -> resolve (async compile
//      enqueued, AOT fallback answers). ejit_drain_taskpool() lets the worker
//      publish; the NEXT resolve is a taskpool cache hit which icacheFill's
//      cell 0 - with a CALLER dim count (3) that no longer has to match the
//      registered shape (0).
//   2. Frozen within a run: mutating the may_const source WITHOUT
//      invalidating keeps the old answer - the scalar cell served the cached
//      specialization; an AOT fallback execution would answer the new value.
//   3. CROSS-VALUE ISOLATION, the whole point of the mode's contract: with a
//      different shared value (a different "test"), the switch goes through
//      the lifecycle (deactivate old instance, activate new). Every toggle
//      drains the scalar cell back to &MissFn and retires the taskpool
//      entry, so the first call at the new value misses, resolves a FRESH
//      specialization, and answers from the NEW value's data -- never the
//      old run's specialization.
//   4. Frozen again within the new run.
//
// NOT asserted (deliberately): a bare same-run value switch with no boundary
// and no lifecycle toggle. Under the mode's contract that never happens; if
// it did, the scalar cell would serve the previous specialization by design
// of the guardless probe. That hazard is documented with the flag, not
// pinned here.
//
// TU layout: the entry lives in its OWN translation unit with its period
// arrays (see the note in ejit_sentinel_smoke_test.c); this harness only
// calls it and mutates the data through the exported setter.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>
#include <stdio.h>

#include "ejit_test_helpers.h"

// Entry + setter from the sibling TU.
extern uint32_t single_slot_3d(uint8_t i1, uint8_t i2, uint8_t i3);
extern void single_slot_set_3d(uint8_t i, uint32_t kind);

static int g_fail = 0;
#define T(cond, fmt, ...)                                                      \
  do {                                                                         \
    if (cond)                                                                  \
      printf("  OK   " fmt "\n", ##__VA_ARGS__);                               \
    else {                                                                     \
      printf("  FAIL " fmt "\n", ##__VA_ARGS__);                                \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

static const uint8_t kOld = 1; // first run's shared value
static const uint8_t kNew = 3; // second run's shared value

static void activate_all(uint8_t v) {
  T(ejit_activate("p1", v) == EJIT_OK, "activate p1[%u]", v);
  T(ejit_activate("p2", v) == EJIT_OK, "activate p2[%u]", v);
  T(ejit_activate("p3", v) == EJIT_OK, "activate p3[%u]", v);
}

static void deactivate_all(uint8_t v) {
  T(ejit_deactivate("p1", v) == EJIT_OK, "deactivate p1[%u]", v);
  T(ejit_deactivate("p2", v) == EJIT_OK, "deactivate p2[%u]", v);
  T(ejit_deactivate("p3", v) == EJIT_OK, "deactivate p3[%u]", v);
}

int main(void) {
  printf("=== EJIT Single-Slot Smoke (old=%u new=%u) ===\n\n", kOld, kNew);

  // Run 1: value kOld, data "on" -> answer 7. Run 2: value kNew, data "off"
  // -> answer 0. The two answers must differ, or the cross-value assertion
  // below proves nothing.
  single_slot_set_3d(kOld, 1u);
  single_slot_set_3d(kNew, 0u);

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  if (ejit_init(&cfg) != EJIT_OK) {
    printf("FAIL: ejit_init\n");
    return 2;
  }

  printf("--- run 1 (v=%u): warm -> fill -> frozen ---\n", kOld);
  activate_all(kOld);
  ejit_drain_taskpool();

  // Host nuance (same as the sentinel smoke's 0-dim note): this build wires
  // per-core prepareCodeFn_ (no 4K seal), so icacheFill DECLINES the shared
  // scalar cell here -- the assertions below then pass through the taskpool's
  // frozen cache (the scalar slot's keying, drain, and lifecycle contracts
  // are identical either way). On the 4K-seal board the fill goes through and
  // the probe serves the cell directly; the gtest
  // InlineCacheScalarSlotDeclinesUnderPerCorePrepare pins both shapes.
  uint32_t a = single_slot_3d(kOld, kOld, kOld); /* cell=&MissFn -> fallback */
  T(a == 7u, "run 1 warm = %u (expected 7)", a);
  ejit_drain_taskpool();
  uint32_t b = single_slot_3d(kOld, kOld, kOld); /* taskpool hit (fill where
                                                    the platform allows) */
  T(b == 7u, "run 1 fill = %u (expected 7)", b);

  single_slot_set_3d(kOld, 0u); /* no invalidation: scalar cell keeps old code */
  uint32_t c = single_slot_3d(kOld, kOld, kOld);
  T(c == 7u,
    "run 1 frozen = %u (expected 7; AOT fallback would answer 0)", c);
  single_slot_set_3d(kOld, 1u); /* restore */

  printf("--- switch runs via the lifecycle: v=%u -> v=%u ---\n", kOld, kNew);
  deactivate_all(kOld); /* every toggle drains the scalar cell to &MissFn */
  activate_all(kNew);

  uint32_t d = single_slot_3d(kNew, kNew, kNew); /* sentinel -> fresh resolve */
  T(d == 0u,
    "run 2 cross-value resolve = %u (expected 0; the old run's cached "
    "specialization would answer 7)",
    d);
  ejit_drain_taskpool();
  uint32_t e = single_slot_3d(kNew, kNew, kNew); /* new specialization filled */
  T(e == 0u, "run 2 refill = %u (expected 0)", e);

  printf("--- run 2 (v=%u): frozen again within the run ---\n", kNew);
  single_slot_set_3d(kNew, 1u); /* no invalidation */
  uint32_t f = single_slot_3d(kNew, kNew, kNew);
  T(f == 0u,
    "run 2 frozen = %u (expected 0; AOT fallback would answer 7)", f);
  single_slot_set_3d(kNew, 0u); /* restore */

  ejit_shutdown();
  printf("\n%s (%d failures)\n", g_fail ? "SINGLE-SLOT FAILED" : "SINGLE-SLOT PASSED",
         g_fail);
  return g_fail ? 1 : 0;
}
