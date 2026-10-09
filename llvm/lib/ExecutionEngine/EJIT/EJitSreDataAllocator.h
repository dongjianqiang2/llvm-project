//===-- EJitSreDataAllocator.h - Private DataOnly arena algorithm -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITSREDATAALLOCATOR_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITSREDATAALLOCATOR_H

#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedData.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <type_traits>

namespace llvm {
namespace ejit {
namespace detail {

constexpr uint64_t SreDataBlockBytes = 2u << 20;
constexpr uint64_t SreDataPageBytes = 4096;
constexpr uint64_t SreDataResourceLimit = 4u << 20;
constexpr uint64_t SreDataHistoricalLimit = 16u << 20;
constexpr uint32_t SreDataMaxBlocks = 8;
constexpr uint32_t SreDataMaxRecords = 256;

struct SreDataOnlyBlock {
  uintptr_t base;
  uint64_t bytes;
  uint64_t used;
};
struct SreDataOnlyRecord {
  EJitSreDataAllocation allocation;
  uint32_t state; // 0 unused, 1 live, 2 released, 3 failed: never becomes 0.
};
/// Shared backing contains scalar POD only, with no repeated init-array work.
/// A host fixture owns a zero-initialized instance of this exact state/algorithm.
struct alignas(64) SreDataOnlyState {
  EJitAtomicU32 lock;
  EJitAtomicU64 domainIdentity;
  uintptr_t domainBase;
  uint64_t domainBytes;
  SreDataOnlyBlock blocks[SreDataMaxBlocks];
  SreDataOnlyRecord records[SreDataMaxRecords];
  EJitSreDataAllocationStats stats;
};
static_assert(std::is_standard_layout<SreDataOnlyState>::value,
              "data allocator state must have fixed scalar layout");
static_assert(std::is_trivially_default_constructible<SreDataOnlyState>::value,
              "shared DataOnly state must not run init-array constructors");

/// Same bounded allocation algorithm in production and tests. Claim returns an
/// exclusive contiguous range from the SAME monotonic domain cursor used by
/// code managers. It is invoked once for a multi-block resource, never once per
/// block (which could interleave code claims). No memory is read or written.
class SreDataOnlyArena {
public:
  using ClaimRangeFn = uintptr_t (*)(void *Context, uint64_t Bytes);

  SreDataOnlyArena(SreDataOnlyState &State, uintptr_t DomainBase,
                  uint64_t DomainBytes, uint64_t DomainIdentity,
                  ClaimRangeFn Claim, void *Context)
      : state_(State), base_(DomainBase), size_(DomainBytes),
        identity_(DomainIdentity), claim_(Claim), context_(Context) {}

  Error allocate(uint64_t Payload, uint64_t Generation,
                 EJitSreDataAllocation &Out) {
    Out = {};
    Guard G(state_);
    if (!G.held)
      return refusal("DataOnly allocator busy");
    if (!domainValid() || !identity_ || !claim_ || !Generation || !Payload ||
        Payload > SreDataResourceLimit)
      return reject("DataOnly resource identity, size or domain invalid");
    if (state_.stats.blockCount > SreDataMaxBlocks ||
        state_.stats.recordCount > SreDataMaxRecords ||
        state_.stats.claimedBytes > SreDataHistoricalLimit ||
        state_.stats.consumedBytes > state_.stats.claimedBytes)
      return reject("DataOnly allocator accounting invalid");
    if (state_.stats.recordCount >= SreDataMaxRecords)
      return reject("DataOnly allocation-record capacity exhausted");
    const uint64_t Bytes = (Payload + SreDataPageBytes - 1) &
                           ~(SreDataPageBytes - 1);
    if (!state_.domainBase) {
      state_.domainBase = base_;
      state_.domainBytes = size_;
    }
    if (state_.domainBase != base_ || state_.domainBytes != size_ ||
        state_.domainIdentity.loadAcquire() != identity_)
      return reject("DataOnly allocation domain changed");

    SreDataOnlyBlock *Block = nullptr;
    for (uint32_t I = 0; I < state_.stats.blockCount; ++I) {
      auto &B = state_.blocks[I];
      if (B.used <= B.bytes && Bytes <= B.bytes - B.used) {
        Block = &B;
        break;
      }
    }
    if (!Block) {
      const uint64_t ClaimBytes = (Bytes + SreDataBlockBytes - 1) &
                                  ~(SreDataBlockBytes - 1);
      if (state_.stats.blockCount >= SreDataMaxBlocks ||
          state_.stats.claimedBytes > SreDataHistoricalLimit ||
          ClaimBytes > SreDataHistoricalLimit - state_.stats.claimedBytes)
        return reject("DataOnly historical claimed-block budget exhausted");
      const uintptr_t Address = claim_(context_, ClaimBytes);
      if (!Address)
        return reject("DataOnly fixed reservation exhausted; no heap fallback");
      // The underlying monotonic cursor has already advanced. Even a malformed
      // return is charged; it may not become an invisible/reusable hole.
      state_.stats.claimedBytes += ClaimBytes;
      if ((Address & (SreDataBlockBytes - 1)) || Address < base_ ||
          Address > UINTPTR_MAX - ClaimBytes || Address + ClaimBytes > base_ + size_)
        return failedClaim(ClaimBytes, "DataOnly claim escaped its exact domain");
      for (uint32_t I = 0; I < state_.stats.blockCount; ++I) {
        const auto &B = state_.blocks[I];
        if (Address < B.base + B.bytes && B.base < Address + ClaimBytes)
          return failedClaim(ClaimBytes, "DataOnly claim overlaps older data storage");
      }
      Block = &state_.blocks[state_.stats.blockCount++];
      *Block = {Address, ClaimBytes, 0};
    }
    const uint32_t Slot = state_.stats.recordCount++;
    Out.address = Block->base + Block->used;
    Out.blockBase = Block->base;
    Out.bytes = Bytes;
    Out.payloadBytes = Payload;
    Out.blockBytes = Block->bytes;
    Out.domainIdentity = identity_;
    Out.identity = uint64_t(Slot) + 1;
    Out.generation = Generation;
    Out.recordIndex = Slot;
    Block->used += Bytes;
    state_.records[Slot].allocation = Out;
    state_.records[Slot].state = 1;
    state_.stats.consumedBytes += Bytes;
    state_.stats.liveBytes += Bytes;
    ++state_.stats.allocationCount;
    return Error::success();
  }

  bool validate(const EJitSreDataAllocation &A) const {
    Guard G(state_);
    return G.held && matches(A);
  }

  bool release(const EJitSreDataAllocation &A, bool Failed) {
    Guard G(state_);
    if (!G.held || !matches(A))
      return false;
    auto &R = state_.records[A.recordIndex];
    R.state = Failed ? 3 : 2;
    state_.stats.liveBytes -= A.bytes;
    if (Failed) {
      state_.stats.failedBytes += A.bytes;
      ++state_.stats.failedCount;
    } else {
      state_.stats.releasedBytes += A.bytes;
      ++state_.stats.releasedCount;
    }
    return true;
  }

  EJitSreDataAllocationStats stats() const {
    Guard G(state_);
    if (!G.held)
      return {};
    auto Result = state_.stats;
    Result.snapshotValid = 1;
    return Result;
  }

private:
  struct Guard {
    SreDataOnlyState &state;
    bool held = false;
    explicit Guard(SreDataOnlyState &State) : state(State) {
      // Normally one true worker owns allocation/release. Peer validation can
      // overlap it; bounded contention fails closed, never a fabricated proof.
      for (unsigned I = 0; I < 1024; ++I) {
        uint32_t Expected = 0;
        if (state.lock.compareExchange(Expected, 1)) {
          held = true;
          break;
        }
      }
    }
    ~Guard() { if (held) state.lock.storeRelease(0); }
  };
  bool domainValid() const {
    return base_ && size_ && !(base_ & (SreDataBlockBytes - 1)) &&
           !(size_ & (SreDataBlockBytes - 1)) && size_ <= UINTPTR_MAX - base_;
  }
  bool matches(const EJitSreDataAllocation &A) const {
    if (!domainValid() || !identity_ || A.domainIdentity != identity_ ||
        state_.domainIdentity.loadAcquire() != identity_ ||
        state_.domainBase != base_ || state_.domainBytes != size_ ||
        state_.stats.recordCount > SreDataMaxRecords ||
        A.recordIndex >= state_.stats.recordCount ||
        A.recordIndex >= SreDataMaxRecords || A.reserved || !A.generation)
      return false;
    const auto &R = state_.records[A.recordIndex];
    const auto &B = R.allocation;
    return R.state == 1 && A.address == B.address && A.blockBase == B.blockBase &&
           A.bytes == B.bytes && A.payloadBytes == B.payloadBytes &&
           A.blockBytes == B.blockBytes && A.identity == B.identity &&
           A.generation == B.generation && A.domainIdentity == B.domainIdentity;
  }
  static Error refusal(const char *Why) {
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  Error reject(const char *Why) {
    ++state_.stats.rejectedCount;
    return refusal(Why);
  }
  Error failedClaim(uint64_t Bytes, const char *Why) {
    state_.stats.failedClaimBytes += Bytes;
    ++state_.stats.failedCount;
    return reject(Why);
  }
  SreDataOnlyState &state_;
  uintptr_t base_;
  uint64_t size_;
  uint64_t identity_;
  ClaimRangeFn claim_;
  void *context_;
};

#ifndef EJIT_FREESTANDING
/// Private, explicit real-memory host test backend. Never available to SRE or
/// installed clients. The fixture must join all physical calls before leaving
/// its scope; destruction restores the previous backend, not arena cursors.
class ScopedSreDataOnlyTestDomain {
public:
  ScopedSreDataOnlyTestDomain(SreDataOnlyState &State, uintptr_t Base,
                            uint64_t Bytes, SreDataOnlyArena::ClaimRangeFn Claim,
                            void *Context);
  ~ScopedSreDataOnlyTestDomain();
  ScopedSreDataOnlyTestDomain(const ScopedSreDataOnlyTestDomain &) = delete;
  ScopedSreDataOnlyTestDomain &operator=(const ScopedSreDataOnlyTestDomain &) = delete;
  SreDataOnlyArena &arena() { return arena_; }
private:
  SreDataOnlyArena arena_;
  SreDataOnlyArena *previous_ = nullptr;
};
#endif

} // namespace detail
} // namespace ejit
} // namespace llvm

#endif
