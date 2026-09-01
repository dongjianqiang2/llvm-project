//===-- EJitCodePool.h - EmbeddedJIT SRE machine-code memory pool ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Dedicated machine-code memory pool for EmbeddedJIT on SRE-style targets.
//
//  Background: the target's execute-permission primitive (enable_ex) flips
//  permissions on the 4KiB page containing the supplied VA, but the underlying
//  large page is still 2MiB: a 2MiB-aligned region must first be split into 4K
//  mappings via split_2m_to_4k before any per-page enable_ex is legal. Wrapping
//  the global mprotect breaks ORC/JITLink's later writes, so instead
//  EmbeddedJIT owns the JIT code memory directly. There are two sealing modes:
//
//    * Legacy whole-pool seal (fourKSeal = false): each 2MiB-aligned pool is
//      sealed as a unit (one enable_ex on the pool base); a sealed pool is
//      never written or allocated from again.
//    * 4K page seal (fourKSeal = true): the pool is still a 2MiB-aligned region
//      (split into 4K mappings at creation), but only the 4KiB pages that a
//      finalized allocation actually covers are sealed (one enable_ex per
//      page). Each allocation starts on a fresh 4K page and is rounded up to a
//      4K multiple, so subsequent allocations never touch an already-RX page
//      and the rest of the pool stays RW. This is far cheaper than burning a
//      whole 2MiB pool per function.
//
//  In both modes, while JITLink writes machine code the affected memory stays
//  RW; the RW->RX seal happens only after finalize, before a function pointer
//  is handed back to the caller. New JIT code always lands in writable memory.
//
//  This class is intentionally free of any SRE header dependency: the raw
//  allocator and the seal (enable_ex) primitive are injected as callbacks so
//  that the manager is fully unit-testable on a host with mocks. The thin SRE
//  platform adapter lives in EJitSrePlatform.h and is only pulled in when
//  EJIT_SRE_CODE_POOL is enabled.
//
//  Memory boundary: the *machine code bytes* always come from the injected raw
//  allocator (SRE on the target), never from malloc/new/business heap. The
//  small pool-descriptor bookkeeping below is ordinary host memory and never
//  holds executable code.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOL_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOL_H

#include "llvm/ExecutionEngine/EJIT/EJitCodeRange.h"
#include "llvm/Support/Error.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#ifndef EJIT_FREESTANDING
#include <mutex>
#endif

namespace llvm {
namespace ejit {

/// A single 2MiB-aligned JIT code pool.
struct CodePool {
  /// Raw pointer returned by the injected allocator (SRE_MemDbgAlloc on the
  /// target). Retained for debugging and possible future reclamation.
  uint8_t *raw = nullptr;
  /// 2MiB-aligned usable base inside [raw, raw + rawSize).
  uint8_t *base = nullptr;
  /// Usable size of the pool (the configured pool size, default 2MiB).
  size_t size = 0;
  /// Bump-allocation offset within [base, base + size).
  size_t used = 0;
  /// Whole-pool seal flag (legacy mode only): false = RW and still accepting
  /// allocations; true = sealed RX, frozen. In 4K seal mode the pool is sealed
  /// per page and this stays false (the pool keeps serving fresh-page allocs).
  bool executable = false;
};

/// Manages a set of 2MiB-aligned JIT code pools with bump allocation and
/// RW->RX sealing (whole-pool or per-4K-page). Not coupled to LLVM containers
/// for the code bytes themselves; the code storage comes exclusively from the
/// injected raw allocator.
class EJitCodePoolManager {
public:
  /// Allocate at least `bytes` of writable memory. Returns nullptr on failure.
  /// On the target this is backed by SRE_MemDbgAlloc; in tests, by a mock.
  using RawAllocFn = std::function<void *(size_t bytes)>;

  /// Seal one execute-permission unit, switching it to RX. Returns 0 on
  /// success, non-zero on failure. On the target this calls enable_ex(1, va);
  /// in tests, a mock. In legacy whole-pool mode `va` is the 2MiB pool base; in
  /// 4K page-seal mode it is the base VA of a single 4KiB page.
  using SealFn = std::function<unsigned(void *va)>;

  /// Split a 2MiB-aligned region [base, base + size) into 4KiB mappings so the
  /// platform can later flip execute permission per 4K page. Returns 0 on
  /// success, non-zero on failure. On the target this calls
  /// split_2m_to_4k(base, size); in tests, a mock. Only used when
  /// Options::fourKSeal is set.
  using SplitFn = std::function<unsigned(void *base, size_t size)>;

  /// Make one 4KiB page writable (RX -> RW) so JIT code can be written into a
  /// fixed region placed in the executable code segment (which is RX by
  /// default). Returns 0 on success, non-zero on failure. On the target this
  /// calls enable_rw(va); in tests, a mock. Only used when
  /// Options::needsEnableRw is set (code-segment fixed-pool placement).
  ///
  /// W^X contract: the platform enable_rw MUST clear execute permission
  /// (set UXN/PXN on AArch64) as well as set write permission, so the page
  /// transitions RX -> RW (writable, NOT executable) during the write window.
  /// Flipping only the write/AP bit would leave the page RWX, violating W^X.
  /// enable_ex (seal, RW -> RX) is the symmetric inverse.
  using EnableRwFn = std::function<unsigned(void *va)>;

  struct Options {
    /// Diagnostic placement class propagated with finalized code ranges.
    EJitCodePoolKind kind = EJitCodePoolKind::Unknown;
    /// Usable bytes per pool (EJIT_SRE_CODE_POOL_SIZE). Default 2MiB. In 4K
    /// seal mode this is rounded up to a multiple of poolAlign.
    size_t poolSize = static_cast<size_t>(2) * 1024 * 1024;
    /// Alignment of each pool base — the large-page / split granularity.
    /// Default 2MiB.
    size_t poolAlign = static_cast<size_t>(2) * 1024 * 1024;
    /// Minimum alignment applied to every code allocation. Default 64.
    size_t minCodeAlign = 64;
    /// When true, seal execute permission per 4KiB page (split_2m_to_4k at pool
    /// creation + enable_ex per covered page at finalize) instead of sealing
    /// the whole 2MiB pool. Default false (legacy whole-pool seal).
    bool fourKSeal = false;
    /// Keep finalized code RW/NX until flushPendingRanges(), allowing pure
    /// executable allocations to share pages at minCodeAlign granularity.
    bool batchedPageSeal = false;
    /// Execute-permission seal granularity in 4K mode. Platform constant 4KiB.
    size_t sealPageSize = 4096;
    /// When fixedSize > 0, new pools are carved sequentially from the fixed
    /// pre-reserved region [fixedBase, fixedBase + fixedSize) (e.g. a
    /// linker-script .text.ejit bounded by __ejit_code_start/__ejit_code_end)
    /// instead of calling RawAllocFn. The region MUST be poolAlign-aligned
    /// (2MiB); Split_/Seal_ still apply. Two placement modes:
    ///   * data-region (default, needsEnableRw=false): region is RW at load,
    ///     writable directly; enable_ex seals .text pages to RX.
    ///   * code-segment (needsEnableRw=true): region is RX at load (placed in
    ///     the code segment for bl/adrp reach to AOT .text); each slab must be
    ///     enable_rw'd (RX->RW) before writing, then enable_ex'd (RW->RX) at
    ///     finalize. Requires an EnableRwFn.
    /// Exhausting the region is a clean Error (no fallback to RawAllocFn, which
    /// would break the fixed-address guarantee). Default 0 = dynamic
    /// allocation.
    uintptr_t fixedBase = 0;
    size_t fixedSize = 0;
    /// When true, the fixed region starts read-only (code segment, RX) and each
    /// slab is enable_rw'd before writing. Ignored unless fixedSize > 0.
    /// Default false (data-region placement, RW).
    bool needsEnableRw = false;
  };

  struct Stats {
    size_t poolCount = 0;       ///< total pools created
    size_t sealedCount = 0;     ///< pools currently sealed (RX)
    size_t activeCount = 0;     ///< pools still RW
    size_t usedBytes = 0;       ///< sum of bump offsets across all pools
    size_t reservedBytes = 0;   ///< sum of pool sizes across all pools
    size_t wastedBytes = 0;     ///< unused tail bytes inside sealed pools
    size_t sealInvocations = 0; ///< number of successful seal (enable_ex) calls
                                ///< (per 4K page in 4K seal mode)
    size_t splitInvocations = 0; ///< number of successful split_2m_to_4k calls
                                 ///< (one per pool in 4K seal mode)
    size_t rwEnableInvocations = 0; ///< number of successful enable_rw calls
                                    ///< (per 4K page, code-segment mode only)
    size_t finalizedRangeCount = 0; ///< distinct executable ranges recorded
                                    ///< (duplicates are not double-counted)
    /// Union of the executable extents in finalized/pending range records.
    /// This is code bytes, not pool bump usage (which includes alignment and
    /// abandoned tails).
    size_t finalizedExecBytes = 0;
    size_t pendingExecBytes = 0;
    size_t pendingRangeCount = 0;
    size_t pendingAllocationCount = 0;
  };

  EJitCodePoolManager(Options Opts, RawAllocFn Alloc, SealFn Seal,
                      SplitFn Split = nullptr, EnableRwFn EnableRw = nullptr);
  ~EJitCodePoolManager();

  EJitCodePoolManager(const EJitCodePoolManager &) = delete;
  EJitCodePoolManager &operator=(const EJitCodePoolManager &) = delete;

  /// Bump-allocate `Size` bytes of RW code memory aligned to
  /// max(Align, minCodeAlign) (and to sealPageSize in 4K seal mode). Allocation
  /// strategy:
  ///   1. no active pool          -> new 2MiB-aligned pool
  ///   2. active pool out of room  -> new 2MiB-aligned pool
  ///   3. (legacy mode) active pool sealed -> new pool; a full active pool is
  ///      sealed before rolling over so it is never written again
  ///   4. otherwise                -> bump-allocate inside the active pool
  /// In 4K seal mode every allocation starts on a fresh 4KiB page and its used
  /// extent is rounded up to a 4K multiple, so a later allocation never lands
  /// on an already-sealed (RX) page; pools are not whole-sealed on rollover.
  /// A request larger than the pool size is a clean error (no silent fallback).
  /// If sealing the full active pool (legacy case 3) fails, returns that Error.
  Expected<void *> allocateCode(size_t Size, size_t Align);

  /// Seal the pool that contains `Ptr` (RW -> RX) if it is not already sealed.
  /// Idempotent: a second call for an address in an already-sealed pool is a
  /// no-op success and does not re-invoke enable_ex. Returns an Error if the
  /// pointer is not owned by any pool, or if enable_ex fails.
  Error sealPoolContaining(const void *Ptr);

  /// Seal every pool that is still writable. Used at shutdown/quiesce points.
  Error sealAllWritablePools();

  /// 4K seal mode: seal the 4KiB pages covering [Start, Start + Size), i.e.
  /// [alignDown(Start, sealPageSize), alignUp(Start + Size, sealPageSize)),
  /// invoking enable_ex(1, pageVA) once per page. Used after a JIT allocation
  /// has been fully written/finalized, before its function pointer is returned.
  /// Returns an Error if `Start` is not owned by any pool or if any page seal
  /// fails (in which case no callable pointer must be handed back).
  Error sealCodeRange(const void *Start, size_t Size);

  /// Code-segment mode (Options::needsEnableRw): make the 4KiB pages covering
  /// [Start, Start + Size) writable (RX -> RW) so JITLink can write code into
  /// the fixed region placed in the executable code segment. Called by the
  /// code-pool memory manager at allocate, BEFORE memset/JITLink writes.
  /// No-op (returns success) when needsEnableRw is false (data-region placement
  /// is already RW). Returns an Error if `Start` is not owned by any pool or if
  /// any enable_rw fails (in which case the slab must not be written).
  Error enableRwRange(const void *Start, size_t Size);

  /// Code-segment mode failure cleanup: seal every page covering
  /// [Start, Start + Size) back to RX. This is used when an allocation was
  /// made writable but JITLink abandons it or fails before publication. No-op
  /// when needsEnableRw is false. All pages are attempted and failures are
  /// joined so a partial W^X restoration cannot be hidden.
  Error restoreRxRange(const void *Start, size_t Size);

  /// True if this manager seals execute permission per 4KiB page (rather than
  /// per whole 2MiB pool).
  bool usesPageSeal() const { return Opts_.fourKSeal; }

  /// True when this pool is a fixed RX code-segment region whose slabs must be
  /// enable_rw'd (RX -> RW) before writing (Options::needsEnableRw). A peer
  /// core executing this pool's code must therefore make the runtime-writable
  /// pages writable in its own translation context. False for a dynamic pool
  /// (SRE_MemDbgAlloc), whose backing memory is already RW, so a peer needs no
  /// enable_rw. findRange() stamps this into EJitCompiledCodeInfo::
  /// requiresPeerEnableRw so the shared cache can gate per-core enable_rw
  /// correctly (the same JITLink layout is used by both pool kinds).
  bool needsPeerEnableRw() const { return Opts_.needsEnableRw; }

  bool usesBatchedPageSeal() const { return Opts_.batchedPageSeal; }
  size_t codeAlignment() const { return Opts_.minCodeAlign; }

  /// Stage a fully linked executable range without making it callable yet.
  /// All staged ranges are sealed and promoted atomically as one publication
  /// batch by flushPendingRanges().
  bool recordPendingRange(const void *Base, size_t Size,
                          const EJitWritableRange *Writables = nullptr,
                          uint32_t WritableCount = 0);
  void notePendingAllocation();
  Error flushPendingRanges();
  size_t pendingRangeCount() const;

  /// Record the executable extent of a finalized JITLink allocation
  /// [Base, Base + Size). Called by the code-pool memory manager at finalize,
  /// after all writes/relocations are complete (and, in 4K mode, after the
  /// range has been sealed RX). The recorded range is the real, fully-prepared
  /// executable allocation; findRange() later resolves a function pointer back
  /// to it so a peer core can seal exactly the pages it covers. A zero-size
  /// record is ignored.
  ///
  /// \p Writables / \p WritableCount give the allocation's runtime-writable
  /// data extents (e.g. the Tier-1 __profc_ counters) that a peer core must
  /// enable_rw before executing this code. They are recorded verbatim against
  /// the executable range and returned by findRange(). Returns true when the
  /// executable range was recorded, false when it was REJECTED (and therefore
  /// NOT recorded, so findRange() will not resolve \p Base). Rejection cases —
  /// each a clean fallback, never a silent truncation to zero:
  ///   * WritableCount > kEJitMaxWritableRanges (over-bound set);
  ///   * WritableCount > 0 but Writables == nullptr (malformed);
  ///   * any writable range with size 0, an address wrap, or an extent not
  ///     wholly inside the pool that owns \p Base.
  /// A null/zero writable set (WritableCount == 0) records the executable range
  /// with no writable data and returns true. A benign no-op (Base null / Size
  /// 0) also returns true.
  bool recordFinalizedRange(const void *Base, size_t Size,
                            const EJitWritableRange *Writables = nullptr,
                            uint32_t WritableCount = 0);

  /// Resolve a function pointer to the finalized executable allocation and pool
  /// that contain it, filling \p Out (codeStart/codeSize from the recorded
  /// allocation, poolBase/poolSize/poolId from the owning pool, fnPtr = Ptr).
  /// Returns false (and leaves \p Out unspecified) when \p Ptr is not inside
  /// any recorded finalized range owned by a known pool — the caller must then
  /// take a clean fallback and never publish a shared pointer with no range.
  bool findRange(const void *Ptr, EJitCompiledCodeInfo &Out) const;

  /// True if `Ptr` falls inside the usable range of any owned pool.
  bool contains(const void *Ptr) const;

  /// Snapshot of pool statistics (thread-safe).
  Stats getStats() const;

private:
  CodePool *findPoolLocked(const void *Ptr);
  Error newActivePoolLocked();
  Error sealPoolLocked(CodePool &P);
  bool poolHasRoomLocked(const CodePool &P, size_t Size, size_t Align) const;

  Options Opts_;
  RawAllocFn Alloc_;
  SealFn Seal_;
  SplitFn Split_;
  EnableRwFn EnableRw_;

  // Pool descriptors (ordinary host bookkeeping; never holds code bytes).
  std::vector<std::unique_ptr<CodePool>> Pools_;
  CodePool *Active_ = nullptr;
  size_t SealInvocations_ = 0;
  size_t SplitInvocations_ = 0;
  size_t RwEnableInvocations_ = 0;
  /// Bump cursor (bytes consumed) over the fixed region [fixedBase, fixedBase +
  /// fixedSize) when Options::fixedSize > 0; unused in dynamic-allocation mode.
  size_t FixedUsed_ = 0;

  /// Finalized executable allocation extents (ordinary host bookkeeping; never
  /// holds code bytes). Recorded at finalize, queried by findRange(). Append
  /// only in v1 (pool memory is not reclaimed), so a pointer never resolves to
  /// a stale range.
  struct FinalizedRange {
    uintptr_t start = 0;
    uint64_t size = 0;
    /// Runtime-writable data extents of THIS allocation (e.g. __profc_) that a
    /// peer core must enable_rw before executing the code. 0..writableCount are
    /// valid; the rest are unused. Bounded, POD, never truncated (overflow is
    /// rejected at record time).
    uint32_t writableCount = 0;
    EJitWritableRange writables[kEJitMaxWritableRanges] = {};
  };
  std::vector<FinalizedRange> FinalizedRanges_;
  std::vector<FinalizedRange> PendingRanges_;
  size_t PendingAllocations_ = 0;

#ifndef EJIT_FREESTANDING
  mutable std::mutex Mutex_;
#endif
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOL_H
