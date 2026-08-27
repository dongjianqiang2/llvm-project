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

namespace llvm {
namespace ejit {

/// JITLinkMemoryManager that allocates JIT segments from EJitCodePoolManager
/// and never changes page protections itself (sealing is done out-of-band by
/// the pool manager). The referenced pool manager must outlive this object.
class EJitCodePoolMemoryManager : public jitlink::JITLinkMemoryManager {
public:
  EJitCodePoolMemoryManager(EJitCodePoolManager &Pool, size_t PageSize);
  EJitCodePoolMemoryManager(EJitCodePoolManager &NearPool,
                            EJitCodePoolManager &FarPool, size_t PageSize);
  EJitCodePoolMemoryManager(EJitCodePoolManager &NearPool,
                            EJitCodePoolManager &ColdPool,
                            EJitCodePoolManager &FarPool, size_t PageSize);
  ~EJitCodePoolMemoryManager() override;

  void allocate(const jitlink::JITLinkDylib *JD, jitlink::LinkGraph &G,
                OnAllocatedFunction OnAllocated) override;

  void deallocate(std::vector<FinalizedAlloc> Allocs,
                  OnDeallocatedFunction OnDeallocated) override;

  // Bring in the convenience / blocking overloads hidden by the overrides.
  using JITLinkMemoryManager::allocate;
  using JITLinkMemoryManager::deallocate;

  EJitCodePoolManager &getPool() { return NearPool_; }

  /// Resolve an entry pointer to every executable extent in its finalized
  /// allocation. A split Tier-2 allocation has a hot primary range and one
  /// cold companion range that peers must also make executable.
  bool findAllocation(const void *Ptr, EJitCompiledCodeInfo &Out) const;

private:
  class InFlightAllocImpl;
  struct FinalizedInfo;
  struct State;

  EJitCodePoolManager &selectPool(const jitlink::JITLinkDylib *JD) const;

  EJitCodePoolManager &NearPool_;
  EJitCodePoolManager *ColdPool_ = nullptr;
  EJitCodePoolManager *FarPool_ = nullptr;
  size_t PageSize_;
  std::unique_ptr<State> State_;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOLMEMORYMANAGER_H
