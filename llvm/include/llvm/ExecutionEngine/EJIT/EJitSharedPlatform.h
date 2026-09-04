//===-- EJitSharedPlatform.h - Cross-core shared taskpool platform seam ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Platform seam for the cross-core SHARED taskpool (EJIT_SRE_SHARED_TASKPOOL).
//
//  Two host-overridable / platform-injected primitives live here so the rest of
//  the shared taskpool never names a platform symbol directly:
//
//   * EJIT_SHARED_SECTION: the section attribute that places the single shared
//     state blob into inter-core shared memory. The build overrides it via
//     -D'EJIT_SHARED_SECTION=__attribute__((section(".xxxxx")))'. The default
//     is empty so host / unit-test builds use ordinary static storage (a single
//     process already shares one address space, which is exactly what the
//     deterministic multi-core simulation needs).
//
//   * EJitCoreId::current(): the identity of the core running the call. Real
//     cross-core builds (EJIT_SRE_SHARED_TASKPOOL_PLATFORM) bind it to a
//     declared-only platform symbol with NO weak fallback, so a missing
//     platform implementation is a link error rather than a silently-wrong
//     constant. Host builds use a per-thread settable value so a single test
//     process can simulate many cores deterministically, without any real
//     thread.
//
//  This header pulls in no STL and no <atomic>; it is safe in freestanding
//  builds.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDPLATFORM_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDPLATFORM_H

#include <cstdint>

//===----------------------------------------------------------------------===//
// Shared-section placement attribute (overridable by the build).
//===----------------------------------------------------------------------===//
#ifndef EJIT_SHARED_SECTION
#define EJIT_SHARED_SECTION
#endif

namespace llvm {
namespace ejit {

//===----------------------------------------------------------------------===//
// Shared-state ABI identity. Stored as separate fixed-width scalar fields and
// always compared by value (never byte-parsed), so the same definition is valid
// on little- and big-endian targets.
//===----------------------------------------------------------------------===//

/// Magic word stamped into the shared state header. Distinct, non-symmetric
/// value so a partially-zeroed or foreign blob is rejected.
constexpr uint32_t kEJitSharedAbiMagic = 0x456A5370u; // "EjSp"

/// ABI version of the shared state layout. Bump on any field/layout change.
/// v2: EJitCompileRequest carries a generation field and the flat dedup slot
/// stores the owning generation (0 = free) instead of a 1-bit flag.
/// v3: the shared state carries an owner registration fingerprint (peers
/// validate their funcIndex/dimType mapping against the owner before use).
/// v4: each cache slot carries a per-core executable-permission ready mask.
/// v5: each cache slot carries the real executable code range
/// (codeStart/codeSize/poolBase/poolSize/poolId) and the shared state gains a
/// per-core, per-pool 4K split-readiness table, so a non-owner core in 4K-seal
/// mode can split its pool once and seal exactly the pages the code covers.
/// v6: dump slots carry dynamic IR/ASM payload pointers instead of fixed text
/// buffers, and each cache bucket carries a monotonic publishSeq word used by
/// the optional EJIT_SRE_TASKPOOL_NO_RECLAIM seqlock reader.
/// v7: full IR/ASM payloads are worker-local again; shared dump state contains
/// only a bounded filter and latest-capture metadata. publishSeq is unchanged.
/// v8: the SwitchController line gains icacheDrainSeq, bumped by every
/// inline-cache drain so a resolve that raced the drain drops its fill rather
/// than refilling a cell the drain just cleared.
/// v9 adds the shared inline-cache gates and diagnostics needed by every core.
/// v10: each cache slot carries PGO fields (hitCount + profcAddr + profdAddr)
/// for online PGO hotspot detection and Tier-1 counter capture (§6/§7.1).
/// The shared counters struct gains tier1Compiles/tier2Compiles/
/// profileMergeFails. PGO behavior is opt-in (Config::enablePgo); the fields
/// exist in every build for a stable layout and are 0 when PGO is off.
/// Online-PGO enable/threshold control also lives in the shared blob so
/// every producer core observes the owner's configuration.
/// v11: each cache slot carries a bounded set of runtime-writable code ranges
/// (writableCount + writableRanges[]) — the pages the JIT body writes at
/// runtime, e.g. the Tier-1 __profc_ counters — plus a requiresPeerEnableRw
/// flag. A non-owner core running from a fixed RX .text.ejit code segment
/// (requiresPeerEnableRw=1) must enable_rw exactly these in its own translation
/// context before executing, or the first counter atomicrmw faults with a
/// write-permission abort. For a dynamic SRE_MemDbgAlloc pool
/// (requiresPeerEnableRw=0) the backing memory is already RW, so the ranges are
/// diagnostic only and a peer executes without enable_rw. The ranges are
/// page-disjoint from the executable extent, so a peer never makes a code page
/// writable (no RWX).
/// v12: staged PGO admission has a configurable fixed-capacity slot table, so
/// one or more functions may profile concurrently with independent progress;
/// the shared blob also records completed-function and deferred-miss counts.
/// v13: non-owner cores can post a may_const-ranking diagnostic request to the
/// owner worker and wait for its completion without sharing optimizer objects.
/// v14: EJitCompileRequest introduced the old inline bound-pointer payload.
/// v15: each cache slot records whether its published JIT pointer was resolved
/// by a later taskpool lookup, diagnosing compiled versions with no reuse.
/// v16: code ranges identify near/far placement and the shared code-pool
/// diagnostic mirror publishes aggregate plus placement-specific statistics.
/// v17: cache slots can remain Pending while compact code waits for an explicit
/// owner-worker batch publish request.
/// v18: each cache slot carries fnSize (the entry function's real size in
/// bytes, recovered from the finalized graph's defined symbols) so every core's
/// print_compiled can report per-function fn_size and waste overhead
/// (codeSize - fnSize) without owner-private lookups. Purely diagnostic; a peer
/// core never uses it for sealing or enable_rw. 0 means no symbol metadata was
/// recorded (print_compiled reports fn_size=0, overhead=codeSize).
/// v19: the owner-published pool mirror carries the 16 cell + public near-hot
/// pool details and pool ids are stable across separate managers.
/// v20: the old inline payload is replaced by a fixed table of borrowed
/// raw bound-pointer descriptors; no pointee bytes or ownership cross the
/// shared queue.
/// v21: requests and PGO admission slots carry exact lifecycle tokens and
/// identities; cold-profile progress/expiry and per-function VP gates are
/// shared across producers and the owner worker.
/// v22: a bounded exact-identity registry suppresses duplicate sampling while
/// Tier-2 code is linked but waiting for batch publication.
constexpr uint32_t kEJitSharedAbiVersion = 22u;

/// Sentinel "no core" id. Out of any plausible core-id range.
constexpr uint32_t kEJitInvalidCoreId = 0xFFFFFFFFu;

//===----------------------------------------------------------------------===//
// EJitCoreId: injectable current-core identity.
//===----------------------------------------------------------------------===//
class EJitCoreId {
public:
  /// Identity of the core executing this call.
  static uint32_t current();

#ifndef EJIT_SRE_SHARED_TASKPOOL_PLATFORM
  /// Host/unit-test only: set the simulated current-core id for the calling
  /// thread. Lets one process deterministically model many cores with no real
  /// thread. Unavailable in a real platform build (which uses the declared-only
  /// platform symbol).
  static void setCurrentForTest(uint32_t coreId);

  /// Host/unit-test only: restore the default simulated core id (0).
  static void resetForTest();
#endif
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDPLATFORM_H
