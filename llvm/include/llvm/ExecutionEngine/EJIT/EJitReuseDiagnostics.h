//===-- EJitReuseDiagnostics.h - bounded owner-side reuse explanations -----===//
#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITREUSEDIAGNOSTICS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITREUSEDIAGNOSTICS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitFrozenValues.h"
#include "llvm/ExecutionEngine/EJIT/EJitRepresentativeDiagnostics.h"
#include <algorithm>
#include <cstring>

namespace llvm {
namespace ejit {

// Diagnostic-only copies; no pointers to LLVM IR, business data, or registries.
struct EJitReuseDiagnostic {
  EJitFrozenComparison frozen;
  uint64_t sequence = 0, peerGroup = 0, peerCode = 0, repeats = 0;
  EJitRepresentativeDiagIdentity identity;
  uint32_t level = 1, diffLine = 0;
  uint64_t diffOffset = 0;
  bool truncated = false;
  char entry[96] = {}, stage[24] = {}, reason[48] = {}, action[32] = {};
  char detail[192] = {}, left[257] = {}, right[257] = {};
};

// Single nonblocking lock: a shell print/config must not stall the compiler.
// Contention loses diagnostics only, never a compilation or reuse decision.
class EJitReuseDiagnosticStore {
public:
  static constexpr unsigned Capacity = 16;
  static constexpr unsigned DefaultLevel = 2;
  struct Snapshot {
    EJitReuseDiagnostic records[Capacity];
    uint32_t count = 0, level = 0;
    uint64_t evicted = 0, dropped = 0;
    char filter[96] = {};
  };
  template <size_t N> static bool copyText(char (&To)[N], StringRef From) {
    size_t Count = std::min(From.size(), N - 1);
    for (size_t I = 0; I < Count; ++I) {
      unsigned char C = From[I];
      To[I] = (C >= 32 && C < 127) ? static_cast<char>(C) : ' ';
    }
    To[Count] = 0;
    return Count != From.size();
  }
  static bool validFilter(StringRef Filter) {
    if (Filter.size() >= 96) return false;
    for (unsigned char C : Filter)
      if (C < 32 || C >= 127) return false;
    return true;
  }
  bool configure(StringRef Filter, uint32_t Level) {
    if (Level > 2 || !validFilter(Filter)) return false;
    if (!tryLock()) return false;
    copyText(filter_, Filter.empty() ? StringRef("*") : Filter);
    level_ = Level;
    // A new capture configuration starts a fresh bounded diagnostic window.
    count_ = next_ = 0;
    evicted_ = 0;
    dropped_.storeRelaxed(0);
    unlock();
    return true;
  }
  unsigned levelFor(StringRef Entry) {
    if (!tryLock()) { dropped_.fetchAdd(1); return 0; }
    unsigned Level = (StringRef(filter_) == "*" || Entry == filter_) ? level_ : 0;
    unlock();
    return Level;
  }
  bool record(EJitReuseDiagnostic &R) {
    if (!tryLock()) { dropped_.fetchAdd(1); return false; }
    if (!level_ || (StringRef(filter_) != "*" && StringRef(R.entry) != filter_)) {
      unlock();
      return false;
    }
    for (unsigned I = 0; I < count_; ++I) {
      auto &Old = records_[I];
      bool SameDims = Old.identity.numDims == R.identity.numDims;
      for (unsigned D = 0; SameDims && D < R.identity.numDims &&
                           D < kEJitMaxRequestDims; ++D)
        SameDims = Old.identity.dims[D].dimType == R.identity.dims[D].dimType &&
                   Old.identity.dims[D].instanceId == R.identity.dims[D].instanceId &&
                   Old.identity.versions[D] == R.identity.versions[D];
      if (Old.identity.funcIndex == R.identity.funcIndex &&
          SameDims &&
          Old.identity.generation == R.identity.generation &&
          Old.identity.groupId == R.identity.groupId &&
          Old.identity.groupGeneration == R.identity.groupGeneration &&
          Old.peerGroup == R.peerGroup && Old.peerCode == R.peerCode &&
          !std::strcmp(Old.stage, R.stage) && !std::strcmp(Old.reason, R.reason) &&
          !std::strcmp(Old.action, R.action)) {
        ++Old.repeats;
        unlock();
        return false;
      }
    }
    R.sequence = ++sequence_;
    R.level = std::min(R.level, level_);
    if (R.level < 2) R.left[0] = R.right[0] = 0;
    records_[next_] = R;
    next_ = (next_ + 1) % Capacity;
    if (count_ < Capacity) ++count_; else ++evicted_;
    unlock();
    return true;
  }
  bool snapshot(Snapshot &Out) {
    if (!tryLock()) return false;
    Out.count = count_; Out.level = level_; Out.evicted = evicted_;
    Out.dropped = dropped_.loadRelaxed();
    copyText(Out.filter, filter_);
    unsigned Start = count_ == Capacity ? next_ : 0;
    for (unsigned I = 0; I < count_; ++I)
      Out.records[I] = records_[(Start + I) % Capacity];
    unlock();
    return true;
  }
  bool reset() {
    if (!tryLock()) return false;
    count_ = next_ = 0; evicted_ = 0; dropped_.storeRelaxed(0);
    unlock();
    return true;
  }
private:
  bool tryLock() { uint32_t Expected = 0; return lock_.compareExchange(Expected, 1); }
  void unlock() { lock_.storeRelease(0); }
  EJitAtomicU32 lock_{0};
  EJitAtomicU64 dropped_{0};
  EJitReuseDiagnostic records_[Capacity];
  uint64_t sequence_ = 0, evicted_ = 0;
  uint32_t count_ = 0, next_ = 0, level_ = DefaultLevel;
  char filter_[96] = "*";
};

// Core-local, not shared ABI. Shell API checks compile-owner core explicitly.
EJitReuseDiagnosticStore &reuseDiagnosticStore();
void printReuseDiagnostic(const EJitReuseDiagnostic &R, bool ShowDetails = true);
void recordReuseDiagnostic(EJitReuseDiagnostic R);

} // namespace ejit
} // namespace llvm
#endif
