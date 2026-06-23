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
  /// Reserved (must be 0). Keeps the struct's tail explicit.
  uint32_t reserved = 0;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITCODERANGE_H
