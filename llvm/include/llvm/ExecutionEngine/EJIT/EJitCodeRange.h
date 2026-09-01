//===-- EJitCodeRange.h - POD executable code range descriptor ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  EJitCompiledCodeInfo: the real executable extent of one finalized JIT
//  compilation, as recorded by the code pool at JITLink finalize time. It is a
//  plain POD of fixed-width scalars so it can be carried unchanged from the
//  owner's compile path into a shared cache slot and read back by a peer core
//  (which must seal every 4KiB page the code actually covers in its own
//  translation context).
//
//  All values come from the real JITLink/code-pool allocation+finalize
//  metadata — never estimated, hard-coded, or recovered by scanning machine
//  code. A zeroed value (codeSize == 0) means "no range metadata available",
//  which callers treat as a clean fallback (do not hand back a shared pointer).
//
//  This header pulls in no STL and no platform symbols; it is safe in
//  freestanding builds and shared between the code-pool layer and the shared
//  taskpool without coupling them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCODERANGE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCODERANGE_H

#include <cstdint>

namespace llvm {
namespace ejit {

enum class EJitCodePoolKind : uint32_t {
  Unknown = 0,
  Near = 1, // Hot fixed .text.ejit pool (kept for ABI compatibility).
  Far = 2,
  Cold = 3,
};

constexpr uint32_t kEJitMaxExtraCodeRanges = 1u;

struct EJitExecutableRange {
  uintptr_t codeStart = 0;
  uint64_t codeSize = 0;
  uintptr_t poolBase = 0;
  uint64_t poolSize = 0;
  uint32_t poolId = 0;
  EJitCodePoolKind poolKind = EJitCodePoolKind::Unknown;
};

/// Maximum number of runtime-writable ranges carried with one finalized
/// compilation. A finalized allocation normally has a single writable data
/// segment (the Tier-1 __profc_ counters); the small fixed bound leaves head
/// room for the rare graph with several writable segments while keeping the
/// descriptor POD and fixed-size. More writable segments than this is a clean
/// reject (never a silent truncation): the allocation is not published, so a
/// peer core never faults writing an un-prepared counter page. This value MUST
/// stay in lockstep with kEJitSharedMaxWritableRanges (the shared-slot bound).
constexpr uint32_t kEJitMaxWritableRanges = 4u;

/// One runtime-writable range of a finalized compilation: the extent a peer
/// core must make writable (enable_rw, RX -> RW) in its own translation context
/// before it may execute the JIT function, whose body writes here at runtime
/// (e.g. the Tier-1 __profc_ atomicrmw profile counters). Read-only data
/// (e.g. __profd_) is NOT listed: a peer reads it fine from an RX page. Every
/// field is a fixed-width scalar accessed by value (endian-safe on aarch64_be).
struct EJitWritableRange {
  /// Start of the runtime-writable extent (inside the same pool as the code).
  uintptr_t addr = 0;
  /// Size in bytes of that writable extent. 0 => unused entry.
  uint64_t size = 0;
};

/// Real executable extent of one finalized compilation. Every field is a
/// fixed-width scalar accessed by value (endian-safe on aarch64_be).
struct EJitCompiledCodeInfo {
  /// The resolved function entry pointer (may be anywhere inside the range).
  void *fnPtr = nullptr;
  /// Start of the fully-written, relocated, RX-sealed executable allocation
  /// that contains fnPtr.
  uintptr_t codeStart = 0;
  /// Size in bytes of that executable allocation. 0 => no range metadata.
  uint64_t codeSize = 0;
  /// Base of the 2MiB-aligned code pool that contains the allocation (the
  /// split_2m_to_4k granularity on the target).
  uintptr_t poolBase = 0;
  /// Usable size of that pool.
  uint64_t poolSize = 0;
  /// Stable identifier of the pool (its index in the manager). Lets callers
  /// key per-pool readiness without comparing raw bases when convenient.
  uint32_t poolId = 0;
  /// Number of valid entries in writableRanges (0..kEJitMaxWritableRanges). A
  /// value of 0 means the code has no runtime-writable data (e.g. a non-PGO or
  /// Tier-2 function): a peer core seals only the executable pages.
  uint32_t writableCount = 0;
  /// 1 when a peer core MUST make the writableRanges writable (enable_rw) in
  /// its own translation context before executing, i.e. the code lives in a
  /// fixed RX code-segment pool (EJitCodePoolManager Options::needsEnableRw). 0
  /// for a dynamic pool whose backing memory is already RW (SRE_MemDbgAlloc
  /// data mapping): the writableRanges are then diagnostic only and a peer
  /// executes without any enable_rw. Fixed-width so it rides through the shared
  /// cache.
  uint32_t requiresPeerEnableRw = 0;
  /// The runtime-writable extents a peer core must enable_rw before executing.
  /// Only the first writableCount entries are meaningful.
  EJitWritableRange writableRanges[kEJitMaxWritableRanges] = {};
  /// Placement class of the owning pool. Near is the fixed .text.ejit region;
  /// Far is the dynamic SRE_MemDbgAlloc region used by temporary Tier-1 code.
  EJitCodePoolKind poolKind = EJitCodePoolKind::Unknown;
  /// Additional executable extent belonging to the same compilation. MFS
  /// currently produces at most one cold RX extent in .text.ejit_cold.
  uint32_t extraCodeCount = 0;
  EJitExecutableRange extraCodeRanges[kEJitMaxExtraCodeRanges] = {};
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITCODERANGE_H
