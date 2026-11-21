//===-- ejit_test_helpers.h - shared EJIT test utilities -------------------===//
//
// Shared test-side utilities for the ejit_test/*.c integration tests.
//
// Types, attribute macros, enums, and runtime API declarations all come from
// llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h (the canonical header).
// Tests use the macro attribute form (ejit_entry, ejit_group(cell), ...)
// exclusively — never the raw __attribute__((...)) form — so including
// EJitRuntime.h (which #defines those macros) does not cause double-expansion.
//
// This header adds only two test conveniences on top:
//   * ejit_default_config  — fill ejit_config_t with the canonical test
//                            defaults (ASYNC under EJIT_SRE_SHARED_TASKPOOL,
//                            SYNC otherwise).
//   * ejit_drain_taskpool  — bounded spin until the async worker has published
//                            all in-flight compiles (no-op in non-taskpool
//                            builds; must not reference taskpool symbols there
//                            or the sync test suite fails to link).
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
//
// Only compiled when EJIT_SRE_SHARED_TASKPOOL is defined: the async path is the
// only one that produces pending work.  In a sync build ejit_drain_taskpool is
// a no-op (see below) and must NOT reference ejit_taskpool_pending_count — that
// symbol only exists in a taskpool runtime, so referencing it unconditionally
// breaks linking the sync test suite.
#ifdef EJIT_SRE_SHARED_TASKPOOL
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
#else
// Sync / non-taskpool build: no async worker, nothing to drain.  Intentionally
// references no taskpool symbols so the test links against a non-taskpool
// runtime.
static inline void ejit_drain_taskpool(void) {}
#endif

// Fill `cfg` with the canonical async defaults used by the integration tests.
// compileMode is ASYNC under EJIT_SRE_SHARED_TASKPOOL (required to start the
// single owner worker during ejit_init), SYNC otherwise.
static inline void ejit_default_config(ejit_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
#ifdef EJIT_SRE_SHARED_TASKPOOL
  cfg->compileMode = EJIT_COMPILE_ASYNC;
#else
  cfg->compileMode = EJIT_COMPILE_SYNC;
#endif
  cfg->optLevel = EJIT_OPT_L2;
  cfg->maxCodeMemory = 512 * 1024;
  cfg->maxDataMemory = 256 * 1024;
  cfg->maxCacheEntries = 64;
  cfg->maxCacheSize = 1024 * 1024;
}

#ifdef __cplusplus
}
#endif

#endif // EJIT_TEST_HELPERS_H
