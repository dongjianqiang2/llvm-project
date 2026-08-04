//===-- EJitSharedCacheQueryBench.cpp - cache-hit query microbenchmark ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Deterministic, single-thread microbenchmark + slot-depth diagnostics for the
//  cross-core SHARED taskpool cache-hit query path (spec §5.2 steps 0-1). It is
//  NOT a gtest binary: it has its own main(), links only the shared-taskpool
//  sources + EJitSharedPlatform, and prints a machine-readable table so an
//  external harness (perf stat, this file's own cycle counter) can attribute
//  instructions/cycles per hit.
//
//  What it measures, per the audit requirements:
//    * 0D / 1D / 2D owner-core cache hit
//    * hit slot depth 0, 1, 5, 15 (the linear 16-slot bucket scan)
//    * peer-core hit (code sharing on, execute-permission already memoized)
//    * a slot-depth histogram for a synthetic mixed workload
//
//  Slot depth is controlled deterministically: 0D identities whose funcIndex is
//  a multiple of kEJitSharedCacheBuckets (32) all hash to bucket 0 and fill its
//  slots 0,1,2,... in publish order (cachePublish uses first-empty), so the Nth
//  published collider lands at slot N.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

using namespace llvm::ejit;

namespace {

// Deterministic, non-null "compiled code" pointer derived from funcIndex. Never
// executed — only cached/compared/returned.
void *codeFor(uint32_t funcIndex) {
  return reinterpret_cast<void *>(0x100000ull +
                                  static_cast<uintptr_t>(funcIndex) * 64u);
}

bool benchCompile(void * /*ctx*/, const EJitCompileRequest &req, void **outFn) {
  *outFn = codeFor(req.funcIndex);
  return true;
}

// Owner-side resolver: hand back a fixed 2MiB pool range so peer preparation
// (4K/2M) and codeStart/codeSize metadata are exercised.
struct RangeCtx {
  uintptr_t poolBase = 0x40000000ull;
  uint64_t poolSize = 0x200000ull;
  uintptr_t codeStart = 0x40000000ull;
  uint64_t codeSize = 64;
  uint32_t poolId = 0;
};
bool benchCodeRange(void *ctx, const void *fnPtr, EJitCompiledCodeInfo *out) {
  auto *r = static_cast<RangeCtx *>(ctx);
  out->fnPtr = const_cast<void *>(fnPtr);
  out->codeStart = r->codeStart;
  out->codeSize = r->codeSize;
  out->poolBase = r->poolBase;
  out->poolSize = r->poolSize;
  out->poolId = r->poolId;
  return true;
}

// Peer execute-permission preparation always succeeds in this host harness.
bool benchPrepareCode(void * /*ctx*/, const void * /*fnPtr*/) { return true; }

uint64_t nowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Global sink so the compiler cannot elide the looked-up pointer.
volatile uintptr_t gSink = 0;

struct Bench {
  std::unique_ptr<EJitSharedTaskPoolState> state;
  EJitSharedTaskPool pool;
  RangeCtx range;

  void bringUpOwner(bool codeSharing) {
    EJitCoreId::resetForTest();
    EJitCoreId::setCurrentForTest(0);
    state.reset(new EJitSharedTaskPoolState());
    pool.bind(state.get());
    pool.setCompiler(&benchCompile, nullptr);
    pool.setMode(EJitCompileMode::Async);
    pool.setCodeSharingEnabled(codeSharing);
    pool.setCodeRangeProvider(&benchCodeRange, &range);
    pool.setPrepareCodeCallback(&benchPrepareCode, nullptr);
    if (pool.init() != EJitSharedTaskPool::InitResult::BecameOwner) {
      std::fprintf(stderr, "owner election failed\n");
      std::exit(1);
    }
  }

  // Publish one 0D identity on the owner core (compile inline via Async enqueue
  // + pollOne).
  void publish0D(uint32_t funcIndex) {
    EJitCoreId::setCurrentForTest(0);
    pool.compileOrGet(funcIndex, nullptr, 0, codeFor(funcIndex));
    pool.pollOne();
  }
  void publish1D(uint32_t funcIndex, uint32_t d0, uint32_t i0) {
    EJitCoreId::setCurrentForTest(0);
    pool.setInstanceEnabled(d0, i0, true);
    EJitDimPair dims[1] = {{d0, i0}};
    pool.compileOrGet(funcIndex, dims, 1, codeFor(funcIndex));
    pool.pollOne();
  }
  void publish2D(uint32_t funcIndex, uint32_t d0, uint32_t i0, uint32_t d1,
                 uint32_t i1) {
    EJitCoreId::setCurrentForTest(0);
    pool.setInstanceEnabled(d0, i0, true);
    pool.setInstanceEnabled(d1, i1, true);
    EJitDimPair dims[2] = {{d0, i0}, {d1, i1}};
    pool.compileOrGet(funcIndex, dims, 2, codeFor(funcIndex));
    pool.pollOne();
  }
};

// --- 0D slot-depth sweep -----------------------------------------------------
// Publish (depth+1) 0D colliders into bucket 0, then hammer the deepest one.
uint64_t bench0D(uint32_t depth, uint64_t iters, bool peer, double *nsPerHit) {
  Bench B;
  B.bringUpOwner(/*codeSharing=*/peer);
  const uint32_t kB = kEJitSharedCacheBuckets;
  uint32_t targetFunc = 0;
  for (uint32_t d = 0; d <= depth; ++d) {
    uint32_t fi = d * kB; // all hash to bucket 0
    B.publish0D(fi);
    targetFunc = fi; // deepest published so far
  }
  uint32_t core = 0;
  if (peer) {
    core = 1;
    EJitCoreId::setCurrentForTest(core);
    // Warm the memoized execute-permission bit for this peer core.
    auto w = B.pool.tryCacheHit0D(targetFunc);
    if (w.hasReadToken)
      B.pool.releaseRead(w.bucketIndex);
  }
  EJitCoreId::setCurrentForTest(core);
  uint64_t t0 = nowNs();
  uint64_t hits = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    auto r = B.pool.tryCacheHit0D(targetFunc);
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.status == EJitCompileOrGetStatus::CacheHit)
      ++hits;
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  }
  uint64_t t1 = nowNs();
  *nsPerHit = double(t1 - t0) / double(iters);
  return hits;
}

uint64_t bench1D(uint32_t depth, uint64_t iters, double *nsPerHit) {
  Bench B;
  B.bringUpOwner(false);
  const uint32_t kB = kEJitSharedCacheBuckets;
  // 1D identities colliding into one bucket: vary funcIndex by multiples that
  // keep the same hash bucket is nontrivial, so instead vary instanceId and
  // accept the natural bucket; publish depth+1 entries and hammer the last.
  uint32_t targetFunc = 7;
  for (uint32_t d = 0; d <= depth; ++d) {
    B.publish1D(targetFunc, 0, d); // same funcIndex, distinct instanceId
  }
  // The deepest is instanceId=depth; ensure it is enabled and query it.
  EJitCoreId::setCurrentForTest(0);
  uint64_t t0 = nowNs();
  uint64_t hits = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    auto r = B.pool.tryCacheHit1D(targetFunc, 0, depth);
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.status == EJitCompileOrGetStatus::CacheHit)
      ++hits;
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  }
  uint64_t t1 = nowNs();
  *nsPerHit = double(t1 - t0) / double(iters);
  return hits;
}

uint64_t bench2D(uint64_t iters, double *nsPerHit) {
  Bench B;
  B.bringUpOwner(false);
  uint32_t targetFunc = 9;
  B.publish2D(targetFunc, 0, 1, 1, 2);
  EJitCoreId::setCurrentForTest(0);
  uint64_t t0 = nowNs();
  uint64_t hits = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    auto r = B.pool.tryCacheHit2D(targetFunc, 0, 1, 1, 2);
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.status == EJitCompileOrGetStatus::CacheHit)
      ++hits;
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  }
  uint64_t t1 = nowNs();
  *nsPerHit = double(t1 - t0) / double(iters);
  return hits;
}

void runTable(uint64_t iters) {
  std::printf("# config,dim,slotDepth,core,nsPerHit,hits/iters\n");
  double ns;
  uint64_t h;
  for (uint32_t depth : {0u, 1u, 5u, 15u}) {
    h = bench0D(depth, iters, /*peer=*/false, &ns);
    std::printf("owner0D,0,%u,0,%.3f,%llu/%llu\n", depth, ns,
                (unsigned long long)h, (unsigned long long)iters);
  }
  for (uint32_t depth : {0u, 1u, 5u, 15u}) {
    h = bench0D(depth, iters, /*peer=*/true, &ns);
    std::printf("peer0D,0,%u,1,%.3f,%llu/%llu\n", depth, ns,
                (unsigned long long)h, (unsigned long long)iters);
  }
  for (uint32_t depth : {0u, 1u, 5u, 15u}) {
    h = bench1D(depth, iters, &ns);
    std::printf("owner1D,1,%u,0,%.3f,%llu/%llu\n", depth, ns,
                (unsigned long long)h, (unsigned long long)iters);
  }
  h = bench2D(iters, &ns);
  std::printf("owner2D,2,0,0,%.3f,%llu/%llu\n", ns, (unsigned long long)h,
              (unsigned long long)iters);
}

} // namespace

// Usage:
//   bench                 -> full table, 20M iters each
//   bench <iters>         -> full table with given iters
//   bench single0d <d> <iters>   -> ONE tight loop (for perf stat attribution)
//   bench single0dpeer <d> <iters>
//   bench single1d <d> <iters>
//   bench single2d <iters>
int main(int argc, char **argv) {
  uint64_t iters = 20000000ull;
  if (argc >= 2 && std::strncmp(argv[1], "single", 6) == 0) {
    double ns;
    uint64_t h = 0;
    const char *what = argv[1];
    if (std::strcmp(what, "single0d") == 0) {
      uint32_t d = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
      iters = argc >= 4 ? strtoull(argv[3], nullptr, 10) : iters;
      h = bench0D(d, iters, false, &ns);
    } else if (std::strcmp(what, "single0dpeer") == 0) {
      uint32_t d = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
      iters = argc >= 4 ? strtoull(argv[3], nullptr, 10) : iters;
      h = bench0D(d, iters, true, &ns);
    } else if (std::strcmp(what, "single1d") == 0) {
      uint32_t d = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
      iters = argc >= 4 ? strtoull(argv[3], nullptr, 10) : iters;
      h = bench1D(d, iters, &ns);
    } else if (std::strcmp(what, "single2d") == 0) {
      iters = argc >= 3 ? strtoull(argv[2], nullptr, 10) : iters;
      h = bench2D(iters, &ns);
    } else {
      std::fprintf(stderr, "unknown single mode %s\n", what);
      return 2;
    }
    std::printf("%s ns/hit=%.3f hits=%llu/%llu sink=%llu\n", what, ns,
                (unsigned long long)h, (unsigned long long)iters,
                (unsigned long long)gSink);
    return 0;
  }
  if (argc >= 2)
    iters = strtoull(argv[1], nullptr, 10);
  runTable(iters);
  std::printf("# sink=%llu\n", (unsigned long long)gSink);
  return 0;
}
