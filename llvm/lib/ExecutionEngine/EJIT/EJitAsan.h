//===-- EJitAsan.h - Worker-only substitution checks ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITASAN_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITASAN_H

#include "EJitAsanState.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitTaskPoolTypes.h"
#include <string>
#include <vector>

namespace llvm::ejit {

// Scan pacing and budgets, overridable at build time (target-scoped
// definitions on LLVMEJIT from the EJIT_ASAN_* CMake cache
// variables). The #ifndef defaults keep standalone consumers — including the
// host unit tests, which drive idle() directly and rely on deterministic
// budgets — independent of any board configuration.
// Product clock calibration: 1 platform tick ~ 1 ms, so the default
// IDLE_TICKS is one probe slice every ~5 seconds.
#ifndef EJIT_ASAN_IDLE_TICKS
#define EJIT_ASAN_IDLE_TICKS 5000
#endif
#ifndef EJIT_ASAN_STEP_BUDGET
#define EJIT_ASAN_STEP_BUDGET 32
#endif
// 0 disables in-slice yielding (tight-host builds only; leave >= 1 on the
// board so a print-heavy dump slice cannot starve the watchdog heartbeat).
#ifndef EJIT_ASAN_YIELD_EVERY
#define EJIT_ASAN_YIELD_EVERY 8
#endif
// Global budgets across all tracked versions: total watches (folded-load
// sites, each with its residue addresses) and total simultaneous versions.
// Overrun trims the NEWEST capture (existing watches are never evicted) and
// counts into `dropped`; the stats line reports watches=<n>/<cap> with a FULL
// marker once the budget is exhausted.
#ifndef EJIT_ASAN_MAX_WATCHES
#define EJIT_ASAN_MAX_WATCHES 81920
#endif
#ifndef EJIT_ASAN_MAX_VERSIONS
#define EJIT_ASAN_MAX_VERSIONS 1024
#endif

class EJitSharedTaskPool;

struct EJitAsanWatch {
  // Widest remainder domain a watch may enumerate (compile-side cap in
  // EJitStructFieldPass.cpp; arrays below are sized to it).
  static constexpr unsigned MaxResidues = 16;

  // First-mismatch record, sparse per watch: the value seen (and whether it
  // was a changed pointer root) at the moment a residue FIRST mismatched.
  // Dump rows report this frozen value; nothing is re-read at print time.
  struct FirstMismatch {
    uint8_t residue;
    bool changedRoot;
    uint64_t value;
  };

  std::string function;
  std::string period; // fold's source period (dump rows report its name)
  std::string object; // fold's source global (C symbol name)
  // The address `offset` in the report row is relative to. For value folds
  // it is the registered object/array base; for a pointer slot compared as the
  // value (or as a changed root) it is the slot itself, with offset 0.
  uintptr_t objectBase = 0;
  // Driving free-dim parameter (function argument index) for folds with an
  // enumerable remainder domain; 0xFF when the index is fully constant.
  uint8_t freeArg = 0xFF;
  uint8_t width = 0;
  bool littleEndian = false;
  bool rootValue = false;
  bool bound = false; // fold of a borrowed bound-pointer pointee (kind=bound)
  uint64_t expected = 0;
  uintptr_t rootSlot = 0;
  uintptr_t expectedRoot = 0;
  SmallVector<uintptr_t, 10> addresses;
  // Per-residue sticky record: once a residue has ever mismatched it stays on
  // the dump report (with its rate) until the version retires; recovery does
  // not unregister it. Both counters saturate at 0xFFFF and then freeze, so
  // the printed ratio stays meaningful.
  uint16_t count[MaxResidues] = {};
  uint16_t samples[MaxResidues] = {};
  uint16_t seen = 0;
  SmallVector<FirstMismatch, 2> first;
};

// One bound fold eligible for the entry stub: a constant-offset fold through
// a bound_ptr argument (modulus == 1). The specialized entry can re-load the
// field through the CURRENT bound argument — the only place the current
// family member is observable. siteId stays kEJitAsanNoSite until the
// pipeline-end emitter publishes it (EJitOptimizer emitAsanEntryChecks).
struct EJitAsanEntryFold {
  std::string function; // Owning function: the fold's load site lives here.
  std::string period;
  uint32_t siteId = 0; // kEJitAsanNoSite until published
  uint32_t argNo = 0;  // Bound-pointer argument index (of `function`).
  uint64_t byteOffset = 0;
  uint64_t expected = 0;
  uint8_t width = 0;
  uint8_t align = 0; // Field alignment in bytes.
};

// Compile-side only. No LLVM IR or borrowed calling arguments survive capture.
struct EJitAsanCapture {
  static constexpr unsigned MaxWatches = 128;
  std::vector<EJitAsanWatch> watches;
  std::vector<EJitAsanEntryFold> entryFolds;
  unsigned uncovered = 0;
  unsigned dropped = 0;
};

class EJitAsan {
public:
  void onCompiled(const EJitCompileRequest &Req, uintptr_t Fn,
                  EJitAsanCapture Capture);
  void onPublished(const EJitCompileRequest &Req, bool Published);

  /// One throttled probe slice on the worker's periodic idle path. Normal
  /// slices never print: mismatches only bump the per-residue sticky
  /// records and the global counters. Skipped while a direct dump is
  /// printing on this core.
  /// \p Yield, when non-null, feeds the platform tick: once per
  /// EJIT_ASAN_YIELD_EVERY completed steps while scanning. The
  /// driver maps it to EJitSreTask::delay, the same primitive the worker
  /// loop's own per-round yield uses.
  using VerifyYieldFn = void (*)(void *Ctx, uint32_t Ticks);
  void idle(const EJitSharedTaskPool &Pool, VerifyYieldFn Yield = nullptr,
            void *YieldCtx = nullptr);

  /// Print the report NOW, in the caller's context (the worker core): one
  /// probe slice, then one row per recorded location (nothing re-read at
  /// print time), then the stats line — each paced with fixed yields — and
  /// only then return. Returns false when another dump is already printing
  /// on this core. While printing, the worker's periodic scan stands down;
  /// the row yields still let the worker consume compiles, and live_ is
  /// append-only during the print, so row data is re-fetched by index
  /// across every yield.
  bool dumpNow(const EJitSharedTaskPool &Pool, VerifyYieldFn Yield = nullptr,
               void *YieldCtx = nullptr);

private:
  struct Version {
    EJitCompileRequest request;
    uintptr_t function;
    EJitAsanCapture capture;
    unsigned watch = 0;
    unsigned residue = 0;
  };
  static bool sameRequest(const EJitCompileRequest &A,
                          const EJitCompileRequest &B);
  void dropCurrent();
  void advance();
  void scanSlice(const EJitSharedTaskPool &Pool, VerifyYieldFn Yield,
                 void *YieldCtx);
  void printRecordedRows(VerifyYieldFn Yield, void *YieldCtx) const;
  void dump(VerifyYieldFn Yield, void *YieldCtx) const;

  static constexpr unsigned MaxVersions = EJIT_ASAN_MAX_VERSIONS;
  static constexpr unsigned MaxWatches = EJIT_ASAN_MAX_WATCHES;
  static constexpr unsigned IdleTicks = EJIT_ASAN_IDLE_TICKS;
  static constexpr unsigned StepBudget = EJIT_ASAN_STEP_BUDGET;
  // Verifier yield pacing, deliberately fixed (not a CMake knob): every
  // platform yield the verifier makes — one per EJIT_ASAN_YIELD_EVERY
  // completed scan steps, after every printed problem row, and once after the
  // stats line — pauses this many platform ticks, so neither a large step
  // budget nor a long report can stress the console or the system.
  static constexpr unsigned YieldDelayTicks = 20;
  std::vector<Version> pending_, live_;
  size_t cursor_ = 0;
  unsigned retainedWatches_ = 0;
  unsigned idleTicks_ = 0;
  // Direct-dump in-progress flag (see dumpNow): plain bool, not atomic —
  // every context touching the verifier runs on one core, where cooperative
  // tasks only interleave at platform yields, so a task switch always
  // observes the flag.
  bool printActive_ = false;
  uint64_t checks_ = 0;
  uint64_t activeProbes_ = 0;
  uint64_t checkedActiveProbes_ = 0;
  uint64_t mismatchSamples_ = 0;
  uint64_t uncovered_ = 0;
  uint64_t dropped_ = 0;
  uint64_t blockedRoot_ = 0;
  uint64_t retired_ = 0;
};

} // namespace llvm::ejit
#endif
