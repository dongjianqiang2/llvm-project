//===-- EJitCodePoolMemoryManager.cpp - JITLink mem mgr over code pool ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkDylib.h"
#include "llvm/ExecutionEngine/Orc/Shared/AllocationActions.h"
#include "llvm/ExecutionEngine/Orc/Shared/MemoryFlags.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <cstring>
#ifndef EJIT_FREESTANDING
#include <mutex>
#endif
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
  EJitCodePoolManager *Pool = nullptr;
  uintptr_t Addr = 0;
  uint64_t Size = 0;
};

struct PoolAllocRange {
  EJitCodePoolManager *Pool = nullptr;
  void *Base = nullptr;
  size_t Size = 0;
};
} // namespace

/// Side record used as the FinalizedAlloc handle. Holds the dealloc actions and
/// the pool-backed base address. Pool memory itself is not freed in v1.
struct EJitCodePoolMemoryManager::FinalizedInfo {
  void *Base = nullptr;
  std::vector<WrapperFunctionCall> DeallocActions;
};

struct EJitCodePoolMemoryManager::State {
  struct AllocationRecord {
    std::vector<ExecSegRange> ExecRanges;
  };
  // Code-pool v1 never reclaims executable storage, so allocation addresses
  // remain valid for the manager lifetime. If reclamation is introduced,
  // deallocate() must remove these records and this lookup should be indexed.
  std::vector<AllocationRecord> Allocations;
#ifndef EJIT_FREESTANDING
  mutable std::mutex Mutex;
#endif
};

class EJitCodePoolMemoryManager::InFlightAllocImpl
    : public JITLinkMemoryManager::InFlightAlloc {
public:
  InFlightAllocImpl(EJitCodePoolMemoryManager &Owner, LinkGraph &G,
                    BasicLayout BL, std::vector<PoolAllocRange> Allocs,
                    std::vector<ExecSegRange> ExecRanges,
                    std::vector<EJitWritableRange> WritableRanges)
      : Owner(&Owner), G(&G), BL(std::move(BL)), Allocs(std::move(Allocs)),
        ExecRanges(std::move(ExecRanges)),
        WritableRanges(std::move(WritableRanges)) {}

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
    for (const ExecSegRange &R : ExecRanges) {
      if (!R.Pool->usesPageSeal())
        continue;
      if (auto Err = R.Pool->sealCodeRange(reinterpret_cast<void *>(R.Addr),
                                           static_cast<size_t>(R.Size))) {
        EJIT_DIAG("finalize FAIL: sealCodeRange addr=0x%llx size=%llu",
                  static_cast<unsigned long long>(R.Addr),
                  static_cast<unsigned long long>(R.Size));
        for (const PoolAllocRange &A : Allocs)
          Err = joinErrors(std::move(Err),
                           A.Pool->restoreRxRange(A.Base, A.Size));
        OnFinalized(std::move(Err));
        return;
      }
    }
    runFinalizeActions(
        G->allocActions(),
        [this, OnFinalized = std::move(OnFinalized)](
            Expected<std::vector<WrapperFunctionCall>> DeallocActions) mutable {
          if (!DeallocActions) {
            Error Err = DeallocActions.takeError();
            for (const PoolAllocRange &A : Allocs)
              Err = joinErrors(std::move(Err),
                               A.Pool->restoreRxRange(A.Base, A.Size));
            EJIT_DIAG("finalize FAIL: runFinalizeActions error");
            OnFinalized(std::move(Err));
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
            SmallVector<EJitWritableRange, kEJitMaxWritableRanges> LocalWrites;
            for (const EJitWritableRange &W : WritableRanges)
              if (R.Pool->contains(reinterpret_cast<void *>(W.addr)))
                LocalWrites.push_back(W);
            if (!R.Pool->recordFinalizedRange(
                    reinterpret_cast<void *>(R.Addr),
                    static_cast<size_t>(R.Size),
                    LocalWrites.empty() ? nullptr : LocalWrites.data(),
                    static_cast<uint32_t>(LocalWrites.size()))) {
              EJIT_DIAG(
                  "finalize FAIL: recordFinalizedRange rejected addr=0x%llx"
                  " writable=%zu",
                  static_cast<unsigned long long>(R.Addr),
                  WritableRanges.size());
              Error Err = make_error<StringError>(
                  "EJitCodePool: finalized allocation has an over-bound or "
                  "malformed runtime-writable range set",
                  inconvertibleErrorCode());
              for (const PoolAllocRange &A : Allocs)
                Err = joinErrors(std::move(Err),
                                 A.Pool->restoreRxRange(A.Base, A.Size));
              OnFinalized(std::move(Err));
              return;
            }
          }
          {
#ifndef EJIT_FREESTANDING
            std::lock_guard<std::mutex> Lock(Owner->State_->Mutex);
#endif
            Owner->State_->Allocations.push_back({ExecRanges});
          }
          auto *Info = new FinalizedInfo();
          Info->Base = Allocs.empty() ? nullptr : Allocs.front().Base;
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
    Error Err = Error::success();
    for (const PoolAllocRange &A : Allocs)
      Err = joinErrors(std::move(Err), A.Pool->restoreRxRange(A.Base, A.Size));
    OnAbandoned(std::move(Err));
  }

private:
  EJitCodePoolMemoryManager *Owner;
  LinkGraph *G;
  BasicLayout BL;
  std::vector<PoolAllocRange> Allocs;
  std::vector<ExecSegRange> ExecRanges;
  std::vector<EJitWritableRange> WritableRanges;
};

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(EJitCodePoolManager &Pool,
                                                     size_t PageSize)
    : NearPool_(Pool), PageSize_(PageSize), State_(std::make_unique<State>()) {}

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(
    EJitCodePoolManager &NearPool, EJitCodePoolManager &FarPool,
    size_t PageSize)
    : NearPool_(NearPool), FarPool_(&FarPool), PageSize_(PageSize),
      State_(std::make_unique<State>()) {}

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(
    EJitCodePoolManager &NearPool, EJitCodePoolManager &ColdPool,
    EJitCodePoolManager &FarPool, size_t PageSize)
    : NearPool_(NearPool), ColdPool_(&ColdPool), FarPool_(&FarPool),
      PageSize_(PageSize), State_(std::make_unique<State>()) {}

EJitCodePoolMemoryManager::~EJitCodePoolMemoryManager() = default;

EJitCodePoolManager &EJitCodePoolMemoryManager::selectPool(
    const JITLinkDylib *JD) const {
  if (FarPool_ && JD && StringRef(JD->getName()).starts_with("spec_t1_"))
    return *FarPool_;
  return NearPool_;
}

bool EJitCodePoolMemoryManager::findAllocation(
    const void *Ptr, EJitCompiledCodeInfo &Out) const {
  const uintptr_t Addr = reinterpret_cast<uintptr_t>(Ptr);
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Lock(State_->Mutex);
#endif
  for (const State::AllocationRecord &A : State_->Allocations) {
    const ExecSegRange *Primary = nullptr;
    for (const ExecSegRange &R : A.ExecRanges)
      if (Addr >= R.Addr && Addr < R.Addr + R.Size) {
        Primary = &R;
        break;
      }
    if (!Primary)
      continue;
    EJitCompiledCodeInfo Result{};
    if (!Primary->Pool->findRange(Ptr, Result))
      return false;
    for (const ExecSegRange &R : A.ExecRanges) {
      if (&R == Primary)
        continue;
      if (Result.extraCodeCount >= kEJitMaxExtraCodeRanges)
        return false;
      EJitCompiledCodeInfo ExtraInfo{};
      if (!R.Pool->findRange(reinterpret_cast<void *>(R.Addr), ExtraInfo))
        return false;
      EJitExecutableRange &Extra =
          Result.extraCodeRanges[Result.extraCodeCount++];
      Extra.codeStart = ExtraInfo.codeStart;
      Extra.codeSize = ExtraInfo.codeSize;
      Extra.poolBase = ExtraInfo.poolBase;
      Extra.poolSize = ExtraInfo.poolSize;
      Extra.poolId = ExtraInfo.poolId;
      Extra.poolKind = ExtraInfo.poolKind;
    }
    Out = Result;
    return true;
  }
  return false;
}

void EJitCodePoolMemoryManager::allocate(const JITLinkDylib *JD, LinkGraph &G,
                                         OnAllocatedFunction OnAllocated) {
  EJitCodePoolManager &Pool = selectPool(JD);
  std::vector<PoolAllocRange> Allocs;
  std::vector<ExecSegRange> ExecRanges;
  auto RestoreAll = [&Allocs]() {
    Error Err = Error::success();
    for (const PoolAllocRange &A : Allocs)
      Err = joinErrors(std::move(Err), A.Pool->restoreRxRange(A.Base, A.Size));
    return Err;
  };

  // MFS emits profile-cold blocks as .text.split.<function>. Give those blocks
  // addresses and working memory from the dedicated fixed cold pool, then mark
  // the sections NoAlloc so BasicLayout lays out the remaining graph in the
  // normal hot/far pool without merging both RX classes back together.
  if (ColdPool_ && &Pool == &NearPool_) {
    SmallVector<Section *, 4> ColdSections;
    SmallVector<Block *, 16> ColdBlocks;
    for (Section &Sec : G.sections()) {
      if (!Sec.getName().starts_with(".text.split.") &&
          !Sec.getName().starts_with(".text.ejit_cold"))
        continue;
      if ((Sec.getMemProt() & orc::MemProt::Exec) == orc::MemProt::None)
        continue;
      ColdSections.push_back(&Sec);
      for (Block *B : Sec.blocks())
        ColdBlocks.push_back(B);
    }
    if (!ColdBlocks.empty()) {
      llvm::sort(ColdBlocks, [](const Block *L, const Block *R) {
        if (L->getSection().getOrdinal() != R->getSection().getOrdinal())
          return L->getSection().getOrdinal() < R->getSection().getOrdinal();
        return L->getAddress() < R->getAddress();
      });
      uint64_t ColdSize = 0;
      for (Block *B : ColdBlocks) {
        ColdSize = alignToBlock(ColdSize, *B);
        ColdSize += B->getSize();
      }
      ColdSize = alignTo(ColdSize, PageSize_);
      auto ColdMemOrErr =
          ColdPool_->allocateCode(static_cast<size_t>(ColdSize), PageSize_);
      if (!ColdMemOrErr) {
        OnAllocated(ColdMemOrErr.takeError());
        return;
      }
      void *ColdBase = *ColdMemOrErr;
      if (auto Err = ColdPool_->enableRwRange(ColdBase, ColdSize)) {
        OnAllocated(joinErrors(std::move(Err),
                               ColdPool_->restoreRxRange(ColdBase, ColdSize)));
        return;
      }
      std::memset(ColdBase, 0, static_cast<size_t>(ColdSize));
      uintptr_t ColdAddr = reinterpret_cast<uintptr_t>(ColdBase);
      size_t WorkingOffset = 0;
      for (Block *B : ColdBlocks) {
        ColdAddr = alignToBlock(ColdAddr, *B);
        WorkingOffset = static_cast<size_t>(
            alignToBlock(static_cast<uint64_t>(WorkingOffset), *B));
        B->setAddress(ExecutorAddr(ColdAddr));
        if (!B->isZeroFill()) {
          char *Dst = static_cast<char *>(ColdBase) + WorkingOffset;
          std::memcpy(Dst, B->getContent().data(), B->getSize());
          B->setMutableContent({Dst, B->getSize()});
        }
        ColdAddr += B->getSize();
        WorkingOffset += B->getSize();
      }
      for (Section *Sec : ColdSections)
        Sec->setMemLifetime(orc::MemLifetime::NoAlloc);
      Allocs.push_back({ColdPool_, ColdBase, static_cast<size_t>(ColdSize)});
      ExecRanges.push_back(
          {ColdPool_, reinterpret_cast<uintptr_t>(ColdBase), ColdSize});
      EJIT_DIAG("allocate: graph=%s cold=%llu bytes sections=%zu",
                G.getName().c_str(), static_cast<unsigned long long>(ColdSize),
                ColdSections.size());
    }
  }
  BasicLayout BL(G);

  auto SegsSizes = BL.getContiguousPageBasedLayoutSizes(PageSize_);
  if (!SegsSizes) {
    EJIT_DIAG("allocate FAIL: layout sizes error graph=%s",
              G.getName().c_str());
    OnAllocated(joinErrors(SegsSizes.takeError(), RestoreAll()));
    return;
  }

  uint64_t Total = SegsSizes->total();
  const char *Placement = FarPool_ == &Pool ? "far" : "near";
  EJIT_DIAG("allocate: graph=%s pool=%s total=%llu pageSize=%zu",
            G.getName().c_str(), Placement,
            static_cast<unsigned long long>(Total), PageSize_);

  void *Slab = nullptr;
  if (Total > 0) {
    auto MemOrErr = Pool.allocateCode(static_cast<size_t>(Total), PageSize_);
    if (!MemOrErr) {
      EJIT_DIAG("allocate FAIL: pool allocateCode total=%llu",
                static_cast<unsigned long long>(Total));
      OnAllocated(joinErrors(MemOrErr.takeError(), RestoreAll()));
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
      // enableRwRange seals every page it made writable before returning an
      // error. Only the earlier companion cold allocation remains to restore.
      OnAllocated(joinErrors(std::move(Err), RestoreAll()));
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
            {&Pool, reinterpret_cast<uintptr_t>(SegAddr.toPtr<char *>()),
             SegSize});
    } else if (IsWrite) {
      if (SegSize > 0)
        WritableRanges.push_back(
            {reinterpret_cast<uintptr_t>(SegAddr.toPtr<char *>()), SegSize});
    }
    SegAddr += alignTo(Seg.ContentSize + Seg.ZeroFillSize, PageSize_);
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
        joinErrors(Pool.restoreRxRange(Slab, static_cast<size_t>(Total)),
                   RestoreAll())));
    return;
  }

  if (ExecRanges.size() > 1 + kEJitMaxExtraCodeRanges) {
    Error Err = make_error<StringError>(
        "EJitCodePool: too many executable extents for cross-core metadata",
        inconvertibleErrorCode());
    Err = joinErrors(std::move(Err), RestoreAll());
    if (Total > 0)
      Err = joinErrors(std::move(Err),
                       Pool.restoreRxRange(Slab, static_cast<size_t>(Total)));
    OnAllocated(std::move(Err));
    return;
  }

  if (auto Err = BL.apply()) {
    EJIT_DIAG("allocate FAIL: BasicLayout apply error graph=%s",
              G.getName().c_str());
    OnAllocated(joinErrors(
        std::move(Err),
        joinErrors(Pool.restoreRxRange(Slab, static_cast<size_t>(Total)),
                   RestoreAll())));
    return;
  }

  EJIT_DIAG("allocate OK: slab=%p total=%llu execRanges=%zu writableRanges=%zu",
            Slab, static_cast<unsigned long long>(Total), ExecRanges.size(),
            WritableRanges.size());
  if (Total > 0)
    Allocs.push_back({&Pool, Slab, static_cast<size_t>(Total)});
  OnAllocated(std::make_unique<InFlightAllocImpl>(
      *this, G, std::move(BL), std::move(Allocs), std::move(ExecRanges),
      std::move(WritableRanges)));
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
