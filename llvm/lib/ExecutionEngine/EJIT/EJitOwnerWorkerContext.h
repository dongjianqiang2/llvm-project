//===-- EJitOwnerWorkerContext.h - Internal EJIT worker capability --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This is deliberately kept in the implementation directory: the capability
// is an internal C++ handoff token, not a product/SDK ABI or shared-memory
// identity. It is created on the real worker-entry stack and explicitly passed
// down that worker's call chain.
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITOWNERWORKERCONTEXT_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITOWNERWORKERCONTEXT_H

#include <cstdint>
#include <functional>

#include "llvm/Support/Error.h"

namespace llvm {
namespace ejit {

class EJitSharedTaskPool;
class EJitSmallTableHost;
struct EJitSharedTaskPoolTestAccess;

namespace detail {

/// Internal evidence that a call was reached through a particular
/// shared-task-pool worker entry. The private constructor prevents ordinary
/// callers from creating one; trusted internal code must not transfer or save
/// its address. It is valid only while that worker loop is active and while
/// both the pool epoch and shared generation still match.
class OwnerWorkerContext final {
  friend class ::llvm::ejit::EJitSharedTaskPool;
  friend struct ::llvm::ejit::EJitSharedTaskPoolTestAccess;

  OwnerWorkerContext(const EJitSharedTaskPool *Pool, uint64_t PoolEpoch,
                     uint32_t StateGeneration)
      : pool_(Pool), poolEpoch_(PoolEpoch), stateGeneration_(StateGeneration) {}

public:
  /// A capability proves only the explicit worker-entry call chain. Keeping
  /// this header implementation-private and never exposing callbacks that
  /// hand out Worker prevents normal product callers from obtaining it.
  bool validFor(const EJitSharedTaskPool &Pool) const;
  /// Cleanup-only check: remains true while this exact worker is unwinding
  /// through Stopping, so a final pin cannot destroy/join its own worker.
  bool activeFor(const EJitSharedTaskPool &Pool) const;

  OwnerWorkerContext(const OwnerWorkerContext &) = delete;
  OwnerWorkerContext &operator=(const OwnerWorkerContext &) = delete;
  OwnerWorkerContext(OwnerWorkerContext &&) = delete;
  OwnerWorkerContext &operator=(OwnerWorkerContext &&) = delete;

private:
  const EJitSharedTaskPool *pool_;
  uint64_t poolEpoch_;
  uint32_t stateGeneration_;
};

} // namespace detail

using SmallTableOwnerWorkerJob =
    std::function<Error(const detail::OwnerWorkerContext &)>;

/// Internal owner handoff entry points. The context-aware callback declarations
/// live here rather than an installed header so product callers cannot obtain
/// a worker context from a public queued callback.
Error runSmallTableOwnerRequest(uint32_t FuncIndex, EJitSmallTableHost *Host,
                                SmallTableOwnerWorkerJob Job,
                                bool InitialHandoff = true);
Error runSmallTableOwnerRequest(const detail::OwnerWorkerContext &Worker,
                                uint32_t FuncIndex, EJitSmallTableHost *Host,
                                SmallTableOwnerWorkerJob Job,
                                bool InitialHandoff = true);
bool smallTableSreLeaveHostTicket(
    const detail::OwnerWorkerContext &Worker, EJitSmallTableHost *ExpectedHost,
    uint64_t Ticket);

} // namespace ejit
} // namespace llvm

#endif // LLVM_LIB_EXECUTIONENGINE_EJIT_EJITOWNERWORKERCONTEXT_H
