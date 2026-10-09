//===-- EJitSharedData.h - Bounded shared data allocation identity -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDDATA_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDDATA_H

#include <cstdint>

namespace llvm {
namespace ejit {

/// By-value identity of one independent DataOnly allocation. This is data,
/// never a compiled-code range. Only an exact live allocator record validates
/// it; knowing an address inside the fixed reservation does not grant access.
/// The containing block is exclusive to data and never enters an executable
/// manager's seal/finalization/abandon paths. bytes includes page padding,
/// payloadBytes is the requested column layout. Neither storage is recycled.
struct EJitSreDataAllocation {
  uintptr_t address;
  uintptr_t blockBase;
  uint64_t bytes;
  uint64_t payloadBytes;
  uint64_t blockBytes;
  uint64_t domainIdentity;
  uint64_t identity;
  uint64_t generation;
  uint32_t recordIndex;
  uint32_t reserved;
};

/// Permanent NO_RECLAIM accounting. claimed includes whole exclusive blocks;
/// consumed includes page-rounded resources (also released/failed resources).
/// Do not add claimed and consumed: consumed is contained in claimed.
struct EJitSreDataAllocationStats {
  uint64_t claimedBytes;
  uint64_t consumedBytes;
  uint64_t liveBytes;
  uint64_t releasedBytes;
  uint64_t failedBytes;
  uint64_t failedClaimBytes;
  uint64_t allocationCount;
  uint64_t releasedCount;
  uint64_t failedCount;
  uint64_t rejectedCount;
  uint32_t blockCount;
  uint32_t recordCount;
  uint32_t snapshotValid;
  uint32_t reserved;
};

} // namespace ejit
} // namespace llvm

#endif
