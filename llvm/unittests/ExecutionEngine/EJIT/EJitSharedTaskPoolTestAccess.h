//===-- EJitSharedTaskPoolTestAccess.h - actual worker entry for tests -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_UNITTESTS_EXECUTIONENGINE_EJIT_SHAREDTASKPOOLTESTACCESS_H
#define LLVM_UNITTESTS_EXECUTIONENGINE_EJIT_SHAREDTASKPOOLTESTACCESS_H

#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "../../../lib/ExecutionEngine/EJIT/EJitOwnerWorkerContext.h"
#include <memory>
#include <utility>

namespace llvm {
namespace ejit {

/// Tests may invoke the SAME trampoline supplied to the real task scheduler.
/// No production token factory, alternate worker loop or public entry is added.
struct EJitSharedTaskPoolTestAccess {
  static void runRealWorker(EJitSharedTaskPool &Pool) {
    EJitSharedTaskPool::workerEntryThunk(&Pool);
  }
  static EJitSharedTaskPool::OwnerControlResult runControlOnOwnerAndWait(
      EJitSharedTaskPool &Pool,
      std::function<void(const llvm::ejit::detail::OwnerWorkerContext &)> Work,
      uint32_t WaitRounds = 1u << 20) {
    return Pool.runControlOnOwnerAndWait(std::move(Work), WaitRounds);
  }
  static EJitSharedTaskPool::OwnerControlResult runControlOnOwnerAndWait(
      EJitSharedTaskPool &Pool, const llvm::ejit::detail::OwnerWorkerContext &Context,
      std::function<void(const llvm::ejit::detail::OwnerWorkerContext &)> Work,
      uint32_t WaitRounds = 1u << 20) {
    return Pool.runControlOnOwnerAndWait(Context, std::move(Work), WaitRounds);
  }
  static bool isCurrentOwnerWorker(
      const EJitSharedTaskPool &Pool,
      const llvm::ejit::detail::OwnerWorkerContext &Context) {
    return Pool.isCurrentOwnerWorker(Context);
  }
  static bool abortFunctionPgoOnOwner(
      EJitSharedTaskPool &Pool, const llvm::ejit::detail::OwnerWorkerContext &Context,
      uint32_t FuncIndex) {
    return Pool.abortFunctionPgoOnOwner(Context, FuncIndex);
  }
  /// Copy real context values into a separate, long-lived UNIT-TEST object.
  /// This permits epoch/generation rejection testing without ever dereferencing
  /// the old worker's destroyed stack, or making production contexts copyable.
  static std::unique_ptr<llvm::ejit::detail::OwnerWorkerContext>
  cloneForStaleValidation(const llvm::ejit::detail::OwnerWorkerContext &Context) {
    return std::unique_ptr<llvm::ejit::detail::OwnerWorkerContext>(
        new llvm::ejit::detail::OwnerWorkerContext(Context.pool_, Context.poolEpoch_,
                                      Context.stateGeneration_));
  }
};

} // namespace ejit
} // namespace llvm
#endif
