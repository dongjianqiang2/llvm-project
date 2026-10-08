//===-- EJitSrePlatform.cpp - SRE platform adapter for the code pool ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Wires EJitCodePoolManager to the real SRE platform primitives. Compiled
//  only when EJIT_SRE_CODE_POOL is enabled. The platform symbols (enable_ex,
//  split_2m_to_4k, SRE_MemDbgAlloc) are ONLY declared here — never defined and
//  never given weak fallbacks. The real platform / business link environment
//  must supply their strong definitions; if a symbol is missing it must surface
//  as a link-time error rather than be silently satisfied by a no-op. Host unit
//  tests do not reference makeSreCodePoolManager (they inject mock callbacks
//  into EJitCodePoolManager directly), so this translation unit's external
//  references are never pulled into a host test link.
//
//===----------------------------------------------------------------------===//

#if defined(EJIT_FIXED_CODE_POOL) && !defined(EJIT_SRE_SHARED_TASKPOOL)
#error "EJIT_FIXED_CODE_POOL requires the shared taskpool"
#endif

#ifdef EJIT_SRE_CODE_POOL

#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPoolState.h" // seal/split granule contract

#include <cstdint>
#include <limits>
#include <type_traits>

#ifndef EJIT_SRE_CODE_POOL_SIZE
#define EJIT_SRE_CODE_POOL_SIZE                                                \
  (static_cast<unsigned long long>(2) * 1024 * 1024)
#endif

#ifndef EJIT_SRE_CODE_POOL_PTNO
#define EJIT_SRE_CODE_POOL_PTNO 8
#endif

// Memory module id passed to SRE_MemDbgAlloc. Not architecturally significant
// for the pool; overridable if a deployment needs a specific id.
#ifndef EJIT_SRE_CODE_POOL_MID
#define EJIT_SRE_CODE_POOL_MID 0
#endif

namespace {
constexpr unsigned long long kSrePoolSize = EJIT_SRE_CODE_POOL_SIZE;
constexpr unsigned char kSrePtNo =
    static_cast<unsigned char>(EJIT_SRE_CODE_POOL_PTNO);
constexpr unsigned kSreMid = static_cast<unsigned>(EJIT_SRE_CODE_POOL_MID);
constexpr size_t k2MiB = static_cast<size_t>(2) * 1024 * 1024;
constexpr size_t k4KiB = static_cast<size_t>(4) * 1024;
} // namespace

// Hard-lock the platform split/seal granularities against the shared-pool
// contract copies: splitting by one size and sealing by another would leave
// RWX windows or fail seals — memory safety, not just a perf issue.
static_assert(k2MiB == llvm::ejit::kEJitSharedSplitGranule,
              "SRE pool split granule (2 MiB) must equal "
              "kEJitSharedSplitGranule (EJitSharedTaskPoolState.h).");
static_assert(k4KiB == llvm::ejit::kEJitSharedSealPage,
              "SRE pool seal page (4 KiB) must equal "
              "kEJitSharedSealPage (EJitSharedTaskPoolState.h).");

//===----------------------------------------------------------------------===//
// Platform primitives (declaration only — defined by the platform/business)
//
// enable_ex / split_2m_to_4k are renamed via asm labels so the generic
// identifiers (ejit_sre_enable_ex / ejit_sre_split_2m_to_4k) are used in C++
// while the linker sees the real platform symbol names. These are intentionally
// NOT given weak fallbacks: in static-pack / partial-link / platform-SDK
// scenarios a weak local definition could shadow or collide with the real
// symbol or bind incorrectly. EmbeddedJIT only declares and calls them; the
// platform must provide the strong definitions.
//===----------------------------------------------------------------------===//
extern "C" unsigned
ejit_sre_enable_ex(unsigned startLevel,
                   unsigned long long va) __asm__("enable_ex");

// Split a 2MiB-aligned [va, va + size) window into 4KiB mappings. Must be
// called before any per-page enable_ex on that window. Returns 0 on success.
extern "C" unsigned
ejit_sre_split_2m_to_4k(unsigned long long va,
                        unsigned long long size) __asm__("split_2m_to_4k");

// Make a 4KiB page writable (RX -> RW) so JIT code can be written into a fixed
// region placed in the executable code segment. Symmetric to enable_ex (which
// makes a page executable, RW -> RX). Returns 0 on success. Only declared when
// the fixed code pool is placed in the code segment
// (EJIT_FIXED_CODE_POOL); the platform MUST supply the strong
// definition - there is intentionally no weak no-op fallback, so a missing
// enable_rw surfaces as a hard link error rather than silent non-writability.
// Platform signature: unsigned enable_rw(unsigned level, unsigned long long va)
// (level mirrors enable_ex's startLevel; the EnableRw lambda below passes 1).
//
// W^X contract: enable_rw MUST clear execute permission (set UXN/PXN on
// AArch64) as well as set write permission (AP bit), so the page transitions
// RX -> RW (writable, NOT executable) during the write window. Flipping only
// the AP/write bit would leave the page RWX, violating W^X. enable_ex is the
// symmetric inverse (RW -> RX: clear write, set executable + I-cache sync).
#if defined(EJIT_FIXED_CODE_POOL)
extern "C" unsigned
ejit_sre_enable_rw(unsigned level, unsigned long long va) __asm__("enable_rw");
#endif

extern "C" void *SRE_MemDbgAlloc(unsigned int mid, unsigned char ptNo,
                                 unsigned long size, const char *func,
                                 unsigned int line);

// Fixed code-pool region bounds, defined by the linker script (a .text.ejit
// output section bracketed by these two symbols). When EJIT_FIXED_CODE_POOL is
// on, makeSreCodePoolManager() prefers this fixed region over dynamic
// SRE_MemDbgAlloc so JIT code lands at a stable, predictable address (enabling
// direct bl/adrp reach to AOT code). Declared weak on the host (absent -> null
// -> dynamic fallback, so host tests link without a linker script) and strong
// under EJIT_FREESTANDING, exactly like __start_ejit_bitcode in EJit.cpp: a
// bare-metal link MUST define them via the linker script, and a missing
// definition is a hard link error rather than a silent no-op.
#if defined(EJIT_FIXED_CODE_POOL)
extern "C" {
#ifndef EJIT_FREESTANDING
extern const unsigned char __ejit_code_start[] __attribute__((weak));
extern const unsigned char __ejit_code_end[] __attribute__((weak));
#else
extern const unsigned char __ejit_code_start[];
extern const unsigned char __ejit_code_end[];
#endif
}
#endif

namespace {
// Same real shared mapping as the taskpool, without a dynamic constructor that
// another core's init-array could repeat. Ready publishes every identity field.
// State 0=unused/default OFF, 1=legacy near factory already constructed,
// 2=shared domain ready, 3=initializing. State/cursor never reset.
struct alignas(64) SharedFixedCodePoolDomain {
  llvm::ejit::EJitAtomicU32 state;
  llvm::ejit::EJitAtomicU64 signature;
  llvm::ejit::EJitAtomicUPtr reservationStart, reservationEnd, usableBase;
  llvm::ejit::EJitAtomicU64 usableBytes, poolSize, usedBytes;
};
static_assert(std::is_trivially_default_constructible<
                  SharedFixedCodePoolDomain>::value,
              "shared fixed domain must have no init-array constructor");
static_assert(std::is_standard_layout<SharedFixedCodePoolDomain>::value,
              "shared fixed domain must have fixed scalar layout");
EJIT_SHARED_SECTION SharedFixedCodePoolDomain SharedFixedDomain;

bool linkerFixedDomain(uintptr_t &Start, uintptr_t &End, uintptr_t &Base,
                       uint64_t &Bytes, uint64_t &PoolSize) {
#ifdef EJIT_FIXED_CODE_POOL
  Start = reinterpret_cast<uintptr_t>(__ejit_code_start);
  End = reinterpret_cast<uintptr_t>(__ejit_code_end);
  if (!Start || (Start & (k4KiB - 1)) || End <= Start ||
      (End & (k4KiB - 1)) || Start > UINTPTR_MAX - (k2MiB - 1))
    return false;
  Base = (Start + k2MiB - 1) & ~(static_cast<uintptr_t>(k2MiB) - 1);
  const uintptr_t AlignedEnd = End & ~(static_cast<uintptr_t>(k2MiB) - 1);
  if (AlignedEnd <= Base || kSrePoolSize == 0 ||
      kSrePoolSize > std::numeric_limits<size_t>::max() - (k2MiB - 1))
    return false;
  PoolSize = (kSrePoolSize + k2MiB - 1) & ~(uint64_t(k2MiB) - 1);
  Bytes = AlignedEnd - Base;
  return PoolSize != 0 && PoolSize <= Bytes;
#else
  (void)Start; (void)End; (void)Base; (void)Bytes; (void)PoolSize;
  return false;
#endif
}

bool sharedFixedDomainMatchesLinker() {
#if defined(EJIT_FIXED_CODE_POOL) && defined(EJIT_CODE_POOL_4K_SEAL) && \
    defined(EJIT_SRE_ENABLE_EX)
  uintptr_t Start = 0, End = 0, Base = 0;
  uint64_t Bytes = 0, PoolSize = 0;
  return SharedFixedDomain.state.loadAcquire() == 2 &&
         SharedFixedDomain.signature.loadRelaxed() == 0x454a4658444f4d31ULL &&
         linkerFixedDomain(Start, End, Base, Bytes, PoolSize) &&
         SharedFixedDomain.reservationStart.loadRelaxed() == Start &&
         SharedFixedDomain.reservationEnd.loadRelaxed() == End &&
         SharedFixedDomain.usableBase.loadRelaxed() == Base &&
         SharedFixedDomain.usableBytes.loadRelaxed() == Bytes &&
         SharedFixedDomain.poolSize.loadRelaxed() == PoolSize &&
         SharedFixedDomain.usedBytes.loadAcquire() <= Bytes;
#else
  return false;
#endif
}

void *claimSharedFixedPool(size_t Size) {
  if (!sharedFixedDomainMatchesLinker() ||
      Size != SharedFixedDomain.poolSize.loadRelaxed())
    return nullptr;
  const uint64_t Capacity = SharedFixedDomain.usableBytes.loadRelaxed();
  uint64_t Used = SharedFixedDomain.usedBytes.loadAcquire();
  for (;;) {
    if (Used > Capacity || Size > Capacity - Used) {
      EJIT_DIAG("shared fixed domain exhausted used=%llu capacity=%llu "
                "poolSize=%zu; no reuse or dynamic fallback",
                static_cast<unsigned long long>(Used),
                static_cast<unsigned long long>(Capacity), Size);
      return nullptr;
    }
    if (SharedFixedDomain.usedBytes.compareExchange(Used, Used + Size))
      return reinterpret_cast<void *>(
          SharedFixedDomain.usableBase.loadRelaxed() + Used);
  }
}

/// Make newly-written JIT code in [Va, Va + Size) observable to instruction
/// fetch. On AArch64 the I-cache does not snoop D-cache writes, so code
/// written into a RW page is not executable until the D-cache is cleaned and
/// the I-cache invalidated for that range, then the core context-syncs.
/// enable_ex makes the page executable but does NOT perform this cache sync,
/// so the seal path does it explicitly before making the page executable.
///
/// Per-core: every core that executes JIT code seals in its own translation
/// context, so each executing core syncs its own I-cache before first
/// execution.
///
/// Inline asm rather than __builtin___clear_cache / llvm::sys::Memory::
/// InvalidateInstructionCache: both resolve to the external __clear_cache
/// symbol, which the freestanding SRE link does not provide.
void syncCodeCaches(uintptr_t Va, size_t Size) {
  if (Size == 0)
    return;
#ifdef __aarch64__
  uint64_t Ctr;
  __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(Ctr));
  size_t DLine = static_cast<size_t>(4) << ((Ctr >> 16) & 0xF);
  size_t ILine = static_cast<size_t>(4) << (Ctr & 0xF);

  uintptr_t End = Va + Size;
  uintptr_t P = Va & ~static_cast<uintptr_t>(DLine - 1);
  for (; P < End; P += DLine)
    __asm__ __volatile__("dc cvau, %0" ::"r"(P) : "memory");
  __asm__ __volatile__("dsb ish" ::: "memory");

  P = Va & ~static_cast<uintptr_t>(ILine - 1);
  for (; P < End; P += ILine)
    __asm__ __volatile__("ic ivau, %0" ::"r"(P) : "memory");
  __asm__ __volatile__("dsb ish" ::: "memory");
  __asm__ __volatile__("isb" ::: "memory");
#else
  // Non-AArch64 (host) fallback: compiler-rt/libgcc is available here, so the
  // builtin's __clear_cache resolves. The SRE target is always AArch64.
  __builtin___clear_cache(reinterpret_cast<char *>(Va),
                          reinterpret_cast<char *>(Va + Size));
#endif
}

/// Seal one code range on the calling core: make the just-written JIT code
/// observable to instruction fetch (syncCodeCaches), then make the page
/// executable (enable_ex). Returns enable_ex's rc (0 = success).
unsigned sealAndSyncCache(uintptr_t Va, size_t Size) {
  syncCodeCaches(Va, Size);
  return ejit_sre_enable_ex(1, static_cast<unsigned long long>(Va));
}
} // namespace

llvm::Error llvm::ejit::enableSreSharedFixedCodePoolDomain() {
#ifdef EJIT_FIXED_CODE_POOL
  return enableSreSharedFixedCodePoolDomain(
      reinterpret_cast<uintptr_t>(__ejit_code_start),
      reinterpret_cast<uintptr_t>(__ejit_code_end));
#else
  return make_error<StringError>("shared fixed domain requires fixed code pool",
                                inconvertibleErrorCode());
#endif
}

llvm::Error llvm::ejit::enableSreSharedFixedCodePoolDomain(
    uintptr_t ReservationStart, uintptr_t ReservationEnd) {
#if !defined(EJIT_FIXED_CODE_POOL) || !defined(EJIT_CODE_POOL_4K_SEAL) || \
    !defined(EJIT_SRE_ENABLE_EX)
  (void)ReservationStart;
  (void)ReservationEnd;
  return make_error<StringError>(
      "shared fixed domain requires fixed pool, 4K sealing and real execute "
      "permissions", inconvertibleErrorCode());
#else
  uintptr_t Start = 0, End = 0, Base = 0;
  uint64_t Bytes = 0, PoolSize = 0;
  if (!linkerFixedDomain(Start, End, Base, Bytes, PoolSize) ||
      ReservationStart != Start || ReservationEnd != End)
    return make_error<StringError>(
        "shared fixed domain reservation identity/alignment/bounds invalid",
        inconvertibleErrorCode());
  uint32_t Expected = 0;
  if (!SharedFixedDomain.state.compareExchange(Expected, 3)) {
    if (Expected == 2 && sharedFixedDomainMatchesLinker())
      return Error::success(); // SAME identity, no cursor reset.
    return make_error<StringError>(
        "shared fixed domain must be enabled before any legacy near manager; "
        "cannot change or reset a live allocation domain",
        inconvertibleErrorCode());
  }
  SharedFixedDomain.reservationStart.storeRelaxed(Start);
  SharedFixedDomain.reservationEnd.storeRelaxed(End);
  SharedFixedDomain.usableBase.storeRelaxed(Base);
  SharedFixedDomain.usableBytes.storeRelaxed(Bytes);
  SharedFixedDomain.poolSize.storeRelaxed(PoolSize);
  SharedFixedDomain.usedBytes.storeRelaxed(0);
  SharedFixedDomain.signature.storeRelaxed(0x454a4658444f4d31ULL);
  SharedFixedDomain.state.storeRelease(2);
  EJIT_DIAG("shared fixed domain enabled: reservation=[0x%llx,0x%llx) "
            "usable=[0x%llx,+%llu) poolSize=%llu; no reset/reuse",
            static_cast<unsigned long long>(Start),
            static_cast<unsigned long long>(End),
            static_cast<unsigned long long>(Base),
            static_cast<unsigned long long>(Bytes),
            static_cast<unsigned long long>(PoolSize));
  return Error::success();
#endif
}

bool llvm::ejit::sreSharedFixedCodePoolDomainActive() {
  return sharedFixedDomainMatchesLinker();
}

llvm::ejit::EJitSreFixedCodePoolDomainInfo
llvm::ejit::getSreFixedCodePoolDomainInfo() {
  EJitSreFixedCodePoolDomainInfo Info;
  Info.enabled = sharedFixedDomainMatchesLinker();
  if (SharedFixedDomain.state.loadAcquire() != 2)
    return Info;
  Info.reservationStart = SharedFixedDomain.reservationStart.loadRelaxed();
  Info.reservationEnd = SharedFixedDomain.reservationEnd.loadRelaxed();
  Info.usableBase = SharedFixedDomain.usableBase.loadRelaxed();
  Info.usableBytes = SharedFixedDomain.usableBytes.loadRelaxed();
  Info.usedBytes = SharedFixedDomain.usedBytes.loadAcquire();
  Info.poolSize = SharedFixedDomain.poolSize.loadRelaxed();
  return Info;
}

std::unique_ptr<llvm::ejit::EJitCodePoolManager>
llvm::ejit::makeSreCodePoolManager(EJitCodePoolPlacement Placement) {
  EJitCodePoolManager::Options Opts;
  Opts.kind = Placement == EJitCodePoolPlacement::NearFixed
                  ? EJitCodePoolKind::Near
                  : EJitCodePoolKind::Far;
  Opts.poolSize = static_cast<size_t>(kSrePoolSize);
  Opts.poolAlign = k2MiB; // large-page / split granularity
  Opts.minCodeAlign = 64;
#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
  if (Placement == EJitCodePoolPlacement::NearFixed) {
    Opts.minCodeAlign = 16;
    Opts.batchedPageSeal = true;
  }
#endif
  EJIT_DIAG_VERBOSE(
      "makeSreCodePoolManager: kind=%s poolSize=%llu poolAlign=%zu batched=%u",
      Placement == EJitCodePoolPlacement::NearFixed ? "near" : "far",
      kSrePoolSize, k2MiB, static_cast<unsigned>(Opts.batchedPageSeal));
#ifdef EJIT_CODE_POOL_4K_SEAL
  // Adapt to the platform's 4K execute-permission interface: the 2MiB pool is
  // split into 4K mappings at creation and sealed one 4KiB page at a time.
  Opts.fourKSeal = true;
  Opts.sealPageSize = k4KiB;
#endif

#ifdef EJIT_FIXED_CODE_POOL
  // The linker script reserves [.text.ejit] = [__ejit_code_start, __ejit_code_end)
  // with only page (4KiB) alignment. Align the start UP to the 2MiB large-page
  // granularity here: split_2m_to_4k / enable_ex require 2MiB-aligned bases, so
  // the runtime (not the linker script) owns the 2MiB alignment. The bytes
  // [__ejit_code_start, AlignedBase) are wasted alignment slack (at most ~2MiB),
  // so size the region with that headroom in mind (16M -> ~14M usable). Engage
  // fixed mode only if the aligned region can hold at least one pool; otherwise
  // fall back to SRE_MemDbgAlloc (a too-small fixed region would exhaust on
  // every compile). A fixed region gives a stable JIT address range and, when
  // placed within +-128MiB of .text, lets codegen use direct bl/adrp.
  if (Placement == EJitCodePoolPlacement::NearFixed) {
    uint32_t Unused = 0;
    // The latch lives in shared memory: another core's factory cannot hide an
    // earlier legacy manager (even if it has not allocated its first pool).
    SharedFixedDomain.state.compareExchange(Unused, 1);
    uintptr_t FBase = reinterpret_cast<uintptr_t>(__ejit_code_start);
    uintptr_t FEnd = reinterpret_cast<uintptr_t>(__ejit_code_end);
    uintptr_t AlignedBase = (FBase + (static_cast<uintptr_t>(k2MiB) - 1)) &
                            ~(static_cast<uintptr_t>(k2MiB) - 1);
    if (FEnd > AlignedBase && (FEnd - AlignedBase) >= kSrePoolSize) {
      Opts.fixedBase = AlignedBase;
      Opts.fixedSize = FEnd - AlignedBase;
      EJIT_DIAG(
          "makeSreCodePoolManager: FIXED region sym=[0x%llx,0x%llx) "
          "alignedBase=0x%llx usable=%llu (~%llu pools of %lluB)",
          static_cast<unsigned long long>(FBase),
          static_cast<unsigned long long>(FEnd),
          static_cast<unsigned long long>(AlignedBase),
          static_cast<unsigned long long>(FEnd - AlignedBase),
          static_cast<unsigned long long>((FEnd - AlignedBase) / kSrePoolSize),
          static_cast<unsigned long long>(kSrePoolSize));
    } else {
      EJIT_DIAG(
          "makeSreCodePoolManager: fixed region absent or too small after "
          "2MiB alignment (sym=[0x%llx,0x%llx) alignedBase=0x%llx "
          "usable=%llu < poolSize=%llu), falling back to SRE_MemDbgAlloc "
          "ptNo=%u",
          static_cast<unsigned long long>(FBase),
          static_cast<unsigned long long>(FEnd),
          static_cast<unsigned long long>(AlignedBase),
          static_cast<unsigned long long>(
              FEnd > AlignedBase ? FEnd - AlignedBase : 0),
          static_cast<unsigned long long>(kSrePoolSize),
          static_cast<unsigned>(kSrePtNo));
    }
  }
#endif

#ifdef EJIT_FIXED_CODE_POOL
  // Code-segment placement: the fixed region is RX by default, so each slab
  // must be enable_rw'd (RX -> RW) before writing. needsEnableRw makes
  // EJitCodePoolMemoryManager::allocate call enableRwRange before memset. Only
  // meaningful when the fixed region is actually engaged (fixedSize > 0).
  if (Opts.fixedSize > 0) {
    Opts.needsEnableRw = true;
    EJIT_DIAG(
        "makeSreCodePoolManager: code-segment placement -> needsEnableRw=1");
  }
  if (Placement == EJitCodePoolPlacement::NearFixed &&
      SharedFixedDomain.state.loadAcquire() != 1) {
    if (sharedFixedDomainMatchesLinker()) {
      Opts.fixedBase = SharedFixedDomain.usableBase.loadRelaxed();
      Opts.fixedSize = SharedFixedDomain.usableBytes.loadRelaxed();
      Opts.poolSize = SharedFixedDomain.poolSize.loadRelaxed();
      Opts.needsEnableRw = true;
      Opts.sharedFixedAlloc = &claimSharedFixedPool;
    } else {
      // An in-progress/foreign domain cannot fall back to a private cursor or
      // dynamic heap. A rejecting fixed callback makes the first allocation
      // fail closed without handing the engine a null memory manager.
      Opts.fixedBase = reinterpret_cast<uintptr_t>(__ejit_code_start);
      const uintptr_t End = reinterpret_cast<uintptr_t>(__ejit_code_end);
      Opts.fixedSize = End > Opts.fixedBase ? End - Opts.fixedBase : 1;
      Opts.sharedFixedAlloc = [](size_t) -> void * { return nullptr; };
      Opts.needsEnableRw = true;
    }
  }
#endif

  auto RawAlloc = [](size_t Bytes) -> void * {
    return SRE_MemDbgAlloc(kSreMid, kSrePtNo, static_cast<unsigned long>(Bytes),
                           __func__, __LINE__);
  };

  auto Seal = [](void *Va) -> unsigned {
#ifdef EJIT_SRE_ENABLE_EX
  // In 4K seal mode Va is a single 4KiB page; in legacy mode it is the 2MiB
  // pool base. sealAndSyncCache syncs caches for the written range then makes
  // the page executable (enable_ex does not sync caches).
#ifdef EJIT_CODE_POOL_4K_SEAL
    return sealAndSyncCache(reinterpret_cast<uintptr_t>(Va), k4KiB);
#else
    return sealAndSyncCache(reinterpret_cast<uintptr_t>(Va),
                            static_cast<size_t>(kSrePoolSize));
#endif
#else
    // Code-pool routing without permission flips (bring-up / measurement).
    (void)Va;
    return 0;
#endif
  };

  auto Split = [](void *Base, size_t Size) -> unsigned {
#ifdef EJIT_CODE_POOL_4K_SEAL
    return ejit_sre_split_2m_to_4k(reinterpret_cast<unsigned long long>(Base),
                                   static_cast<unsigned long long>(Size));
#else
    (void)Base;
    (void)Size;
    return 0;
#endif
  };

  auto EnableRw = [](void *Va) -> unsigned {
#ifdef EJIT_FIXED_CODE_POOL
    // Make one 4KiB page writable (RX -> RW) so JITLink can write code into the
    // code-segment fixed region. enable_rw must set write permission (AP bit)
    // AND clear execute permission (UXN/PXN) + TLB flush, so the page is
    // RW-but-not-X (true W^X) during the write window - flipping only the AP
    // bit would leave it RWX. No I-cache sync is needed here: we are about to
    // WRITE, not execute (enable_ex / sealAndSyncCache syncs caches later, at
    // RW -> RX). Per-core: only the compiling core calls this; peer cores only
    // enable_ex to execute.
    return ejit_sre_enable_rw(
        /*level=*/1u,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(Va)));
#else
    (void)Va;
    return 0;
#endif
  };

  return std::make_unique<EJitCodePoolManager>(Opts, RawAlloc, Seal, Split,
                                               EnableRw);
}

bool llvm::ejit::prepareSreCodeForCurrentCore(const void *FnPtr) {
#if !defined(EJIT_SRE_ENABLE_EX) || defined(EJIT_CODE_POOL_4K_SEAL)
  EJIT_DIAG("prepareSreCode: unsupported config (FnPtr=%p), clean fallback",
            FnPtr);
  (void)FnPtr;
  return false;
#else
  if (!FnPtr) {
    EJIT_DIAG("prepareSreCode: null FnPtr, reject");
    return false;
  }
  const auto Address = reinterpret_cast<uintptr_t>(FnPtr);
  const auto PoolBase = Address & ~(static_cast<uintptr_t>(k2MiB) - 1);
  unsigned Rc = sealAndSyncCache(PoolBase, static_cast<size_t>(kSrePoolSize));
  if (Rc != 0) {
    EJIT_DIAG("prepareSreCode FAIL: enable_ex poolBase=0x%llx rc=%u",
              static_cast<unsigned long long>(PoolBase), Rc);
    return false;
  }
  return true;
#endif
}

bool llvm::ejit::ejitSreSplitPoolForCurrentCore(uintptr_t PoolBase,
                                                uint64_t PoolSize) {
#if defined(EJIT_SRE_ENABLE_EX) && defined(EJIT_CODE_POOL_4K_SEAL)
  if (PoolBase == 0 || PoolSize == 0) {
    EJIT_DIAG("splitPoolForCurrentCore reject: poolBase=0x%llx size=%llu",
              static_cast<unsigned long long>(PoolBase),
              static_cast<unsigned long long>(PoolSize));
    return false;
  }
  // Per-core: this splits the 2MiB large page into 4K mappings in the calling
  // core's stage-1 translation only. enable_ex per page follows.
  unsigned Rc =
      ejit_sre_split_2m_to_4k(static_cast<unsigned long long>(PoolBase),
                              static_cast<unsigned long long>(PoolSize));
  if (Rc != 0) {
    EJIT_DIAG("splitPoolForCurrentCore FAIL: split_2m_to_4k poolBase=0x%llx "
              "size=%llu rc=%u",
              static_cast<unsigned long long>(PoolBase),
              static_cast<unsigned long long>(PoolSize), Rc);
    return false;
  }
  return true;
#else
  EJIT_DIAG("splitPoolForCurrentCore unsupported config: poolBase=0x%llx",
            static_cast<unsigned long long>(PoolBase));
  (void)PoolBase;
  (void)PoolSize;
  return false;
#endif
}

bool llvm::ejit::ejitSreSealPageForCurrentCore(uintptr_t PageVA) {
#ifdef EJIT_SRE_ENABLE_EX
  if (PageVA == 0) {
    EJIT_DIAG("sealPageForCurrentCore reject: null PageVA");
    return false;
  }
  // Per-core: flips the 4KiB page containing PageVA to RX in the calling core's
  // translation context AND syncs its I-cache for that page. enable_ex does NOT
  // do the cache sync, so it is done here (see sealAndSyncCache). Every core
  // that executes shared JIT code seals its own translation context here before
  // first execution.
  unsigned Rc = sealAndSyncCache(PageVA, k4KiB);
  if (Rc != 0) {
    EJIT_DIAG("sealPageForCurrentCore FAIL: enable_ex pageVA=0x%llx rc=%u",
              static_cast<unsigned long long>(PageVA), Rc);
    return false;
  }
  return true;
#else
  EJIT_DIAG("sealPageForCurrentCore unsupported config: pageVA=0x%llx",
            static_cast<unsigned long long>(PageVA));
  (void)PageVA;
  return false;
#endif
}

bool llvm::ejit::ejitSreEnableRwPageForCurrentCore(uintptr_t PageVA) {
#if defined(EJIT_FIXED_CODE_POOL) && defined(EJIT_CODE_POOL_4K_SEAL)
  if (PageVA == 0) {
    EJIT_DIAG("enableRwPageForCurrentCore reject: null PageVA");
    return false;
  }
  // Per-core: flip the 4KiB page containing PageVA to RW (writable, NOT
  // executable) in the CALLING core's stage-1 translation. This is used for a
  // JIT function's runtime-writable data pages (e.g. Tier-1 __profc_ counters):
  // the fixed .text.ejit code segment is RX on every core at load, and only the
  // owner previously made these pages writable, so a non-owner core must
  // enable_rw them here before executing code that writes them. No I-cache sync
  // is needed (this enables a data write, not code execution). The caller only
  // passes pages that are page-disjoint from executable code, so this never
  // makes a code page writable (no RWX). enable_rw must also clear execute
  // permission (UXN/PXN) so the page is RW-but-not-X (W^X) — see the enable_rw
  // contract note above.
  unsigned Rc = ejit_sre_enable_rw(
      /*level=*/1u, static_cast<unsigned long long>(PageVA));
  if (Rc != 0) {
    EJIT_DIAG("enableRwPageForCurrentCore FAIL: enable_rw pageVA=0x%llx rc=%u",
              static_cast<unsigned long long>(PageVA), Rc);
    return false;
  }
  return true;
#else
  EJIT_DIAG("enableRwPageForCurrentCore unsupported config: pageVA=0x%llx",
            static_cast<unsigned long long>(PageVA));
  (void)PageVA;
  return false;
#endif
}

#endif // EJIT_SRE_CODE_POOL
