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
  InFlightAllocImpl(EJitCodePoolMemoryManager &MM, LinkGraph &G, BasicLayout BL,
                    void *Base, size_t Size,
                    std::vector<ExecSegRange> ExecRanges)
      : Pool(&MM.getPool()), G(&G), BL(std::move(BL)), Base(Base), Size(Size),
        ExecRanges(std::move(ExecRanges)) {}

  void finalize(OnFinalizedFunction OnFinalized) override {
    // The content has already been written into working memory, which (for an
    // in-process pool) is the executor memory, and all JITLink fixups are
    // applied before finalize() runs. Deliberately DO NOT apply any per-segment
    // memory protection here (no mprotect): the pool stays RW.
    //
    // In 4K seal mode, seal ONLY the executable segments' 4KiB pages now
    // that all writes/relocations are complete, before the function pointer can
    // be looked up. If any page fails to seal we must not hand back a callable
    // allocation. (Legacy whole-pool seal is driven later, at lookup, by the
    // engine.) We do not invalidate the instruction cache here either \u2014
    // the SRE seal callback does it (sealAndSyncCache: make page executable +
    // sync caches).
    if (Pool->usesPageSeal()) {
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
          for (const ExecSegRange &R : ExecRanges)
            Pool->recordFinalizedRange(reinterpret_cast<void *>(R.Addr),
                                       static_cast<size_t>(R.Size));
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
};

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(EJitCodePoolManager &Pool,
                                                     size_t PageSize)
    : Pool_(Pool), PageSize_(PageSize) {}

void EJitCodePoolMemoryManager::allocate(const JITLinkDylib *JD, LinkGraph &G,
                                         OnAllocatedFunction OnAllocated) {
  BasicLayout BL(G);

  auto SegsSizes = BL.getContiguousPageBasedLayoutSizes(PageSize_);
  if (!SegsSizes) {
    EJIT_DIAG("allocate FAIL: layout sizes error graph=%s",
              G.getName().c_str());
    OnAllocated(SegsSizes.takeError());
    return;
  }

  uint64_t Total = SegsSizes->total();
  EJIT_DIAG("allocate: graph=%s total=%llu pageSize=%zu",
            G.getName().c_str(),
            static_cast<unsigned long long>(Total), PageSize_);

  void *Slab = nullptr;
  if (Total > 0) {
    auto MemOrErr = Pool_.allocateCode(static_cast<size_t>(Total), PageSize_);
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
    if (auto Err = Pool_.enableRwRange(Slab, static_cast<size_t>(Total))) {
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
  std::vector<ExecSegRange> ExecRanges;
  for (auto &KV : BL.segments()) {
    auto &AG = KV.first;
    auto &Seg = KV.second;

    auto &SegAddr = (AG.getMemLifetime() == orc::MemLifetime::Standard)
                        ? NextStandardSegAddr
                        : NextFinalizeSegAddr;

    Seg.WorkingMem = SegAddr.toPtr<char *>();
    Seg.Addr = SegAddr;
    if ((AG.getMemProt() & orc::MemProt::Exec) != orc::MemProt::None) {
      uint64_t SegSize =
          static_cast<uint64_t>(Seg.ContentSize) + Seg.ZeroFillSize;
      if (SegSize > 0)
        ExecRanges.push_back(
            {reinterpret_cast<uintptr_t>(SegAddr.toPtr<char *>()), SegSize});
    }
    SegAddr += alignTo(Seg.ContentSize + Seg.ZeroFillSize, PageSize_);
  }

  if (auto Err = BL.apply()) {
    EJIT_DIAG("allocate FAIL: BasicLayout apply error graph=%s",
              G.getName().c_str());
    OnAllocated(
        joinErrors(std::move(Err),
                   Pool_.restoreRxRange(Slab, static_cast<size_t>(Total))));
    return;
  }

  EJIT_DIAG("allocate OK: slab=%p total=%llu execRanges=%zu", Slab,
            static_cast<unsigned long long>(Total), ExecRanges.size());
  OnAllocated(std::make_unique<InFlightAllocImpl>(*this, G, std::move(BL), Slab,
                                                  static_cast<size_t>(Total),
                                                  std::move(ExecRanges)));
}

void EJitCodePoolMemoryManager::deallocate(std::vector<FinalizedAlloc> Allocs,
                                           OnDeallocatedFunction OnDeallocated) {
  EJIT_DIAG("deallocate: %zu finalized alloc(s)", Allocs.size());
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
