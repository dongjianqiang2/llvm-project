//===-- ejit_test_helpers.h - shared EJIT test utilities -------------------===//
//
// Shared test-side utilities for the ejit_test/*.c integration tests.
//
// Types, attribute macros, enums, and runtime API declarations all come from
// llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h (the canonical header).
// Tests use the macro attribute form (ejit_entry, ejit_period_arr(cell), ...)
// exclusively — never the raw __attribute__((...)) form — so including
// EJitRuntime.h (which #defines those macros) does not cause double-expansion.
//
// This header adds only two test conveniences on top:
//   * ejit_default_config selects async compilation.
//   * ejit_drain_taskpool waits for in-flight shared compiles.
//
//===----------------------------------------------------------------------===//

#ifndef EJIT_TEST_HELPERS_H
#define EJIT_TEST_HELPERS_H

#include <string.h>

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

#ifdef __cplusplus
extern "C" {
#endif

//===-- Test helpers -------------------------------------------------------===//

// Spin until the taskpool has no pending work, bounded by maxSpins.
// Returns 1 if drained (pending hit 0), 0 on timeout.
//
// The unbounded `while (pending > 0);` previously used in the tests would hang
// forever if the worker never starts (e.g. an EJIT_FREESTANDING build whose
// SRE_TaskCreate is undefined) instead of failing the test.  The bounded form
// lets a caller turn a timeout into a clear VERIFY failure.
static inline int ejit_drain_taskpool_bounded(uint32_t maxSpins) {
  for (uint32_t i = 0; i < maxSpins; ++i) {
    if (ejit_taskpool_pending_count() == 0)
      return 1;
  }
  return 0;
}

// Default drain: generous spin budget.  Workers finish a compile in well under
// this many polls; a timeout indicates the worker is not running.
#define EJIT_DRAIN_SPIN_BUDGET 1000000u
static inline void ejit_drain_taskpool(void) {
  (void)ejit_drain_taskpool_bounded(EJIT_DRAIN_SPIN_BUDGET);
}

// Fill `cfg` with the canonical async defaults used by the integration tests.
static inline void ejit_default_config(ejit_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->compileMode = EJIT_COMPILE_ASYNC;
  cfg->optLevel = EJIT_OPT_L2;
}

#ifdef __cplusplus
}
#endif

#endif // EJIT_TEST_HELPERS_H
