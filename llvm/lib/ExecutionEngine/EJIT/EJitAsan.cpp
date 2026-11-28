//===-- EJitAsan.cpp - Worker-only substitution checks ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitAsan.h"
#include "EJitAsanState.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/Support/ErrorHandling.h"
#include <utility>

using namespace llvm::ejit;

namespace {
template <typename T> uint64_t readWord(uintptr_t Address, bool LittleEndian) {
  const T V = __atomic_load_n(reinterpret_cast<const T *>(Address),
                              __ATOMIC_ACQUIRE);
  const auto *Bytes = reinterpret_cast<const unsigned char *>(&V);
  uint64_t Bits = 0;
  for (unsigned I = 0; I < sizeof(T); ++I)
    Bits |= uint64_t(Bytes[I]) << (8 * (LittleEndian ? I : sizeof(T) - 1 - I));
  return Bits;
}

uint64_t readWatch(uintptr_t Address, unsigned Width, bool LittleEndian) {
  switch (Width) {
  case 1:
    return readWord<uint8_t>(Address, LittleEndian) & 1;
  case 8:
    return readWord<uint8_t>(Address, LittleEndian);
  case 16:
    return readWord<uint16_t>(Address, LittleEndian);
  case 32:
    return readWord<uint32_t>(Address, LittleEndian);
  case 64:
    return readWord<uint64_t>(Address, LittleEndian);
  }
  llvm_unreachable("unsupported verifier read width");
}
} // namespace

//===----------------------------------------------------------------------===//
// Shared entry-instrumentation state (EJitAsanState.h).
//
// One blob for every core: emitted entry stubs call the gate and the
// mismatch hook from whichever core executes the specialized function, so
// the rotation cells, slot table, site table, and sticky records must all
// live in inter-core shared memory. Mirrors the gEJitVpState discipline.
//===----------------------------------------------------------------------===//
namespace llvm {
namespace ejit {
EJIT_SHARED_SECTION EJitAsanState gEJitAsanState;
} // namespace ejit
} // namespace llvm

namespace llvm {
namespace ejit {

uint32_t ejitAsanClaimSlot(StringRef FunctionName) {
  EJitAsanState &St = gEJitAsanState;
  const uint32_t Used = St.slotsUsed.loadRelaxed();
  for (uint32_t I = 0; I < Used; ++I)
    if (FunctionName == St.slots[I].name)
      return I;
  if (Used < EJIT_ASAN_MAX_SLOTS) {
    EJitAsanSlot &S = St.slots[Used];
    ejitAsanCopyName(S.name, sizeof(S.name), FunctionName);
    S.activeSites.storeRelaxed(0);
    St.slotsUsed.storeRelease(Used + 1);
    return Used;
  }
  // Table full: reuse a dead slot (one with no published sites). v1 never
  // frees slots on retirement, so only a renamed dead slot is ever safe.
  for (uint32_t I = 0; I < EJIT_ASAN_MAX_SLOTS; ++I)
    if (St.slots[I].activeSites.loadRelaxed() == 0) {
      EJitAsanSlot &S = St.slots[I];
      ejitAsanCopyName(S.name, sizeof(S.name), FunctionName);
      return I;
    }
  return kEJitAsanNoSlot;
}

void ejitAsanSetSlotBound(uint32_t Slot, uint32_t Bound) {
  if (Slot >= EJIT_ASAN_MAX_SLOTS || Bound == 0)
    return;
  EJitAsanSlot &S = gEJitAsanState.slots[Slot];
  uint32_t Cur = S.boundCount.loadRelaxed();
  while (Bound > Cur && !S.boundCount.compareExchange(Cur, Bound))
    if (Cur >= Bound)
      break; // Lost the race to a writer that already raised the maximum.
}

uint32_t ejitAsanPublishSite(uint32_t Slot, StringRef Function,
                             StringRef Period, uint8_t Kind, uint8_t Tier,
                             uint8_t ArgNo, uint8_t Width, uint8_t FieldAlign,
                             uint64_t ByteOffset, uint64_t Expected) {
  EJitAsanState &St = gEJitAsanState;
  const uint32_t Id = St.siteCounter.loadRelaxed();
  if (Id >= EJIT_ASAN_MAX_SITES)
    return kEJitAsanNoSite;
  St.siteCounter.storeRelaxed(Id + 1);
  EJitAsanSite &S = St.sites[Id];
  ejitAsanCopyName(S.function, sizeof(S.function), Function);
  ejitAsanCopyName(S.period, sizeof(S.period), Period);
  S.kind = Kind;
  S.gen = 0;
  S.slot = Slot;
  S.tier = Tier;
  S.argNo = ArgNo;
  S.width = Width;
  S.fieldAlign = FieldAlign;
  S.byteOffset = ByteOffset;
  S.expected = Expected;
  // Seed the drift baseline so the first observation cannot report a
  // pseudo-change from a zero-filled lastSeen.
  St.sticky[Id].lastSeen.storeRelaxed(Expected);
  S.published.storeRelease(1u);
  if (Slot != kEJitAsanNoSlot)
    St.slots[Slot].activeSites.fetchAddRelaxed(1);
  return Id;
}

} // namespace ejit
} // namespace llvm

//===----------------------------------------------------------------------===//
// Entry-stub hooks. Called from JIT'd specialized code through the
// absolute-symbol path (registered in EJit::init like the value-profiling
// hooks); report-only — a mismatch is recorded, the call always continues.
//===----------------------------------------------------------------------===//

/// Rotation gate, one call per instrumented function entry. At most one
/// function slot is active per core at a time, and each activated call
/// checks exactly one bound argument of that function. Returns the
/// sub-window index to check (0..bound-1) or -1 when this core is not
/// holding the window for the caller's slot.
extern "C" int32_t __ejit_asan_gate(uint32_t Slot) {
  const uint32_t Core = EJitCoreId::current();
  if (Core >= EJIT_ASAN_MAX_CORES)
    return -1; // Safely degraded: this core never checks.
  EJitAsanCell &C = gEJitAsanState.cells[Core];
  const uint32_t Live = gEJitAsanState.slotsUsed.loadRelaxed();
  if (Live == 0)
    return -1;
  if (C.countdown == 0) {
    if (C.checks == 0 && C.cycles == 0 && C.slot == 0) {
      // Fresh core: arm slot 0's first window instead of skipping it.
      C.slot = 0;
    } else {
      uint32_t Next = C.slot + 1;
      if (Next >= Live) {
        Next = 0;
        ++C.cycles;
      }
      C.slot = Next;
    }
    // Arm with the slot's current sub-window count, snapshotted for the
    // whole window; a later registration waits for the next arming.
    uint32_t Bound = gEJitAsanState.slots[C.slot].boundCount.loadRelaxed();
    C.bound = Bound ? Bound : 1;
    C.countdown = C.bound * EJIT_ASAN_ROTATION_WINDOW;
    if (C.countdown == 0) { // bound x window overflowed: degrade
      C.bound = 1;
      C.countdown = EJIT_ASAN_ROTATION_WINDOW;
    }
  }
  if (C.slot != Slot)
    return -1;
  // Sub-window of this call: from the countdown BEFORE this call consumes
  // its slot (calls 1..W are sub 0, W+1..2W are sub 1).
  const int32_t Sub =
      static_cast<int32_t>((C.bound * EJIT_ASAN_ROTATION_WINDOW - C.countdown) /
                           EJIT_ASAN_ROTATION_WINDOW);
  --C.countdown;
  if (C.checks != 0xFFFFFFFFu)
    ++C.checks;
  return Sub;
}

/// Stub cold-block hook (the name predates the drift extension). The
/// site's kind dispatches the protocol: a folded site records a mismatch
/// against the baked constant; a drift site records an observation against
/// the last-seen baseline — a change is annotation evidence, never a fault.
extern "C" void __ejit_asan_mismatch(uint32_t SiteId, const void *Arg,
                                     uint64_t ValueSeen) {
  if (SiteId >= EJIT_ASAN_MAX_SITES)
    return;
  EJitAsanSite &S = gEJitAsanState.sites[SiteId];
  if (S.published.loadAcquire() == 0)
    return;
  EJitAsanSticky &R = gEJitAsanState.sticky[SiteId];
  if (S.kind == kEJitAsanKindDrift) {
    if (R.samples.loadRelaxed() != 0xFFFFFFFFu)
      R.samples.fetchAddRelaxed(1);
    uint64_t Cur = R.lastSeen.loadRelaxed();
    if (Cur == ValueSeen)
      return;
    // Count one value transfer with a bounded CAS: under extreme
    // multi-core flutter the retry gives up and under-counts instead of
    // spinning (documented approximation).
    for (unsigned T = 0; T < 4; ++T) {
      if (R.lastSeen.compareExchange(Cur, ValueSeen)) {
        if (R.driftChanges.loadRelaxed() != 0xFFFFFFFFu)
          R.driftChanges.fetchAddRelaxed(1);
        break;
      }
      if (Cur == ValueSeen)
        break; // A concurrent observer already installed this value.
    }
    uint32_t Freeze = 0;
    if (R.recorded.compareExchange(Freeze, 1u)) {
      R.firstValue = ValueSeen;
      R.firstArg = reinterpret_cast<uint64_t>(Arg);
      R.recorded.storeRelease(2u);
    }
    return;
  }
  if (R.count.loadRelaxed() < 0xFFFFu)
    R.count.fetchAddRelaxed(1);
  // Freeze protocol: CAS 0->1 elects the single freezing writer; only the
  // winner stores the fields, then publishes with release (2 = frozen and
  // complete). Losers never store, so the frozen pair cannot be clobbered;
  // readers acquire the flag and skip a row caught mid-freeze (== 1).
  uint32_t Expected = 0;
  if (R.recorded.compareExchange(Expected, 1u)) {
    R.firstValue = ValueSeen;
    R.firstArg = reinterpret_cast<uint64_t>(Arg);
    R.recorded.storeRelease(2u);
  }
}

bool EJitAsan::sameRequest(const EJitCompileRequest &A,
                                     const EJitCompileRequest &B) {
  if (A.funcIndex != B.funcIndex || A.numDims != B.numDims ||
      A.generation != B.generation)
    return false;
  for (unsigned I = 0; I < A.numDims; ++I)
    if (A.dims[I].dimType != B.dims[I].dimType ||
        A.dims[I].instanceId != B.dims[I].instanceId ||
        A.versions[I] != B.versions[I])
      return false;
  return true;
}

void EJitAsan::mergeAnnotatedFields(const EJitAsanCapture &Capture) {
  // Boot-lifetime table, address-deduped across compiles.
  unsigned Dropped = 0;
  for (const auto &AF : Capture.annotatedFields) {
    if (!AF.base || !AF.stride || !AF.instanceCount)
      continue;
    for (uint32_t Inst = 0; Inst < AF.instanceCount; ++Inst) {
      const uintptr_t Address =
          AF.base + Inst * AF.stride + AF.offset;
      if (fullIndex_.count(Address))
        continue;
      if (fullSites_.size() >= MaxFullSites) {
        ++Dropped;
        continue;
      }
      EJitAsanFullSite FS;
      FS.object = AF.object;
      FS.period = AF.period;
      FS.base = AF.base;
      FS.address = Address;
      FS.instance = Inst;
      FS.instanceCount = AF.instanceCount;
      FS.offset = AF.offset;
      FS.width = AF.width;
      FS.littleEndian = AF.littleEndian;
      FS.initial = FS.lastSeen =
          readWatch(Address, FS.width, FS.littleEndian);
      fullIndex_.try_emplace(Address,
                             static_cast<uint32_t>(fullSites_.size()));
      fullSites_.push_back(std::move(FS));
    }
  }
  // A folded watch owns these addresses for its version lifetime; sticky
  // through retirement (documented blind spot).
  for (const auto &W : Capture.watches)
    for (uintptr_t A : W.addresses) {
      auto It = fullIndex_.find(A);
      if (It != fullIndex_.end())
        fullSites_[It->second].folded = true;
    }
  if (Dropped) {
    driftDropped_ += Dropped;
    EJIT_DIAG("[EJIT-ASAN] capture DROPPED: full-scope site budget full "
              "(cap=%u dropped=%u)",
              MaxFullSites, Dropped);
  }
}

void EJitAsan::onCompiled(const EJitCompileRequest &Req, uintptr_t Fn,
                                   EJitAsanCapture Capture) {
  // Backfill the version generation on the emitted entry sites now that the
  // request identity is known; the pipeline-end emitter publishes them
  // during the compile itself.
  for (const auto &EF : Capture.entryFolds)
    if (EF.siteId != kEJitAsanNoSite)
      gEJitAsanState.sites[EF.siteId].gen = Req.generation;
  // Full-scope tier: merged regardless of the version lifecycle.
  mergeAnnotatedFields(Capture);
  driftUncovered_ += Capture.driftUncovered;
  if (Capture.watches.empty() && Capture.entryFolds.empty() &&
      !Capture.uncovered && !Capture.dropped)
    return;
  if (pending_.size() + live_.size() >= MaxVersions) {
    dropped_ += Capture.watches.size() + Capture.uncovered + Capture.dropped;
    // Budget failures must be discoverable at compile time, not only in a
    // later dump: one line per dropped capture (rare anomaly, not scanning).
    EJIT_DIAG("[EJIT-ASAN] capture DROPPED: version budget full "
              "(tracked=%llu cap=%u) fn=%u watches=%u uncovered=%u",
              (unsigned long long)(pending_.size() + live_.size()),
              MaxVersions, stripReqTier(Req.funcIndex),
              static_cast<unsigned>(Capture.watches.size()), Capture.uncovered);
    return;
  }
  unsigned Trimmed = 0;
  while (Capture.watches.size() + retainedWatches_ > MaxWatches) {
    Capture.watches.pop_back();
    ++Capture.dropped;
    ++Trimmed;
  }
  if (Capture.dropped) {
    // Either the per-capture fold cap (EJitAsanCapture::MaxWatches)
    // or the global watch budget trimmed this capture.
    EJIT_DIAG("[EJIT-ASAN] capture TRIMMED: dropped=%u (global watch "
              "budget trim=%u, retained=%u+%llu cap=%u) fn=%u",
              Capture.dropped, Trimmed, retainedWatches_,
              (unsigned long long)Capture.watches.size(), MaxWatches,
              stripReqTier(Req.funcIndex));
  }
  retainedWatches_ += Capture.watches.size();
  EJitCompileRequest Identity{};
  Identity.funcIndex = Req.funcIndex;
  Identity.generation = Req.generation;
  Identity.numDims = Req.numDims;
  for (unsigned I = 0; I < Req.numDims; ++I) {
    Identity.dims[I] = Req.dims[I];
    Identity.versions[I] = Req.versions[I];
  }
  pending_.push_back({Identity, Fn, std::move(Capture)});
}

void EJitAsan::onPublished(const EJitCompileRequest &Req,
                                    bool Published) {
  for (auto I = pending_.begin(); I != pending_.end(); ++I) {
    if (!sameRequest(I->request, Req))
      continue;
    if (Published) {
      uncovered_ += I->capture.uncovered;
      dropped_ += I->capture.dropped;
      if (!I->capture.watches.empty()) {
        for (const auto &W : I->capture.watches)
          activeProbes_ += W.addresses.size();
        live_.push_back(std::move(*I));
        pending_.erase(I);
        return;
      }
    }
    retainedWatches_ -= I->capture.watches.size();
    pending_.erase(I);
    return;
  }
}

void EJitAsan::dropCurrent() {
  auto &Current = live_[cursor_];
  for (const auto &W : Current.capture.watches) {
    activeProbes_ -= W.addresses.size();
    for (unsigned R = 0; R < W.addresses.size(); ++R)
      checkedActiveProbes_ -= (W.seen >> R) & 1;
  }
  retainedWatches_ -= Current.capture.watches.size();
  if (cursor_ + 1 != live_.size())
    Current = std::move(live_.back());
  live_.pop_back();
  if (cursor_ == live_.size())
    cursor_ = 0;
  ++retired_;
}

void EJitAsan::advance() {
  if (++cursor_ == live_.size())
    cursor_ = 0;
}

void EJitAsan::printRecordedRows(VerifyYieldFn Yield,
                                           void *YieldCtx) const {
  // Dump-only console output. Every location that has EVER mismatched is
  // reported (sticky: recovery does not unregister it), with its saturating
  // count/samples pair as a rate and the value seen at the FIRST mismatch
  // (`first`); nothing is re-read at print time. `object=<symbol>@<base>
  // offset=<bytes>` pins the member inside its shared object (for a changed
  // pointer root the compared member is the slot itself: base=slot,
  // offset=0). `src` names the driving free-dim argument of the remainder
  // domain (or `const` when the index is fully constant); the domain size is
  // the residue denominator. After every row the worker pauses
  // YieldDelayTicks so the report trickles out and never stresses the
  // console or the heartbeat.
  // Index-based iteration, re-fetched every row: the direct dump prints in
  // the caller's context, and each row's platform yield lets the worker
  // task run compiles that append to live_ (a reallocation). live_ is
  // append-only while the dump holds printActive_, so indices stay valid;
  // references held across a yield do not.
  for (size_t Vi = 0; Vi < live_.size(); ++Vi)
    for (size_t Wi = 0; Wi < live_[Vi].capture.watches.size(); ++Wi)
      for (size_t Fi = 0; Fi < live_[Vi].capture.watches[Wi].first.size();
           ++Fi) {
        const auto &V = live_[Vi];
        const auto &W = V.capture.watches[Wi];
        const auto &Rec = W.first[Fi];
        const unsigned R = Rec.residue;
        const unsigned Rate =
            W.samples[R] ? (W.count[R] * 1000u) / W.samples[R] : 0;
        const auto &Q = V.request;
        const unsigned long long Base =
            Rec.changedRoot ? W.rootSlot : W.objectBase;
        const unsigned long long Offset =
            Rec.changedRoot ? 0 : W.addresses[R] - W.objectBase;
        const char *Kind = W.bound
                               ? "bound"
                               : (Rec.changedRoot || W.rootValue)
                                     ? "pointer-root"
                                     : "value";
        if (W.freeArg != 0xFF)
          EJIT_DIAG_RAW(
              "[EJIT-ASAN] MISMATCH tier=%u gen=%u "
              "dims=%u:%u@%u,%u:%u@%u,%u:%u@%u,%u:%u@%u "
              "period=%s fn=%s object=%s@0x%llx offset=0x%llx src=arg%u "
              "residue=%u/%llu kind=%s expected=0x%llx first=0x%llx "
              "count=%u/%u rate=%u.%u%%",
              decodeReqTier(Q.funcIndex), Q.generation,
              Q.dims[0].dimType, Q.dims[0].instanceId, Q.versions[0],
              Q.dims[1].dimType, Q.dims[1].instanceId, Q.versions[1],
              Q.dims[2].dimType, Q.dims[2].instanceId, Q.versions[2],
              Q.dims[3].dimType, Q.dims[3].instanceId, Q.versions[3],
              W.period.c_str(), W.function.c_str(), W.object.c_str(), Base,
              Offset, W.freeArg, R, (unsigned long long)W.addresses.size(),
              Kind,
              (unsigned long long)(Rec.changedRoot ? W.expectedRoot
                                                   : W.expected),
              (unsigned long long)Rec.value,
              W.count[R], W.samples[R], Rate / 10u, Rate % 10u);
        else
          EJIT_DIAG_RAW(
              "[EJIT-ASAN] MISMATCH tier=%u gen=%u "
              "dims=%u:%u@%u,%u:%u@%u,%u:%u@%u,%u:%u@%u "
              "period=%s fn=%s object=%s@0x%llx offset=0x%llx src=const "
              "residue=%u/%llu kind=%s expected=0x%llx first=0x%llx "
              "count=%u/%u rate=%u.%u%%",
              decodeReqTier(Q.funcIndex), Q.generation,
              Q.dims[0].dimType, Q.dims[0].instanceId, Q.versions[0],
              Q.dims[1].dimType, Q.dims[1].instanceId, Q.versions[1],
              Q.dims[2].dimType, Q.dims[2].instanceId, Q.versions[2],
              Q.dims[3].dimType, Q.dims[3].instanceId, Q.versions[3],
              W.period.c_str(), W.function.c_str(), W.object.c_str(), Base,
              Offset, R, (unsigned long long)W.addresses.size(), Kind,
              (unsigned long long)(Rec.changedRoot ? W.expectedRoot
                                                   : W.expected),
              (unsigned long long)Rec.value,
              W.count[R], W.samples[R], Rate / 10u, Rate % 10u);
        if (Yield)
          Yield(YieldCtx, YieldDelayTicks);
      }
  // Entry-instrumentation rows, read from the shared blob. object=bound@<arg>
  // carries the actual bound-pointer argument of the first diverging call —
  // the family member that broke the fold's consistency, which no
  // worker-side probe can name. Entry rows have no residue (the stub reads
  // the current pointer's field directly, nothing to enumerate) and no rate
  // (counting every executed check would put an atomic in the hot path);
  // `src=boundN` names the bound-pointer argument the field was reached
  // through. Drift rows are evidence, not faults: a changed value means the
  // annotation does not hold. A changed site prints its core row at INFO
  // plus a forensic detail line at VERBOSE; a never-changed site prints at
  // VERBOSE only (the stats line carries the aggregate).
  const uint32_t Sites = gEJitAsanState.siteCounter.loadRelaxed();
  for (uint32_t Si = 0; Si < Sites && Si < EJIT_ASAN_MAX_SITES; ++Si) {
    const EJitAsanSite &S = gEJitAsanState.sites[Si];
    if (S.published.loadAcquire() == 0)
      continue;
    const EJitAsanSticky &R = gEJitAsanState.sticky[Si];
    if (S.kind == kEJitAsanKindDrift) {
      const unsigned Recorded = R.recorded.loadAcquire();
      if (Recorded == 1)
        continue; // Caught mid-freeze.
      const unsigned Samples = R.samples.loadRelaxed();
      const unsigned Changes = R.driftChanges.loadRelaxed();
      if (Recorded == 0) {
        // Never changed: VERBOSE only; no diverging member to name.
        EJIT_DIAG_VERBOSE(
            "[EJIT-ASAN] DRIFT period=%s fn=%s "
            "offset=0x%llx src=bound%u kind=drift initial=0x%llx last=0x%llx "
            "changes=%u samples=%u",
            S.period, S.function, (unsigned long long)S.byteOffset,
            S.argNo, (unsigned long long)S.expected,
            (unsigned long long)R.lastSeen.loadRelaxed(), Changes, Samples);
        if (Yield)
          Yield(YieldCtx, YieldDelayTicks);
        continue;
      }
      EJIT_DIAG_RAW(
          "[EJIT-ASAN] DRIFT period=%s fn=%s object=bound@0x%llx "
          "offset=0x%llx src=bound%u kind=drift initial=0x%llx last=0x%llx "
          "changes=%u samples=%u",
          S.period, S.function, (unsigned long long)R.firstArg,
          (unsigned long long)S.byteOffset, S.argNo,
          (unsigned long long)S.expected,
          (unsigned long long)R.lastSeen.loadRelaxed(), Changes, Samples);
      EJIT_DIAG_VERBOSE(
          "[EJIT-ASAN] DRIFT-DETAIL fn=%s offset=0x%llx first=0x%llx "
          "tier=%u gen=%u",
          S.function, (unsigned long long)S.byteOffset,
          (unsigned long long)R.firstValue, S.tier, S.gen);
      if (Yield)
        Yield(YieldCtx, YieldDelayTicks);
      continue;
    }
    if (R.recorded.loadAcquire() < 2)
      continue; // Never mismatched, or caught mid-freeze (== 1).
    unsigned Count = R.count.loadRelaxed();
    if (Count > 0xFFFF)
      Count = 0xFFFF;
    EJIT_DIAG_RAW(
        "[EJIT-ASAN] MISMATCH tier=%u gen=%u period=%s fn=%s "
        "object=bound@0x%llx offset=0x%llx src=bound%u kind=entry "
        "expected=0x%llx first=0x%llx count=%u",
        S.tier, S.gen, S.period, S.function,
        (unsigned long long)R.firstArg, (unsigned long long)S.byteOffset,
        S.argNo, (unsigned long long)S.expected,
        (unsigned long long)R.firstValue, Count);
    if (Yield)
      Yield(YieldCtx, YieldDelayTicks);
  }

  // Full-scope drift rows: registered annotated fields the worker samples
  // directly (src=metadata), one row per field instance. Same two-tier
  // discipline as the stub rows; folded sites are skipped — the version
  // watch rows report their addresses.
  for (size_t Fi = 0; Fi < fullSites_.size(); ++Fi) {
    const EJitAsanFullSite &FS = fullSites_[Fi];
    if (FS.folded)
      continue;
    if (FS.changes) {
      EJIT_DIAG_RAW(
          "[EJIT-ASAN] DRIFT period=%s object=%s@0x%llx offset=0x%llx "
          "instance=%u/%u src=metadata kind=drift initial=0x%llx "
          "last=0x%llx changes=%u samples=%u",
          FS.period.c_str(), FS.object.c_str(),
          (unsigned long long)FS.base, (unsigned long long)FS.offset,
          FS.instance, FS.instanceCount, (unsigned long long)FS.initial,
          (unsigned long long)FS.lastSeen, FS.changes, FS.samples);
      EJIT_DIAG_VERBOSE(
          "[EJIT-ASAN] DRIFT-DETAIL object=%s@0x%llx offset=0x%llx "
          "instance=%u/%u first=0x%llx",
          FS.object.c_str(), (unsigned long long)FS.base,
          (unsigned long long)FS.offset, FS.instance, FS.instanceCount,
          (unsigned long long)FS.firstChanged);
    } else {
      // Never changed: full visibility at VERBOSE only.
      EJIT_DIAG_VERBOSE(
          "[EJIT-ASAN] DRIFT period=%s object=%s@0x%llx offset=0x%llx "
          "instance=%u/%u src=metadata kind=drift initial=0x%llx "
          "last=0x%llx changes=%u samples=%u",
          FS.period.c_str(), FS.object.c_str(),
          (unsigned long long)FS.base, (unsigned long long)FS.offset,
          FS.instance, FS.instanceCount, (unsigned long long)FS.initial,
          (unsigned long long)FS.lastSeen, FS.changes, FS.samples);
    }
    if (Yield)
      Yield(YieldCtx, YieldDelayTicks);
  }
}

void EJitAsan::dump(VerifyYieldFn Yield, void *YieldCtx) const {
  unsigned Recorded = 0;
  for (const auto &V : live_)
    for (const auto &W : V.capture.watches)
      Recorded += W.first.size();
  const bool Full = retainedWatches_ >= MaxWatches;
  // Blocked-root samples count into mismatch_samples_ but not into checks_,
  // so the global ratio is clamped to 100%.
  unsigned GlobalRate =
      checks_ ? (unsigned)((mismatchSamples_ * 1000) / checks_) : 0;
  if (GlobalRate > 1000)
    GlobalRate = 1000;
  // Entry-instrumentation volume: per-core served checks (summed across
  // the shared cells), sites whose sticky record froze (folded mismatches
  // vs drift changes), and the drift observation volume.
  uint64_t EntryChecks = 0, EntryFoldedSites = 0, EntryDriftSites = 0,
           EntryDriftChecks = 0;
  for (const auto &Cell : gEJitAsanState.cells)
    EntryChecks += Cell.checks;
  const uint32_t Sites = gEJitAsanState.siteCounter.loadRelaxed();
  for (uint32_t Si = 0; Si < Sites && Si < EJIT_ASAN_MAX_SITES; ++Si) {
    if (!gEJitAsanState.sites[Si].published.loadAcquire())
      continue;
    const EJitAsanSticky &R = gEJitAsanState.sticky[Si];
    if (gEJitAsanState.sites[Si].kind == kEJitAsanKindDrift) {
      EntryDriftChecks += R.samples.loadRelaxed();
      EntryDriftSites += R.recorded.loadAcquire() >= 2;
    } else {
      EntryFoldedSites += R.recorded.loadAcquire() >= 2;
    }
  }
  // Full-scope tier aggregates: boot-lifetime sites, folded ones (skipped
  // by the scan), sites with recorded drift, and total observations.
  uint64_t FullscopeFolded = 0, FullscopeChanged = 0, FullscopeSamples = 0;
  for (const auto &FS : fullSites_) {
    if (FS.folded) {
      ++FullscopeFolded;
      continue;
    }
    FullscopeSamples += FS.samples;
    FullscopeChanged += FS.changes != 0;
  }
  const bool FullSites = fullSites_.size() >= MaxFullSites;
  EJIT_DIAG_RAW(
      "[EJIT-ASAN] versions=%llu staged=%llu watches=%u/%u%s "
      "checked_active=%llu/%llu checks=%llu mismatch_samples=%llu "
      "rate=%u.%u%% recorded=%u uncovered=%llu dropped=%llu "
      "blocked_root=%llu retired=%llu entry_checks=%llu "
      "entry_folded_sites=%llu entry_drift_sites=%llu entry_drift_checks=%llu "
      "fullscope_sites=%llu/%u%s fullscope_folded=%llu fullscope_changed=%llu "
      "fullscope_samples=%llu drift_uncovered=%llu drift_dropped=%llu",
      (unsigned long long)live_.size(), (unsigned long long)pending_.size(),
      retainedWatches_, MaxWatches,
      Full ? " FULL" : "",
      (unsigned long long)checkedActiveProbes_,
      (unsigned long long)activeProbes_, (unsigned long long)checks_,
      (unsigned long long)mismatchSamples_, GlobalRate / 10u, GlobalRate % 10u,
      Recorded, (unsigned long long)uncovered_, (unsigned long long)dropped_,
      (unsigned long long)blockedRoot_, (unsigned long long)retired_,
      (unsigned long long)EntryChecks, (unsigned long long)EntryFoldedSites,
      (unsigned long long)EntryDriftSites,
      (unsigned long long)EntryDriftChecks,
      (unsigned long long)fullSites_.size(), MaxFullSites,
      FullSites ? " FULL" : "", (unsigned long long)FullscopeFolded,
      (unsigned long long)FullscopeChanged, (unsigned long long)FullscopeSamples,
      (unsigned long long)driftUncovered_, (unsigned long long)driftDropped_);
  if (Yield)
    Yield(YieldCtx, YieldDelayTicks);
}

void EJitAsan::scanSlice(const EJitSharedTaskPool &Pool,
                                   VerifyYieldFn Yield, void *YieldCtx) {
  bool Worked = false;
  for (unsigned N = 0; N < StepBudget && !live_.empty(); ++N) {
    Worked = true;
    // Interleave a platform tick inside one slice so a print-heavy dump or a
    // large configured step budget cannot starve the worker's heartbeat
    // (bare-metal watchdog) between the loop's own per-round yields.
    if (Yield && EJIT_ASAN_YIELD_EVERY &&
        N % EJIT_ASAN_YIELD_EVERY == 0 && N != 0)
      Yield(YieldCtx, YieldDelayTicks);
    // References are taken fresh every step: during a direct dump these
    // in-slice yields let the worker task consume compiles, and a completed
    // compile appends to live_ (reallocating it), so nothing may be held
    // across a yield.
    auto &V = live_[cursor_];
    auto IsLive = Pool.asanLive(V.request, V.function);
    if (!IsLive || !*IsLive) {
      if (IsLive)
        dropCurrent();
      else
        advance();
      continue;
    }
    auto &W = V.capture.watches[V.watch];
    const unsigned R = V.residue;
    auto *PointerSlot = reinterpret_cast<const uintptr_t *>(W.rootSlot);
    uintptr_t Root =
        PointerSlot ? __atomic_load_n(PointerSlot, __ATOMIC_ACQUIRE) : 0;
    // Do not dereference a newly installed root, even to confirm a mismatch.
    bool ChangedRoot = PointerSlot && Root != W.expectedRoot;
    const uint64_t ActualField =
        ChangedRoot ? 0 : readWatch(W.addresses[R], W.width, W.littleEndian);
    if (PointerSlot && !ChangedRoot) {
      Root = __atomic_load_n(PointerSlot, __ATOMIC_ACQUIRE);
      ChangedRoot = Root != W.expectedRoot;
    }
    IsLive = Pool.asanLive(V.request, V.function);
    if (!IsLive || !*IsLive) {
      if (IsLive)
        dropCurrent();
      else
        advance();
      continue;
    }

    const unsigned Bit = 1u << R;
    const uint64_t Expected = ChangedRoot ? W.expectedRoot : W.expected;
    const uint64_t Actual = ChangedRoot ? Root : ActualField;
    if (ChangedRoot)
      ++blockedRoot_;
    else if (!(W.seen & Bit)) {
      ++checkedActiveProbes_;
      W.seen |= Bit;
    }
    if (!ChangedRoot)
      ++checks_;
    // Sticky per-residue record: every probe attempt counts as a sample,
    // every mismatch into the location's count; both saturate at 0xFFFF and
    // freeze so the printed ratio stays meaningful. The FIRST mismatching
    // value (and whether it was a changed pointer root) is frozen into the
    // sparse record; nothing prints here — rows surface only on a dump.
    if (W.samples[R] != 0xFFFF)
      ++W.samples[R];
    if (Actual != Expected) {
      ++mismatchSamples_;
      if (W.count[R] == 0)
        W.first.push_back({uint8_t(R), ChangedRoot, Actual});
      if (W.count[R] != 0xFFFF)
        ++W.count[R];
    }
    if (++V.residue == W.addresses.size()) {
      V.residue = 0;
      if (++V.watch == V.capture.watches.size())
        V.watch = 0;
    }
    advance();
  }

  // Full-scope drift watch, second cursor with its own step budget:
  // boot-lifetime, no version lifecycle (resetting on retirement would
  // destroy the long-run evidence).
  for (unsigned N = 0; N < StepBudget && !fullSites_.empty(); ++N) {
    Worked = true;
    if (Yield && EJIT_ASAN_YIELD_EVERY &&
        N % EJIT_ASAN_YIELD_EVERY == 0 && N != 0)
      Yield(YieldCtx, YieldDelayTicks);
    // Fresh reference each step: a yield lets the worker consume compiles
    // whose merges append to (and may reallocate) fullSites_.
    EJitAsanFullSite &FS = fullSites_[fullCursor_];
    if (++fullCursor_ == fullSites_.size())
      fullCursor_ = 0;
    if (FS.folded)
      continue; // The version watch owns this address.
    const uint64_t V = readWatch(FS.address, FS.width, FS.littleEndian);
    if (FS.samples != 0xFFFFFFFFu)
      ++FS.samples;
    if (V != FS.lastSeen) {
      if (FS.changes == 0)
        FS.firstChanged = V;
      if (FS.changes != 0xFFFFFFFFu)
        ++FS.changes;
      FS.lastSeen = V;
    }
  }
  // Work/rest rhythm: a finished slice always ends with one fixed rest. A
  // light slice's probe work is microseconds per step (far under the work
  // window), so in practice the rest dominates; report rows pause
  // separately, once per row.
  if (Worked && Yield)
    Yield(YieldCtx, YieldDelayTicks);
}

void EJitAsan::idle(const EJitSharedTaskPool &Pool,
                              VerifyYieldFn Yield, void *YieldCtx) {
  // Periodic worker path: never prints, and stands down entirely while a
  // direct dump is printing on this core.
  if (printActive_)
    return;
  if (++idleTicks_ < IdleTicks)
    return;
  idleTicks_ = 0;
  scanSlice(Pool, Yield, YieldCtx);
}

bool EJitAsan::dumpNow(const EJitSharedTaskPool &Pool,
                                 VerifyYieldFn Yield, void *YieldCtx) {
  // Direct report: run in the CALLER's context (the worker core) and return
  // only when the whole report is out — one scan slice, then one row per
  // recorded location, then the stats line, each paced by fixed yields.
  // printActive_ is a plain bool on purpose: every context touching the
  // verifier runs on the same core, where cooperative tasks only interleave
  // at platform yields, so a task switch always observes the flag.
  if (printActive_)
    return false;
  printActive_ = true;
  idleTicks_ = 0;
  scanSlice(Pool, Yield, YieldCtx);
  printRecordedRows(Yield, YieldCtx);
  dump(Yield, YieldCtx);
  printActive_ = false;
  return true;
}
