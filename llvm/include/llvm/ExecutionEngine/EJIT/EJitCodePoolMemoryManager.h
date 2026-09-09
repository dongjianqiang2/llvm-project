//===-- EJitCodePoolMemoryManager.h - JITLink mem mgr over code pool ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  A JITLink memory manager that backs all JIT segment memory with
//  EJitCodePoolManager's 2MiB pools instead of mmap/mprotect.
//
//  Unlike InProcessMemoryManager, finalize() does NOT apply memory protections:
//  the pool is left RW so JITLink/ORC can keep writing relocations, and the
//  RW->RX transition happens later, exactly once per 2MiB pool, via the pool
//  manager's enable_ex sealing (driven from the engine right before a function
//  pointer is returned). This keeps execute-permission flips at the required
//  2MiB granularity and avoids the W^X conflict that wrapping mprotect causes.
//
//  v1 does not reclaim pool memory on deallocate (only dealloc actions run);
//  pool lifetime equals the engine lifetime. See EJIT_SRE_CODE_POOL.md.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOLMEMORYMANAGER_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOLMEMORYMANAGER_H

#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h"
#include <functional>
#include <vector>

namespace llvm {
namespace ejit {

/// JITLinkMemoryManager that allocates JIT segments from EJitCodePoolManager
/// and never changes page protections itself (sealing is done out-of-band by
/// the pool manager). The referenced pool manager must outlive this object.
class EJitCodePoolMemoryManager : public jitlink::JITLinkMemoryManager {
public:
  /// The selector is supplied by EJitOrcEngine and is keyed by an internally
  /// created JITDylib. It returns nullptr for missing/corrupt metadata, which
  /// makes allocation fail instead of silently placing code in another pool.
  using PoolSelector =
      std::function<EJitCodePoolManager *(const jitlink::JITLinkDylib *)>;
  using ColdRangeRecorder =
      std::function<void(uintptr_t, uint64_t, const EJitColdCodeRange &)>;

  EJitCodePoolMemoryManager(EJitCodePoolManager &Pool, size_t PageSize);
  EJitCodePoolMemoryManager(EJitCodePoolManager &NearPool,
                            EJitCodePoolManager &FarPool, size_t PageSize);
  EJitCodePoolMemoryManager(std::vector<EJitCodePoolManager *> NearPools,
                            EJitCodePoolManager &FarPool, size_t PageSize,
                            PoolSelector Selector,
                            PoolSelector ColdSelector = {},
                            ColdRangeRecorder RecordCold = {});

  void allocate(const jitlink::JITLinkDylib *JD, jitlink::LinkGraph &G,
                OnAllocatedFunction OnAllocated) override;

  void deallocate(std::vector<FinalizedAlloc> Allocs,
                  OnDeallocatedFunction OnDeallocated) override;

  // Bring in the convenience / blocking overloads hidden by the overrides.
  using JITLinkMemoryManager::allocate;
  using JITLinkMemoryManager::deallocate;

  EJitCodePoolManager &getPool() { return *NearPool_; }

private:
  class InFlightAllocImpl;
  struct FinalizedInfo;

  EJitCodePoolManager *selectPool(const jitlink::JITLinkDylib *JD) const;

  EJitCodePoolManager *NearPool_ = nullptr;
  std::vector<EJitCodePoolManager *> NearPools_;
  EJitCodePoolManager *FarPool_ = nullptr;
  PoolSelector Selector_;
  PoolSelector ColdSelector_;
  ColdRangeRecorder RecordCold_;
  size_t PageSize_;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOLMEMORYMANAGER_H
