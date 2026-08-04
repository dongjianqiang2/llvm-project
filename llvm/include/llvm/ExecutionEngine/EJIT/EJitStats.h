//===-- EJitStats.h - EmbeddedJIT taskpool statistics counters -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The EmbeddedJIT taskpool maintains monotonic diagnostic counters (cache
// hits, compiles, disabled instances, ...).  Each increment is an acq_rel
// atomic RMW on a shared cache line; on the steady-state cache-hit path the
// cacheHits counter is the single contended cross-core atomic, so updating it
// on every JIT call is the dominant per-call cost on a multi-core target.
//
// Compile with -DEJIT_STATS_ENABLE to embed the counter increments.  When not
// defined, every EJIT_STAT_INC* macro expands to nothing - zero atomic cost on
// the hot path - and ejit_taskpool_get_stats() simply reports zeros.  The
// counter FIELDS stay in their structs (the shared-memory layout / ABI is
// unchanged; only the increments are compiled out).  Default OFF for
// production; bring-up builds pass --stats / -DEJIT_STATS_ENABLE=ON.
//
// Note: this gates only the per-call taskpool counters (EJitSharedCounters /
// EJitTaskPoolCounters).  The code-pool stats (EJitSharedCodePoolStats /
// EJitCodePoolManager::Stats) are a separate, cold-path diagnostic and are
// always compiled.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSTATS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSTATS_H

#ifdef EJIT_STATS_ENABLE

/// Increment a single monotonic counter cell (an EJitAtomicU64).  acq_rel
/// matches the historical fetchAdd ordering of the counters.
#define EJIT_STAT_INC(cell) (cell).fetchAdd(1)

/// Increment instanceDisabled, and - only inside the init->activate window -
/// instanceDisabledPreActivate.  Bundled because the PreActivate increment is
/// gated on anyInstanceActivated, whose acquire load is therefore part of the
/// same stats-only concern: when stats are off both the RMWs AND the gate load
/// vanish entirely from the disabled path.  \p state is the EJitSharedTaskPool
/// shared-state pointer (this counter pair exists only in the shared pool).
#define EJIT_STAT_INC_INSTANCE_DISABLED(state)                               \
  do {                                                                       \
    (state)->counters.instanceDisabled.fetchAdd(1);                          \
    if ((state)->anyInstanceActivated.loadAcquire() == 0)                    \
      (state)->counters.instanceDisabledPreActivate.fetchAdd(1);             \
  } while (0)

#else // !EJIT_STATS_ENABLE

#define EJIT_STAT_INC(cell) ((void)0)
#define EJIT_STAT_INC_INSTANCE_DISABLED(state) ((void)0)

#endif // EJIT_STATS_ENABLE

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSTATS_H
