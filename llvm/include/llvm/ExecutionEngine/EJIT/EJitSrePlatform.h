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
#include <memory>

namespace llvm {
namespace ejit {

/// Construct an EJitCodePoolManager wired to the SRE platform: raw memory from
/// SRE_MemDbgAlloc (partition EJIT_SRE_CODE_POOL_PTNO) and sealing via
/// enable_ex. On a host without real SRE symbols, weak fallbacks make this a
/// link-safe no-op-seal / aligned-host-alloc manager (see EJitSrePlatform.cpp).
std::unique_ptr<EJitCodePoolManager> makeSreCodePoolManager();

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

/// Atomically replace the one-instruction AOT pad at p Pad with `b Target`,
/// preserving W^X and synchronizing instruction caches. Returns false without
/// modifying the pad when either address is invalid/out of range or the pad
/// section cannot be made writable.
bool ejitSrePatchDirectBranch(void *Pad, const void *Target);

} // namespace ejit
} // namespace llvm

#endif // EJIT_SRE_CODE_POOL
#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSREPLATFORM_H
