//===-- EJitRuntimeState.cpp - Activate/Deactivate State ------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include <cassert>
#include <limits>
#include <mutex>

using namespace llvm::ejit;

bool PeriodArrayRegistry::registerPointerPeriodBounds(
    const std::string &varName, const void *base, uint64_t size) {
  if (varName.empty() || !base || !size ||
      size > std::numeric_limits<uintptr_t>::max() -
                 reinterpret_cast<uintptr_t>(base))
    return false;
  auto Insert =
      pointerBounds_.emplace(varName, PointerPeriodBounds{base, size});
  return Insert.second || (Insert.first->second.base == base &&
                           Insert.first->second.size == size);
}

const PointerPeriodBounds *
PeriodArrayRegistry::getPointerPeriodBounds(const std::string &varName) const {
  auto It = pointerBounds_.find(varName);
  return It == pointerBounds_.end() ? nullptr : &It->second;
}

std::optional<PointerPeriodBounds>
PeriodArrayRegistry::resolvePointerPeriodBounds(const std::string &varName,
                                               const void *Pointee) const {
  if (const auto *Explicit = getPointerPeriodBounds(varName))
    return *Explicit;
  if (!Pointee || !allocationSizeLookup_.callback)
    return std::nullopt;
  uint64_t Size =
      allocationSizeLookup_.callback(Pointee, allocationSizeLookup_.context);
  if (!Size)
    return std::nullopt;
  // An extent that wraps the address space authorizes nothing.
  if (Size > std::numeric_limits<uintptr_t>::max() -
                 reinterpret_cast<uintptr_t>(Pointee))
    Size = 0;
  return PointerPeriodBounds{Pointee, Size};
}

void PeriodArrayRegistry::registerArray(const std::string &periodName,
                                        const std::string &varName,
                                        void *baseAddr, size_t size) {
  // Idempotent: the constructor path may register the same array twice
  // (PASS1+PASS2 both add to global_ctors, or the static-registry walk
  // revisits an entry). A repeat with identical (period, base, size) is a
  // no-op (DEBUG only); a same-name conflict with different data is a clean
  // rejection that keeps the first registration. This also prevents duplicate
  // entries accumulating in arraysByPeriod_ (which would make activate fan out
  // redundantly and getArrays() report ghosts).
  auto existing = varNameIndex_.find(varName);
  if (existing != varNameIndex_.end()) {
    const PeriodArrayInfo &first = existing->second;
    if (first.periodName == periodName && first.baseAddr == baseAddr &&
        first.arraySize == size) {
      EJIT_DIAG_DEBUG("registry registerArray idempotent skip period=%s "
                      "var=%s base=%p size=%zu",
                      periodName.c_str(), varName.c_str(), baseAddr, size);
      return;
    }
    EJIT_DIAG("registry registerArray CONFLICT var=%s: kept first "
              "(period=%s base=%p size=%zu), rejected (period=%s base=%p "
              "size=%zu)",
              varName.c_str(), first.periodName.c_str(), first.baseAddr,
              first.arraySize, periodName.c_str(), baseAddr, size);
    return;
  }
  EJIT_DIAG_VERBOSE("registry registerArray period=%s var=%s base=%p size=%zu",
                    periodName.c_str(), varName.c_str(), baseAddr, size);
  PeriodArrayInfo info{varName, periodName, baseAddr, size};
  arraysByPeriod_[periodName].push_back(info);
  varNameIndex_[varName] = info;
  baseAddrIndex_[reinterpret_cast<uintptr_t>(baseAddr)] = info;
}

void PeriodArrayRegistry::registerStaticVar(const std::string &varName,
                                            void *varAddr) {
  // Idempotent (see registerArray): a repeat with the same addr is a no-op;
  // a same-name/different-addr conflict keeps the first registration and
  // prevents staticVars_ from accumulating duplicates.
  auto existing = staticVarIndex_.find(varName);
  if (existing != staticVarIndex_.end()) {
    if (existing->second == varAddr) {
      EJIT_DIAG_DEBUG("registry registerStaticVar idempotent skip var=%s "
                      "addr=%p",
                      varName.c_str(), varAddr);
      return;
    }
    EJIT_DIAG("registry registerStaticVar CONFLICT var=%s: kept addr=%p, "
              "rejected addr=%p",
              varName.c_str(), existing->second, varAddr);
    return;
  }
  EJIT_DIAG_VERBOSE("registry registerStaticVar var=%s addr=%p",
                    varName.c_str(), varAddr);
  staticVars_.push_back({varName, varAddr});
  staticVarIndex_[varName] = varAddr;
}

const std::vector<PeriodArrayInfo> *
PeriodArrayRegistry::getArrays(const std::string &periodName) const {
  auto it = arraysByPeriod_.find(periodName);
  if (it == arraysByPeriod_.end())
    return nullptr;
  return &it->second;
}

const PeriodArrayInfo *
PeriodArrayRegistry::getArrayInfo(const std::string &varName) const {
  auto it = varNameIndex_.find(varName);
  if (it == varNameIndex_.end())
    return nullptr;
  return &it->second;
}

void *
PeriodArrayRegistry::getStaticVarAddr(const std::string &varName) const {
  auto it = staticVarIndex_.find(varName);
  if (it == staticVarIndex_.end())
    return nullptr;
  return it->second;
}

const PeriodArrayInfo *
PeriodArrayRegistry::getArrayByBaseAddr(void *addr) const {
  auto it = baseAddrIndex_.find(reinterpret_cast<uintptr_t>(addr));
  if (it == baseAddrIndex_.end())
    return nullptr;
  return &it->second;
}

void EJitRuntimeState::activate(const std::string &periodName,
                                uint8_t cellIdx) {
  EJIT_DIAG_DEBUG("runtimeState activate period=%s cellIdx=%u",
                  periodName.c_str(), cellIdx);
#ifndef EJIT_FREESTANDING
  std::lock_guard<decltype(mutex_)> lock(mutex_);
#endif
  // Period-level: fan out to all arrays registered under this period name.
  const auto *arrs = registry_.getArrays(periodName);
  if (arrs) {
    for (const auto &info : *arrs)
      arrayStates_[reinterpret_cast<uintptr_t>(info.baseAddr)][cellIdx] =
          PeriodState::Active;
  }
}

void EJitRuntimeState::deactivate(const std::string &periodName,
                                  uint8_t cellIdx) {
  EJIT_DIAG_DEBUG("runtimeState deactivate period=%s cellIdx=%u",
                  periodName.c_str(), cellIdx);
#ifndef EJIT_FREESTANDING
  std::lock_guard<decltype(mutex_)> lock(mutex_);
#endif
  const auto *arrs = registry_.getArrays(periodName);
  if (arrs) {
    for (const auto &info : *arrs)
      arrayStates_[reinterpret_cast<uintptr_t>(info.baseAddr)][cellIdx] =
          PeriodState::Inactive;
  }
}

void EJitRuntimeState::activateAll(const std::string &periodName) {
  EJIT_DIAG_DEBUG("runtimeState activateAll period=%s", periodName.c_str());
#ifndef EJIT_FREESTANDING
  std::lock_guard<decltype(mutex_)> lock(mutex_);
#endif
  const auto *arrs = registry_.getArrays(periodName);
  if (arrs) {
    for (const auto &info : *arrs) {
      auto &inner = arrayStates_[reinterpret_cast<uintptr_t>(info.baseAddr)];
      for (size_t i = 0; i < info.arraySize; i++)
        inner[static_cast<uint8_t>(i)] = PeriodState::Active;
    }
  }
}

void EJitRuntimeState::deactivateAll(const std::string &periodName) {
  EJIT_DIAG_DEBUG("runtimeState deactivateAll period=%s", periodName.c_str());
#ifndef EJIT_FREESTANDING
  std::lock_guard<decltype(mutex_)> lock(mutex_);
#endif
  const auto *arrs = registry_.getArrays(periodName);
  if (arrs) {
    for (const auto &info : *arrs) {
      auto &inner = arrayStates_[reinterpret_cast<uintptr_t>(info.baseAddr)];
      for (size_t i = 0; i < info.arraySize; i++)
        inner[static_cast<uint8_t>(i)] = PeriodState::Inactive;
    }
  }
}

bool EJitRuntimeState::isActive(const std::string &periodName,
                                uint8_t cellIdx) const {
#ifndef EJIT_FREESTANDING
  std::lock_guard<decltype(mutex_)> lock(mutex_);
#endif
  // The built-in "static" time window is always active (SPEC4 §2.1).
  // static variables are registered via ejit_register_static_var, not
  // listed in arraysByPeriod_, so getArrays("static") returns nullptr.
  if (periodName == "static")
    return true;

  // Return true if ANY array registered under this period name is active
  // at the given cellIdx (period-level semantics for JIT compile decisions).
  const auto *arrs = registry_.getArrays(periodName);
  if (!arrs)
    return false;
  for (const auto &info : *arrs) {
    auto pit = arrayStates_.find(reinterpret_cast<uintptr_t>(info.baseAddr));
    if (pit == arrayStates_.end())
      continue;
    auto cit = pit->second.find(cellIdx);
    if (cit != pit->second.end() && cit->second == PeriodState::Active)
      return true;
  }
  return false;
}
