//===-- EJitSreQueue.h - Queue abstraction for the SRE taskpool -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Queue abstraction for the EmbeddedJIT SRE taskpool.
//
//  Per spec §3.3.2 the SRE platform provides no queue primitive, so EJitQueue
//  is a self-implemented lock-free bounded MPSC ring buffer (Vyukov style),
//  shared by host and SRE builds. It needs no mutex, is single-thread testable,
//  and references no external platform queue symbol.
//
//  This header also owns the fixed-layout request record carried by the queue,
//  EJitCompileRequest. It lives here (the lowest-level taskpool header) so the
//  queue can store it by value without a circular include against
//  EJitTaskPool.h. EJitTaskPool.h re-exports it via this include.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSREQUEUE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSREQUEUE_H

#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include <cstdint>

//===----------------------------------------------------------------------===//
// Compile-time configuration (overridable by the build via -D). Sensible
// defaults keep the headers self-contained when included standalone.
//===----------------------------------------------------------------------===//
#ifndef EJIT_SRE_TASKPOOL_QUEUE_CAPACITY
#define EJIT_SRE_TASKPOOL_QUEUE_CAPACITY 1024u
#endif
#ifndef EJIT_BOUND_PTR_MAX_BYTES
#define EJIT_BOUND_PTR_MAX_BYTES 256u
#endif
static_assert(EJIT_BOUND_PTR_MAX_BYTES > 0 &&
                  (EJIT_BOUND_PTR_MAX_BYTES % 8u) == 0,
              "EJIT_BOUND_PTR_MAX_BYTES must be a positive multiple of 8");

namespace llvm {
namespace ejit {

//===----------------------------------------------------------------------===//
// EJitCompileRequest
//
// Fixed-layout compile request. Only uint32_t / uint64_t / uintptr_t fields,
// no bitfields, no constructors/destructors, no STL — so it is trivially
// copyable through a platform queue and has identical field semantics on
// little- and big-endian targets (fields are accessed by type, never parsed
// byte-by-byte). It must NOT be serialized across hosts of differing
// pointer width.
//===----------------------------------------------------------------------===//
struct EJitDimPair {
  uint32_t dimType;
  uint32_t instanceId;
};

struct EJitCompileRequest {
  uint32_t funcIndex;
  uint32_t numDims;
  EJitDimPair dims[4];
  uint32_t versions[4];
  /// Frozen at the final real Tier-1 dispatch. Tier-2 must use these values
  /// instead of measuring after queue/worker delay. pgoSampleEntries == 0
  /// means this timestamp is unavailable; the timestamp itself may be zero.
  uint64_t pgoSampleEnd;
  uintptr_t fallbackPtr;
  // Shared-taskpool owner generation captured at enqueue time. A worker drops a
  // request whose generation no longer equals the shared state's generation
  // (owner re-init), so a stale request can never pollute a new generation's
  // cache. Unused (left 0) by the non-shared taskpool. Endianness: a plain
  // fixed-width scalar accessed by value, never byte-parsed.
  uint32_t generation;
  uint32_t pgoSampleEntries;
  // Optional shallow pointee snapshot for ejit_bound_ptr. It is stored inline
  // so an async request owns every byte it needs after the caller returns.
  // boundSize == 0 means no bound pointer. No raw caller pointer crosses the
  // queue boundary.
  uint32_t boundArgIndex;
  uint32_t boundSize;
  alignas(uintptr_t) uint8_t boundData[EJIT_BOUND_PTR_MAX_BYTES];
};

// Size is stable per pointer width. The 64-bit layout retains 8-byte alignment;
// the 32-bit layout is 4-byte aligned.
static_assert(sizeof(EJitCompileRequest) ==
                  (sizeof(uintptr_t) == 8 ? 88u + EJIT_BOUND_PTR_MAX_BYTES
                                          : 84u + EJIT_BOUND_PTR_MAX_BYTES),
              "EJitCompileRequest size must stay stable (incl. PGO sample)");
static_assert(alignof(EJitCompileRequest) <= 8,
              "EJitCompileRequest alignment must stay <= 8 bytes");
static_assert(sizeof(uintptr_t) == 4 || sizeof(uintptr_t) == 8,
              "EJitCompileRequest assumes a 32- or 64-bit pointer width");

//===----------------------------------------------------------------------===//
// Compile-tier encoding for the queue (EJIT_ONLINE_PGO.md §6.3).
//
// CompileTier (EJitOrcEngine.h) is {Baseline=0, Instrumented=1, PGOUse=2}. It
// is transported through the queue in the TOP 2 BITS of funcIndex, which is
// <2^30 in practice. This needs NO layout change and NO queue-ABI bump.
//
// CRITICAL: cacheKey is built from the STRIPPED funcIndex (stripReqTier), so
// Tier-1 and Tier-2 of the same (funcIndex, dims) share one cacheKey - that is
// how Tier-2 publish overwrites the Tier-1 slot (§2/§7.1). The tier is decoded
// separately and carried on SpecializationContext, never in cacheKey.
//===----------------------------------------------------------------------===//
constexpr uint32_t kEJitTierShift = 30;
constexpr uint32_t kEJitTierMask = 0x3u << kEJitTierShift;

/// CompileTier values (mirror CompileTier in EJitOrcEngine.h) for code that
/// must not include EJitOrcEngine.h (e.g. the taskpool).
constexpr uint32_t kEJitTierBaseline = 0;
constexpr uint32_t kEJitTierInstrumented = 1;
constexpr uint32_t kEJitTierPgoUse = 2;

/// Encode a tier (CompileTier value) into funcIndex's top 2 bits.
inline uint32_t encodeReqTier(uint32_t funcIndex, uint32_t tier) {
  return (funcIndex & ~kEJitTierMask) | ((tier & 0x3u) << kEJitTierShift);
}
/// Decode the tier carried in funcIndex's top 2 bits.
inline uint32_t decodeReqTier(uint32_t funcIndex) {
  return (funcIndex >> kEJitTierShift) & 0x3u;
}
/// Strip the tier bits to recover the real funcIndex (for cacheKey).
inline uint32_t stripReqTier(uint32_t funcIndex) {
  return funcIndex & ~kEJitTierMask;
}

//===----------------------------------------------------------------------===//
// EJitQueue
//
// Bounded multi-producer / single-consumer queue of EJitCompileRequest. The
// default backing is a Vyukov-style lock-free ring (no mutex, no condition
// variable). capacity() is rounded up to a power of two at construction.
//===----------------------------------------------------------------------===//
class EJitQueue {
public:
  /// \p capacity is rounded up to the next power of two (min 2).
  explicit EJitQueue(uint32_t capacity = EJIT_SRE_TASKPOOL_QUEUE_CAPACITY);
  ~EJitQueue();

  EJitQueue(const EJitQueue &) = delete;
  EJitQueue &operator=(const EJitQueue &) = delete;

  /// Enqueue a request. Returns false when the queue is full (producer side
  /// must then roll back any dedup reservation it made).
  bool push(const EJitCompileRequest &req);

  /// Dequeue a request into \p out. Returns false when the queue is empty.
  bool pop(EJitCompileRequest &out);

  /// Fixed power-of-two capacity.
  uint32_t capacity() const { return capacity_; }

  /// Best-effort element count (may be stale under concurrency).
  uint32_t approximateSize() const;

private:
  struct Cell {
    EJitAtomicU32 sequence;
    EJitCompileRequest data;
  };

  // The ring storage is sized at compile time from the queue-capacity macro so
  // it is a single fixed allocation (no std::vector). A capacity argument
  // smaller than the macro simply masks down into this storage.
  static constexpr uint32_t kRingSlots = EJIT_SRE_TASKPOOL_QUEUE_CAPACITY;
  static_assert((kRingSlots & (kRingSlots - 1)) == 0 && kRingSlots >= 2,
                "EJIT_SRE_TASKPOOL_QUEUE_CAPACITY must be a power of two >= 2");

  Cell buffer_[kRingSlots];
  uint32_t capacity_;
  uint32_t mask_;
  EJitAtomicU32 enqueuePos_;
  EJitAtomicU32 dequeuePos_;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSREQUEUE_H
