//===-- EJitRuntimeState.h - Activate/Deactivate State --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITRUNTIMESTATE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITRUNTIMESTATE_H

#include "llvm/ExecutionEngine/EJIT/EJitAllocationBounds.h"
#include "llvm/ExecutionEngine/EJIT/EJitBareMetal.h"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#ifndef EJIT_FREESTANDING
#include <mutex>
#endif

namespace llvm {
namespace ejit {

struct PeriodArrayInfo {
  std::string varName;
  std::string periodName;
  void *baseAddr;
  size_t arraySize;
};

struct StaticVarInfo {
  std::string varName;
  void *varAddr;
};

/// An allocation extent behind a pointer-valued period global. Explicit bounds
/// stay pinned to their registered base; discovered bounds describe the
/// current pointee.
struct PointerPeriodBounds {
  const void *base;
  uint64_t size;
};

/// Registry of period arrays and static variables. Initialized once
/// at ejit_init time from StoredData, then read-only at runtime.
class PeriodArrayRegistry {
public:
  void registerArray(const std::string &periodName,
                     const std::string &varName,
                     void *baseAddr, size_t size);
  void registerStaticVar(const std::string &varName, void *varAddr);
  bool registerPointerPeriodBounds(const std::string &varName, const void *base,
                                   uint64_t size);
  const PointerPeriodBounds *
  getPointerPeriodBounds(const std::string &varName) const;
  /// Initialization only; the provider is immutable while workers run.
  void setAllocationSizeLookup(AllocationSizeLookup Lookup) {
    allocationSizeLookup_ = Lookup;
  }
  /// Explicit bounds win, even if stale. Otherwise ask the allocation lookup
  /// about \p Pointee.
  std::optional<PointerPeriodBounds>
  resolvePointerPeriodBounds(const std::string &varName,
                            const void *Pointee) const;

  const std::vector<PeriodArrayInfo> *getArrays(const std::string &periodName) const;
  const std::vector<StaticVarInfo> &getStaticVars() const { return staticVars_; }
  /// All period arrays grouped by period name. For diagnostics
  /// (ejit_print_registry): iterating this exposes every registered array
  /// without needing to know period names up front.
  const std::unordered_map<std::string, std::vector<PeriodArrayInfo>> &
  arraysByPeriod() const {
    return arraysByPeriod_;
  }
  const PeriodArrayInfo *getArrayInfo(const std::string &varName) const;
  void *getStaticVarAddr(const std::string &varName) const;

  const PeriodArrayInfo *getArrayByBaseAddr(void *addr) const;

private:
  std::unordered_map<std::string, std::vector<PeriodArrayInfo>> arraysByPeriod_;
  std::vector<StaticVarInfo> staticVars_;
  // Indexed by value for stable element addresses (vector push_back would
  // invalidate pointers to vector elements on reallocation).
  std::unordered_map<std::string, PeriodArrayInfo> varNameIndex_;
  std::unordered_map<std::string, void *> staticVarIndex_;
  std::unordered_map<std::string, PointerPeriodBounds> pointerBounds_;
  AllocationSizeLookup allocationSizeLookup_;
  std::unordered_map<uintptr_t, PeriodArrayInfo> baseAddrIndex_;
};

enum class PeriodState { Inactive, Active };

/// Manages activate/deactivate state of time-window period instances.
///
/// Activation is keyed by lifecycle/period name + instance index only:
///   - ejit_activate("cell", 3) → activates ALL arrays under "cell" at index 3
///     (fans out to every array registered under that period name).
///
/// There is no array-pointer dimension in the active state: if multiple arrays
/// share the same period name, activation is name-level for that period
/// instance. isActive() returns true if the period name is active at the given
/// cellIdx.
class EJitRuntimeState {
public:
#ifdef EJIT_FREESTANDING
  using MutexType = BareMetalMutex;
#else
  using MutexType = std::mutex;
#endif
  /// Period-level activation: activates all arrays registered under periodName.
  void activate(const std::string &periodName, uint8_t cellIdx);
  void deactivate(const std::string &periodName, uint8_t cellIdx);

  void activateAll(const std::string &periodName);
  void deactivateAll(const std::string &periodName);
  bool isActive(const std::string &periodName, uint8_t cellIdx) const;

  PeriodArrayRegistry &getRegistry() { return registry_; }
  const PeriodArrayRegistry &getRegistry() const { return registry_; }

private:
  PeriodArrayRegistry registry_;
#ifndef EJIT_FREESTANDING
  mutable MutexType mutex_;
#endif
  /// Array-level state: (baseAddr, cellIdx) → Active/Inactive.
  /// Period-level activate/deactivate fans out to all arrays under that period.
  std::unordered_map<uintptr_t, std::unordered_map<uint8_t, PeriodState>>
      arrayStates_;
};

} // namespace ejit
} // namespace llvm

#endif
