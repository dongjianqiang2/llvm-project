//===-- EJitWorkerVerifier.cpp - Worker-only substitution checks ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitWorkerVerifier.h"
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

bool EJitWorkerVerifier::sameRequest(const EJitCompileRequest &A,
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

void EJitWorkerVerifier::onCompiled(const EJitCompileRequest &Req, uintptr_t Fn,
                                   EJitWorkerVerifyCapture Capture) {
  if (Capture.watches.empty() && !Capture.uncovered && !Capture.dropped)
    return;
  if (pending_.size() + live_.size() >= MaxVersions) {
    dropped_ += Capture.watches.size() + Capture.uncovered + Capture.dropped;
    return;
  }
  while (Capture.watches.size() + retainedWatches_ > MaxWatches) {
    Capture.watches.pop_back();
    ++Capture.dropped;
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

void EJitWorkerVerifier::onPublished(const EJitCompileRequest &Req,
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

void EJitWorkerVerifier::dropCurrent() {
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

void EJitWorkerVerifier::advance() {
  if (++cursor_ == live_.size())
    cursor_ = 0;
}

void EJitWorkerVerifier::dump() const {
  EJIT_DIAG_RAW(
      "[WORKER-VERIFY] versions=%zu staged=%zu checked_active=%llu/%llu "
      "checks=%llu mismatch_samples=%llu uncovered=%llu dropped=%llu "
      "blocked_root=%llu retired=%llu suppressed=%llu",
      live_.size(), pending_.size(),
      (unsigned long long)checkedActiveProbes_, (unsigned long long)activeProbes_,
      (unsigned long long)checks_, (unsigned long long)mismatchSamples_,
      (unsigned long long)uncovered_, (unsigned long long)dropped_,
      (unsigned long long)blockedRoot_,
      (unsigned long long)retired_, (unsigned long long)suppressed_);
}

void EJitWorkerVerifier::idle(const EJitSharedTaskPool &Pool, bool DumpRequested) {
  if (++idleTicks_ < IdleTicks && !DumpRequested)
    return;
  idleTicks_ = 0;
  // Report at most four new discrepancies per slice. The other transitions
  // are counted and can be surfaced on request without flooding the console.
  unsigned Printed = 0;
  for (unsigned N = 0; N < StepBudget && !live_.empty(); ++N) {
    auto &V = live_[cursor_];
    auto IsLive = Pool.workerVerifyLive(V.request, V.function);
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
    IsLive = Pool.workerVerifyLive(V.request, V.function);
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
    if (Actual != Expected) {
      ++mismatchSamples_;
      if (!(W.mismatched & Bit)) {
        if (gEJitDiagLevel >= EJIT_LOG_LVL_INFO &&
            (Printed < 4 || DumpRequested)) {
          ++Printed;
          const auto &Q = V.request;
          EJIT_DIAG_RAW(
              "[WORKER-VERIFY] MISMATCH entry=%u tier=%u gen=%u "
              "dims=%u:%u@%u,%u:%u@%u,%u:%u@%u,%u:%u@%u "
              "period=%.*s fn=%s load=%u object=%s residue=%u/%zu "
              "kind=%s addr=0x%llx expected=0x%llx actual=0x%llx",
              stripReqTier(Q.funcIndex), decodeReqTier(Q.funcIndex), Q.generation,
              Q.dims[0].dimType, Q.dims[0].instanceId, Q.versions[0],
              Q.dims[1].dimType, Q.dims[1].instanceId, Q.versions[1],
              Q.dims[2].dimType, Q.dims[2].instanceId, Q.versions[2],
              Q.dims[3].dimType, Q.dims[3].instanceId, Q.versions[3],
              (int)V.capture.period.size(), V.capture.period.data(),
              W.function.c_str(), W.loadOrdinal, W.object.c_str(), R,
              W.addresses.size(),
              (ChangedRoot || W.rootValue) ? "pointer-root" : "value",
              (unsigned long long)(ChangedRoot ? W.rootSlot : W.addresses[R]),
              (unsigned long long)Expected, (unsigned long long)Actual);
          W.mismatched |= Bit;
        } else {
          ++suppressed_;
        }
      }
    } else {
      W.mismatched &= ~Bit;
    }
    if (++V.residue == W.addresses.size()) {
      V.residue = 0;
      if (++V.watch == V.capture.watches.size())
        V.watch = 0;
    }
    advance();
  }
  if (DumpRequested)
    dump();
}
