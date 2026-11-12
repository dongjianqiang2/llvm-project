//===-- EJitRegistrationStore.cpp - Registration Staging Area
//--------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include <limits>
#include <mutex>

using namespace llvm::ejit;

EJitRegistrationStore &EJitRegistrationStore::instance() {
  static EJitRegistrationStore store;
  return store;
}

bool EJitRegistrationStore::registerPointerPeriodBounds(
    const std::string &varName, const void *base, uint64_t size) {
  if (varName.empty() || !base || !size ||
      size > std::numeric_limits<uintptr_t>::max() -
                 reinterpret_cast<uintptr_t>(base))
    return false;
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  for (const auto &Entry : pointerPeriodBounds_)
    if (Entry.varName == varName)
      return Entry.base == base && Entry.size == size;
  pointerPeriodBounds_.push_back({varName, base, size});
  return true;
}

bool EJitRegistrationStore::setAllocationSizeLookup(AllocationSizeLookup Lookup) {
  if (!Lookup.callback)
    return false;
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  if (allocationSizeLookup_.callback)
    return allocationSizeLookup_.callback == Lookup.callback &&
           allocationSizeLookup_.context == Lookup.context;
  allocationSizeLookup_ = Lookup;
  return true;
}

void EJitRegistrationStore::registerBitcode(const std::string &funcName,
                                            const uint8_t *data, size_t size) {
  EJIT_DIAG_DEBUG("regstore stage bitcode name=%s data=%p size=%zu",
                  funcName.c_str(), static_cast<const void *>(data), size);
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  bitcodes_.push_back({funcName, data, size});
}

void EJitRegistrationStore::registerPeriodArray(const std::string &periodName,
                                                const std::string &varName,
                                                void *baseAddr,
                                                uint64_t arraySize) {
  EJIT_DIAG_DEBUG("regstore stage periodArray period=%s var=%s base=%p size=%llu",
                  periodName.c_str(), varName.c_str(), baseAddr,
                  static_cast<unsigned long long>(arraySize));
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  periodArrays_.push_back({periodName, varName, baseAddr, arraySize});
}

void EJitRegistrationStore::registerStaticVar(const std::string &varName,
                                              void *varAddr) {
  EJIT_DIAG_DEBUG("regstore stage staticVar var=%s addr=%p", varName.c_str(),
                  varAddr);
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  staticVars_.push_back({varName, varAddr});
}

void EJitRegistrationStore::registerSymbol(const std::string &name,
                                           void *addr) {
  EJIT_DIAG_DEBUG("regstore stage symbol name=%s addr=%p", name.c_str(), addr);
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  userSymbols_.push_back({name, addr});
}

void EJitRegistrationStore::recordError(int code, const std::string &message,
                                        const std::string &funcName) {
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  // Preserve the first error so the earliest root cause is reported.
  if (error_.code != 0) {
    EJIT_DIAG_DEBUG("regstore recordError suppressed (first error kept): "
                    "code=%d func=%s msg=%s",
                    code, funcName.c_str(), message.c_str());
    return;
  }
  error_.code = code;
  error_.message = message;
  error_.funcName = funcName;
  EJIT_DIAG("regstore recordError: code=%d func=%s msg=%s", code,
            funcName.c_str(), message.c_str());
}

bool EJitRegistrationStore::hasError() const {
  // error_ is only written under mutex_; a relaxed read of the code is fine for
  // the single-threaded startup registration phase.
  return error_.code != 0;
}

RegistrationError EJitRegistrationStore::consumeError() {
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  RegistrationError taken = error_;
  error_ = RegistrationError{};
  // Only surface at INFO when there was an actual error; the common no-error
  // case (code=0) is DEBUG so it does not spam every init.
  if (taken.code != 0)
    EJIT_DIAG("regstore consumeError: code=%d func=%s", taken.code,
              taken.funcName.c_str());
  else
    EJIT_DIAG_DEBUG("regstore consumeError: code=0 (no error)");
  return taken;
}

StoredData EJitRegistrationStore::consume() {
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  StoredData data;
  data.bitcodes = std::move(bitcodes_);
  data.periodArrays = std::move(periodArrays_);
  data.staticVars = std::move(staticVars_);
  data.userSymbols = std::move(userSymbols_);
  data.pointerPeriodBounds = std::move(pointerPeriodBounds_);
  data.allocationSizeLookup = allocationSizeLookup_;
  allocationSizeLookup_ = {};
  pointerPeriodBounds_.clear();
  bitcodes_.clear();
  periodArrays_.clear();
  staticVars_.clear();
  userSymbols_.clear();
  EJIT_DIAG("regstore consume: bitcodes=%zu periodArrays=%zu staticVars=%zu "
            "symbols=%zu",
            data.bitcodes.size(), data.periodArrays.size(),
            data.staticVars.size(), data.userSymbols.size());
  return data;
}
