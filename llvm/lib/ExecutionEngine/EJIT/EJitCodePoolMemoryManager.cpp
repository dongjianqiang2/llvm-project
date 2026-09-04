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
constexpr size_t kMaxBatchedCompactAlign = 64;

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
    // In 4K seal mode, seal ONLY the executable segments' 4KiB pages now
    // that all writes/relocations are complete, before the function pointer can
    // be looked up. If any page fails to seal we must not hand back a callable
    // allocation. (Legacy whole-pool seal is driven later, at lookup, by the
    // engine.) We do not invalidate the instruction cache here either \u2014
    // the SRE seal callback does it (sealAndSyncCache: make page executable +
    // sync caches).
    if (Pool->usesPageSeal() && !Pool->usesBatchedPageSeal()) {
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
          // Pre-collect this graph's defined symbols (address,size) once: the
          // entry symbol's real size (fnSize) is recovered by findRange() from
          // these, matched against the published fnPtr. Collected before G is
          // marked finalized (below). isolateSpecializationEntry guarantees the
          // TU's ejit_entry has the sole defined body, so the executable
          // segment normally holds exactly one defined symbol (the entry).
          for (const ExecSegRange &R : ExecRanges) {
            EJitFnSymEntry Syms[kEJitMaxSymsPerRange];
            uint32_t SymCount = 0;
            if (G) {
              const uintptr_t SegStart = R.Addr;
              const uintptr_t SegEnd = R.Addr + R.Size;
              for (auto *Sym : G->defined_symbols()) {
                uintptr_t SAddr = Sym->getAddress().getValue();
                if (SAddr < SegStart || SAddr >= SegEnd)
                  continue;
                if (SymCount < kEJitMaxSymsPerRange) {
                  Syms[SymCount] = {SAddr,
                                   static_cast<uint64_t>(Sym->getSize())};
                  ++SymCount;
                }
                // over-bound: truncate (fnSize is diagnostic, not safety)
              }
            }
            const bool Recorded =
                Pool->usesBatchedPageSeal()
                    ? Pool->recordPendingRange(
                          reinterpret_cast<void *>(R.Addr),
                          static_cast<size_t>(R.Size),
                          WritableRanges.empty() ? nullptr
                                                 : WritableRanges.data(),
                          static_cast<uint32_t>(WritableRanges.size()),
                          SymCount ? Syms : nullptr, SymCount)
                    : Pool->recordFinalizedRange(
                          reinterpret_cast<void *>(R.Addr),
                          static_cast<size_t>(R.Size),
                          WritableRanges.empty() ? nullptr
                                                 : WritableRanges.data(),
                          static_cast<uint32_t>(WritableRanges.size()),
                          SymCount ? Syms : nullptr, SymCount);
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
    : NearPool_(&Pool), NearPools_{&Pool}, PageSize_(PageSize) {}

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(
    EJitCodePoolManager &NearPool, EJitCodePoolManager &FarPool,
    size_t PageSize)
    : NearPool_(&NearPool), NearPools_{&NearPool}, FarPool_(&FarPool),
      PageSize_(PageSize) {}

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(
    std::vector<EJitCodePoolManager *> NearPools, EJitCodePoolManager &FarPool,
    size_t PageSize, PoolSelector Selector)
    : NearPool_(NearPools.empty() ? nullptr : NearPools.front()),
      NearPools_(std::move(NearPools)), FarPool_(&FarPool),
      Selector_(std::move(Selector)), PageSize_(PageSize) {}

EJitCodePoolManager *
EJitCodePoolMemoryManager::selectPool(const JITLinkDylib *JD) const {
  if (Selector_) {
    if (EJitCodePoolManager *Pool = Selector_(JD))
      return Pool;
    EJIT_DIAG("selectPool: missing controlled JITDylib metadata name=%s",
              JD ? JD->getName().c_str() : "<null>");
    return nullptr;
  }
  if (FarPool_ && JD && StringRef(JD->getName()).starts_with("spec_t1_"))
    return FarPool_;
  if (!NearPools_.empty())
    return NearPools_.front();
  return NearPool_;
}

void EJitCodePoolMemoryManager::allocate(const JITLinkDylib *JD, LinkGraph &G,
                                         OnAllocatedFunction OnAllocated) {
  EJitCodePoolManager *SelectedPool = selectPool(JD);
  if (!SelectedPool) {
    OnAllocated(make_error<StringError>(
        "EJitCodePool: missing controlled pool metadata",
        inconvertibleErrorCode()));
    return;
  }
  EJitCodePoolManager &Pool = *SelectedPool;

  // Fold pure read-only sections (string constants, const arrays, jump
  // tables) into the executable segment BEFORE the layout is built.
  //
  // Why fold: BasicLayout groups blocks by {MemProt, MemLifetime}. A pure
  // read-only section (R--) and the code section (R+X) land in DIFFERENT
  // segments, and whether the layout is the compact one (batched
  // mode) or the 4K-page one (immediate mode), the two segments each occupy
  // their OWN 4KiB page(s) — a 37-byte .rodata costs a whole page and, in
  // batched mode, also breaks the ExecOnly compact stride. Promoting the
  // R-- section to R+X BEFORE BasicLayout merges it into the code segment,
  // so text and rodata lay out contiguous within one segment and share a
  // page. This wins in BOTH 4K seal sub-modes: batched (16-64 byte compact
  // stride across allocations) and immediate (contiguous within the single
  // allocation's page), and in both near and far pools.
  //
  // Why safe:
  //  - The constant bytes are never executed (the AOT path already embeds
  //    literal pools in .text), and even if executed they are on an RX page.
  //  - Sealing them RX with the code tightens W^X: previously the rodata
  //    page sat on a slab-wide enable_rw'd (RW) page and, being neither exec
  //    nor write, was never sealed back — staying RW in the fixed code
  //    segment. Folded, it seals RX with the code.
  //  - Writable sections (e.g. Tier-1 __profc_ counters, R+W) are excluded
  //    by the Write check, so the peer enable_rw protocol and the no-RWX
  //    invariant (write/exec pages never share) are untouched. __profd_
  //    (read-only) folding into the exec segment is fine: a peer reads it
  //    from the RX page, same as it reads code.
  //  - An over-aligned (greater than 64 bytes) read-only block still triggers
  //    the page-granular fallback: the merged segment's Alignment = max(block
  //    alignments) exceeds kMaxBatchedCompactAlign, so
  //    FitsCompactAlign=false (in batched mode) / LayoutAlign stays PageSize_
  //    (in immediate mode).
  //
  // Gated on usesPageSeal() (4K-seal, both sub-modes): the legacy 2MiB
  // whole-pool seal flips an entire pool RX off a single pointer, which
  // cannot carve out per-page W^X, so folding read-only data there would
  // lose the W^X tightening and is not done.
  if (Pool.usesPageSeal())
    for (Section &Sec : G.sections())
      if ((Sec.getMemProt() & orc::MemProt::Read) != orc::MemProt::None &&
          (Sec.getMemProt() & (orc::MemProt::Write | orc::MemProt::Exec)) ==
              orc::MemProt::None)
        Sec.setMemProt(orc::MemProt::Read | orc::MemProt::Exec);

  BasicLayout BL(G);

  bool ExecOnly = true;
  bool HasSegments = false;
  bool FitsCompactAlign = true;
  size_t SegmentCount = 0;
  size_t MaxSegmentAlign = 0;
  const size_t CompactAlignLimit =
      Pool.codeAlignment() > kMaxBatchedCompactAlign ? Pool.codeAlignment()
                                                     : kMaxBatchedCompactAlign;
  for (auto &KV : BL.segments()) {
    HasSegments = true;
    ++SegmentCount;
    const size_t SegmentAlign = KV.second.Alignment.value();
    if (SegmentAlign > MaxSegmentAlign)
      MaxSegmentAlign = SegmentAlign;
    if ((KV.first.getMemProt() & orc::MemProt::Exec) == orc::MemProt::None)
      ExecOnly = false;
    if (SegmentAlign > CompactAlignLimit)
      FitsCompactAlign = false;
  }
  const bool Compact =
      Pool.usesBatchedPageSeal() && HasSegments && ExecOnly && FitsCompactAlign;
  const size_t CompactAlign = MaxSegmentAlign > Pool.codeAlignment()
                                  ? MaxSegmentAlign
                                  : Pool.codeAlignment();
  const size_t LayoutAlign = Compact ? CompactAlign : PageSize_;
  auto SegsSizes = BL.getContiguousPageBasedLayoutSizes(LayoutAlign);
  if (!SegsSizes) {
    EJIT_DIAG("allocate FAIL: layout sizes error graph=%s",
              G.getName().c_str());
    OnAllocated(SegsSizes.takeError());
    return;
  }

  uint64_t Total = SegsSizes->total();
  [[maybe_unused]] const char *Placement = FarPool_ == &Pool ? "far" : "near";
  EJIT_DIAG_DEBUG(
      "allocate: graph=%s pool=%s total=%llu layoutAlign=%zu compact=%u "
      "batched=%u segments=%zu execOnly=%u fitsAlign=%u maxAlign=%zu "
      "configuredAlign=%zu compactAlignLimit=%zu",
      G.getName().c_str(), Placement, static_cast<unsigned long long>(Total),
      LayoutAlign, static_cast<unsigned>(Compact),
      static_cast<unsigned>(Pool.usesBatchedPageSeal()), SegmentCount,
      static_cast<unsigned>(ExecOnly), static_cast<unsigned>(FitsCompactAlign),
      MaxSegmentAlign, Pool.codeAlignment(), CompactAlignLimit);

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
