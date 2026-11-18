//===-- EJitCodePoolMemoryManager.cpp - JITLink mem mgr over code pool ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkDylib.h"
#include "llvm/ExecutionEngine/Orc/Shared/AllocationActions.h"
#include "llvm/ExecutionEngine/Orc/Shared/MemoryFlags.h"
#include "llvm/Support/MathExtras.h"
#include <cstring>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;
using namespace llvm::jitlink;

using orc::ExecutorAddr;
using WrapperFunctionCall = orc::shared::WrapperFunctionCall;

namespace {
/// One contiguous executable segment of a finalized allocation (the only kind
/// of memory that needs execute permission). An allocation may contain several
/// (non-contiguous) executable segments and any number of non-executable
/// (read-only / writable / GOT) segments, which are NEVER sealed RX.
struct ExecSegRange {
  uintptr_t Addr = 0;
  uint64_t Size = 0;
};
} // namespace

/// Side record used as the FinalizedAlloc handle. Holds the dealloc actions and
/// the pool-backed base address. Pool memory itself is not freed in v1.
struct EJitCodePoolMemoryManager::FinalizedInfo {
  void *Base = nullptr;
  std::vector<WrapperFunctionCall> DeallocActions;
};

class EJitCodePoolMemoryManager::InFlightAllocImpl
    : public JITLinkMemoryManager::InFlightAlloc {
public:
  InFlightAllocImpl(EJitCodePoolManager &Pool, LinkGraph &G, BasicLayout BL,
                    void *Base, size_t Size,
                    std::vector<ExecSegRange> ExecRanges,
                    std::vector<EJitWritableRange> WritableRanges)
      : Pool(&Pool), G(&G), BL(std::move(BL)), Base(Base), Size(Size),
        ExecRanges(std::move(ExecRanges)),
        WritableRanges(std::move(WritableRanges)) {}

  void finalize(OnFinalizedFunction OnFinalized) override {
    // The content has already been written into working memory, which (for an
    // in-process pool) is the executor memory, and all JITLink fixups are
    // applied before finalize() runs. Deliberately DO NOT apply any per-segment
    // memory protection here (no mprotect): the pool stays RW.
    //
    // Seal only executable pages, after all writes and before publication.
    // Batched allocations remain RW/NX until the explicit flush.
    if (!Pool->usesBatchedPageSeal()) {
      for (const ExecSegRange &R : ExecRanges)
        if (auto Err = Pool->sealCodeRange(reinterpret_cast<void *>(R.Addr),
                                           static_cast<size_t>(R.Size))) {
          EJIT_DIAG("finalize FAIL: sealCodeRange addr=0x%llx size=%llu",
                    static_cast<unsigned long long>(R.Addr),
                    static_cast<unsigned long long>(R.Size));
          OnFinalized(
              joinErrors(std::move(Err), Pool->restoreRxRange(Base, Size)));
          return;
        }
    }
    runFinalizeActions(
        G->allocActions(),
        [this, OnFinalized = std::move(OnFinalized)](
            Expected<std::vector<WrapperFunctionCall>> DeallocActions) mutable {
          if (!DeallocActions) {
            EJIT_DIAG("finalize FAIL: runFinalizeActions error base=%p", Base);
            OnFinalized(joinErrors(DeallocActions.takeError(),
                                   Pool->restoreRxRange(Base, Size)));
            return;
          }
          // Publish executable ranges only after every finalize action has
          // succeeded. Failed allocations must never look callable to peers.
          // Each executable range of this allocation carries the allocation's
          // runtime-writable extents (e.g. __profc_) so a peer core resolving
          // any executable pointer learns exactly which pages to enable_rw.
          // recordFinalizedRange REJECTS (returns false) an over-bound or
          // malformed writable set rather than truncating it; that must fail
          // finalize (no callable pointer) so a peer is never handed code whose
          // counter pages it cannot fully prepare. Restore W^X and report.
          for (const ExecSegRange &R : ExecRanges) {
            const bool Recorded =
                Pool->usesBatchedPageSeal()
                    ? Pool->recordPendingRange(
                          reinterpret_cast<void *>(R.Addr),
                          static_cast<size_t>(R.Size),
                          WritableRanges.empty() ? nullptr
                                                 : WritableRanges.data(),
                          static_cast<uint32_t>(WritableRanges.size()))
                    : Pool->recordFinalizedRange(
                          reinterpret_cast<void *>(R.Addr),
                          static_cast<size_t>(R.Size),
                          WritableRanges.empty() ? nullptr
                                                 : WritableRanges.data(),
                          static_cast<uint32_t>(WritableRanges.size()));
            if (!Recorded) {
              EJIT_DIAG(
                  "finalize FAIL: recordFinalizedRange rejected addr=0x%llx"
                  " writable=%zu",
                  static_cast<unsigned long long>(R.Addr),
                  WritableRanges.size());
              OnFinalized(joinErrors(
                  make_error<StringError>(
                      "EJitCodePool: finalized allocation has an over-bound or "
                      "malformed runtime-writable range set",
                      inconvertibleErrorCode()),
                  Pool->restoreRxRange(Base, Size)));
              return;
            }
          }
          if (Pool->usesBatchedPageSeal() && !ExecRanges.empty())
            Pool->notePendingAllocation();
          auto *Info = new FinalizedInfo();
          Info->Base = Base;
          Info->DeallocActions = std::move(*DeallocActions);
#ifndef NDEBUG
          G = nullptr; // mark finalized
#endif
          OnFinalized(FinalizedAlloc(ExecutorAddr::fromPtr(Info)));
        });
  }

  void abandon(OnAbandonedFunction OnAbandoned) override {
    // Pool bytes are not reclaimed, but fixed code-segment pages must not stay
    // writable after an abandoned link.
#ifndef NDEBUG
    G = nullptr;
#endif
    OnAbandoned(Pool->restoreRxRange(Base, Size));
  }

private:
  EJitCodePoolManager *Pool;
  LinkGraph *G;
  BasicLayout BL;
  void *Base;
  size_t Size;
  std::vector<ExecSegRange> ExecRanges;
  std::vector<EJitWritableRange> WritableRanges;
};

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(EJitCodePoolManager &Pool,
                                                     size_t PageSize)
    : NearPool_(Pool), PageSize_(PageSize) {}

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(
    EJitCodePoolManager &NearPool, EJitCodePoolManager &FarPool,
    size_t PageSize)
    : NearPool_(NearPool), FarPool_(&FarPool), PageSize_(PageSize) {}

EJitCodePoolManager &EJitCodePoolMemoryManager::selectPool(
    const JITLinkDylib *JD) const {
  if (FarPool_ && JD && StringRef(JD->getName()).starts_with("spec_t1_"))
    return *FarPool_;
  return NearPool_;
}

void EJitCodePoolMemoryManager::allocate(const JITLinkDylib *JD, LinkGraph &G,
                                         OnAllocatedFunction OnAllocated) {
  EJitCodePoolManager &Pool = selectPool(JD);
  BasicLayout BL(G);

  bool ExecOnly = true;
  bool HasSegments = false;
  bool FitsCompactAlign = true;
  for (auto &KV : BL.segments()) {
    HasSegments = true;
    if ((KV.first.getMemProt() & orc::MemProt::Exec) == orc::MemProt::None) {
      ExecOnly = false;
      break;
    }
    if (KV.second.Alignment > Pool.codeAlignment())
      FitsCompactAlign = false;
  }
  const bool Compact =
      Pool.usesBatchedPageSeal() && HasSegments && ExecOnly && FitsCompactAlign;
  const size_t LayoutAlign = Compact ? Pool.codeAlignment() : PageSize_;
  auto SegsSizes = BL.getContiguousPageBasedLayoutSizes(LayoutAlign);
  if (!SegsSizes) {
    EJIT_DIAG("allocate FAIL: layout sizes error graph=%s",
              G.getName().c_str());
    OnAllocated(SegsSizes.takeError());
    return;
  }

  uint64_t Total = SegsSizes->total();
  const char *Placement = FarPool_ == &Pool ? "far" : "near";
  EJIT_DIAG_DEBUG(
      "allocate: graph=%s pool=%s total=%llu layoutAlign=%zu compact=%u",
      G.getName().c_str(), Placement, static_cast<unsigned long long>(Total),
      LayoutAlign, static_cast<unsigned>(Compact));

  void *Slab = nullptr;
  if (Total > 0) {
    auto MemOrErr = Pool.allocateCode(static_cast<size_t>(Total), LayoutAlign);
    if (!MemOrErr) {
      EJIT_DIAG("allocate FAIL: pool allocateCode total=%llu",
                static_cast<unsigned long long>(Total));
      OnAllocated(MemOrErr.takeError());
      return;
    }
    Slab = *MemOrErr;
    // Code-segment fixed-pool placement: the slab sits in the RX code segment,
    // so make its pages writable (RX -> RW via enable_rw) BEFORE any write. In
    // data-region placement this is a no-op (already RW). Failure means the slab
    // is not writable - do not hand it back for JITLink to write into.
    if (auto Err = Pool.enableRwRange(Slab, static_cast<size_t>(Total))) {
      EJIT_DIAG("allocate FAIL: enableRwRange total=%llu",
                static_cast<unsigned long long>(Total));
      OnAllocated(std::move(Err));
      return;
    }
    // Zero-fill the whole slab up-front (covers zero-fill segments and any
    // inter-segment page padding).
    std::memset(Slab, 0, static_cast<size_t>(Total));
  }

  auto *SlabBytes = static_cast<char *>(Slab);
  auto NextStandardSegAddr = ExecutorAddr::fromPtr(SlabBytes);
  auto NextFinalizeSegAddr =
      ExecutorAddr::fromPtr(SlabBytes + SegsSizes->StandardSegs);

  // Collect the EXECUTABLE segments' assigned ranges as we lay out the slab.
  // Only segments whose permission includes Exec need execute permission; the
  // (page-aligned) layout guarantees an executable segment never shares a 4KiB
  // page with a writable/read-only one, so sealing these ranges never flips a
  // data/GOT page to RX. An allocation may have several executable segments.
  //
  // In parallel, collect the RUNTIME-WRITABLE segments (Write but NOT Exec):
  // these are the pages the JIT function writes at runtime (e.g. the Tier-1
  // __profc_ counters). A peer core must enable_rw exactly these before it may
  // execute the code; read-only data (e.g. __profd_) is deliberately excluded
  // because a peer reads it fine from an RX page. The same page-aligned layout
  // guarantees a writable segment never shares a 4KiB page with an executable
  // one, so making these RW on a peer never touches a code page (no RWX).
  std::vector<ExecSegRange> ExecRanges;
  std::vector<EJitWritableRange> WritableRanges;
  for (auto &KV : BL.segments()) {
    auto &AG = KV.first;
    auto &Seg = KV.second;

    auto &SegAddr = (AG.getMemLifetime() == orc::MemLifetime::Standard)
                        ? NextStandardSegAddr
                        : NextFinalizeSegAddr;

    Seg.WorkingMem = SegAddr.toPtr<char *>();
    Seg.Addr = SegAddr;
    uint64_t SegSize =
        static_cast<uint64_t>(Seg.ContentSize) + Seg.ZeroFillSize;
    bool IsExec = (AG.getMemProt() & orc::MemProt::Exec) != orc::MemProt::None;
    bool IsWrite =
        (AG.getMemProt() & orc::MemProt::Write) != orc::MemProt::None;
    if (IsExec) {
      if (SegSize > 0)
        ExecRanges.push_back(
            {reinterpret_cast<uintptr_t>(SegAddr.toPtr<char *>()), SegSize});
    } else if (IsWrite) {
      if (SegSize > 0)
        WritableRanges.push_back(
            {reinterpret_cast<uintptr_t>(SegAddr.toPtr<char *>()), SegSize});
    }
    SegAddr += alignTo(Seg.ContentSize + Seg.ZeroFillSize, LayoutAlign);
  }

  // Bounded, never-truncated writable set: an allocation with more writable
  // data segments than the fixed descriptor can carry is a clean reject here
  // (before any code is executable) rather than a silent drop that would leave
  // a peer core faulting on an un-prepared counter page.
  if (WritableRanges.size() > kEJitMaxWritableRanges) {
    EJIT_DIAG("allocate FAIL: writable segments=%zu > max=%u graph=%s",
              WritableRanges.size(), kEJitMaxWritableRanges,
              G.getName().c_str());
    OnAllocated(joinErrors(
        make_error<StringError>(
            "EJitCodePool: allocation has more runtime-writable segments than "
            "the fixed cross-core bound",
            inconvertibleErrorCode()),
        Pool.restoreRxRange(Slab, static_cast<size_t>(Total))));
    return;
  }

  if (auto Err = BL.apply()) {
    EJIT_DIAG("allocate FAIL: BasicLayout apply error graph=%s",
              G.getName().c_str());
    OnAllocated(
        joinErrors(std::move(Err),
                   Pool.restoreRxRange(Slab, static_cast<size_t>(Total))));
    return;
  }

  EJIT_DIAG_DEBUG(
      "allocate OK: slab=%p total=%llu execRanges=%zu writableRanges=%zu", Slab,
      static_cast<unsigned long long>(Total), ExecRanges.size(),
      WritableRanges.size());
  OnAllocated(std::make_unique<InFlightAllocImpl>(
      Pool, G, std::move(BL), Slab, static_cast<size_t>(Total),
      std::move(ExecRanges), std::move(WritableRanges)));
}

void EJitCodePoolMemoryManager::deallocate(
    std::vector<FinalizedAlloc> Allocs, OnDeallocatedFunction OnDeallocated) {
  EJIT_DIAG_DEBUG("deallocate: %zu finalized alloc(s)", Allocs.size());
  Error DeallocErr = Error::success();
  for (auto &Alloc : Allocs) {
    auto *Info = Alloc.release().toPtr<FinalizedInfo *>();
    // Run dealloc actions in reverse order. Pool memory is intentionally not
    // released in v1 (sealed/RX pages must not be recycled; see design doc).
    while (!Info->DeallocActions.empty()) {
      if (auto Err = Info->DeallocActions.back().runWithSPSRetErrorMerged()) {
        EJIT_DIAG("deallocate FAIL: dealloc action error base=%p", Info->Base);
        DeallocErr = joinErrors(std::move(DeallocErr), std::move(Err));
      }
      Info->DeallocActions.pop_back();
    }
    delete Info;
  }
  OnDeallocated(std::move(DeallocErr));
}
