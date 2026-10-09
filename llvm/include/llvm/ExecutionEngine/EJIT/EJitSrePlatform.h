//===-- EJitSrePlatform.h - SRE platform adapter for the code pool --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Thin adapter that wires EJitCodePoolManager to the real SRE platform
//  primitives (SRE_MemDbgAlloc for raw memory, enable_ex for sealing). This
//  header is only meaningful when EJIT_SRE_CODE_POOL is defined; it keeps the
//  SRE symbol declarations out of generic LLVM translation units.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSREPLATFORM_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSREPLATFORM_H

#ifdef EJIT_SRE_CODE_POOL

#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedData.h"
#include <memory>

namespace llvm {
namespace ejit {

enum class EJitCodePoolPlacement { NearFixed, FarDynamic };

/// Construct an EJitCodePoolManager wired to the SRE platform: raw memory from
/// SRE_MemDbgAlloc (partition EJIT_SRE_CODE_POOL_PTNO) and sealing via
/// enable_ex. Platform primitives have no weak/no-op definitions: the final
/// link must supply their actual implementations.
std::unique_ptr<EJitCodePoolManager> makeSreCodePoolManager(
    EJitCodePoolPlacement Placement = EJitCodePoolPlacement::NearFixed);

/// Explicit opt-in, BEFORE any near manager exists, to one shared allocator
/// over the actual linker reservation. Default OFF. All ordinary/common near
/// managers then claim disjoint whole pools through the same monotonic cursor.
/// No disable/reset/recycle operation exists: shutdown, failure and destruction
/// cannot make an old executable window available to another manager.
Error enableSreSharedFixedCodePoolDomain();
/// Explicit-bounds form for product validation/tests. Bounds MUST exactly
/// identify the actual __ejit_code_start/end reservation, never guessed VAs.
Error enableSreSharedFixedCodePoolDomain(uintptr_t ReservationStart,
                                       uintptr_t ReservationEnd);
bool sreSharedFixedCodePoolDomainActive();
struct EJitSreFixedCodePoolDomainInfo {
  bool enabled = false;
  uintptr_t reservationStart = 0;
  uintptr_t reservationEnd = 0;
  uintptr_t usableBase = 0;
  uint64_t usableBytes = 0;
  uint64_t usedBytes = 0;
  uint64_t poolSize = 0;
};
EJitSreFixedCodePoolDomainInfo getSreFixedCodePoolDomainInfo();

/// Claim independent, stable DataOnly storage from the existing shared fixed
/// reservation. The first block is claimed on demand; disabled/unused small
/// tables consume no bytes. Each resource is page isolated. A resource may be
/// at most 4MiB, all historical claims at most 16MiB and 256 records. Remaining
/// fixed-domain space is an additional hard bound. No fallback/reset/recycle.
/// Does not split, prepare permissions, zero or publish the storage: the caller
/// must prepare this exact descriptor on its actual core BEFORE writing.
Error allocateSreSmallTableStorage(uint64_t PayloadBytes, uint64_t Generation,
                                   EJitSreDataAllocation &Out);
bool validateSreSmallTableStorage(const EJitSreDataAllocation &Allocation);
/// Release metadata authority, not backing memory. Failed preparation creates
/// a permanent hole, separately accounted; a duplicate release is refused.
bool releaseSreSmallTableStorage(const EJitSreDataAllocation &Allocation,
                                bool Failed = false);
EJitSreDataAllocationStats getSreSmallTableStorageStats();

/// Install execute permission for the legacy 2MiB code pool containing
/// \p FnPtr in the calling core's translation context. This is intentionally a
/// per-core operation: another core sealing the same VA does not imply that
/// this core's stage-1 mapping is executable.
///
/// Returns false when execute permission is unavailable or when 4K page-seal
/// mode is selected (a bare function pointer does not carry the full code
/// extent needed to seal every covered 4K page).
bool prepareSreCodeForCurrentCore(const void *FnPtr);

/// 4K page-seal mode, per-core: split the 2MiB-aligned pool
/// [PoolBase, PoolBase + PoolSize) into 4KiB mappings in the CALLING core's
/// translation context (split_2m_to_4k). A core must do this once per pool
/// before it may seal any 4K page inside it. Returns true on success. A no-op
/// returning false when 4K seal mode / the platform seal symbol is not built.
bool ejitSreSplitPoolForCurrentCore(uintptr_t PoolBase, uint64_t PoolSize);

/// 4K page-seal mode, per-core: seal one 4KiB page at \p PageVA to RX in the
/// CALLING core's translation context (enable_ex(1, PageVA)). Returns true on
/// success. A no-op returning false when the platform seal symbol is not built.
bool ejitSreSealPageForCurrentCore(uintptr_t PageVA);

/// 4K page-seal mode, per-core: make one 4KiB page at \p PageVA writable
/// (RX -> RW, enable_rw) in the CALLING core's translation context. Used for a
/// JIT function's runtime-writable data pages (e.g. the Tier-1 __profc_
/// counters) so a non-owner core running from the fixed RX .text.ejit code
/// segment can execute code that writes them without a write-permission abort.
/// The caller only passes pages that are page-disjoint from executable code, so
/// this never makes a code page writable (no RWX). Returns true on success. A
/// no-op returning false when the fixed code pool / enable_rw symbol is not
/// built.
bool ejitSreEnableRwPageForCurrentCore(uintptr_t PageVA);

} // namespace ejit
} // namespace llvm

#endif // EJIT_SRE_CODE_POOL
#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSREPLATFORM_H
