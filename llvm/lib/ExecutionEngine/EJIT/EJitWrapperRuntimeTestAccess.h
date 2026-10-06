//===-- EJitWrapperRuntimeTestAccess.h - Private real-runtime test access ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_WRAPPERRUNTIMETESTACCESS_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_WRAPPERRUNTIMETESTACCESS_H

#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <string>

namespace llvm {
namespace ejit {
class EJitCompileDriver;
class EJitSharedTaskPool;
class EJitSmallTableHost;

struct EJitWrapperCounterView {
  std::string pgoName;
  uintptr_t profcAddr = 0;
  uintptr_t profdAddr = 0;
};

/// Source-private inspection of genuine objects for lifecycle tests. This is
/// neither a public C ABI nor an alternative resolver/profile implementation.
struct EJitWrapperRuntimeTestAccess {
  using HostInstallationObserver = void (*)(void *, EJitSmallTableHost *);
  /// Observe the actual global publication before the worker handoff starts.
  /// No alternate policy/resolve/profile implementation is installed.
  static void setHostInstallationObserver(HostInstallationObserver Observer,
                                          void *Context);
  static EJitSharedTaskPool *pool();
  static bool counters(uint64_t CacheKey,
                       SmallVectorImpl<EJitWrapperCounterView> &Out);
  static bool driverCounters(const EJitCompileDriver &Driver, uint64_t CacheKey,
                             SmallVectorImpl<EJitWrapperCounterView> &Out);
};
} // namespace ejit
} // namespace llvm

#endif
