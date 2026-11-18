//===-- EJitTaskPoolTypesTest.cpp - Taskpool protocol and registry tests --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitTaskPoolTypes.h"
#include "gtest/gtest.h"
#include <string>
#include <type_traits>

using namespace llvm;
using namespace llvm::ejit;

//===----------------------------------------------------------------------===//
// Request layout
//===----------------------------------------------------------------------===//

TEST(EJitTaskPoolLayout, RequestIsFlatPod) {
  static_assert(std::is_trivially_copyable<EJitCompileRequest>::value,
                "EJitCompileRequest must be trivially copyable");
  static_assert(std::is_standard_layout<EJitCompileRequest>::value,
                "EJitCompileRequest must be standard layout");
  EXPECT_LE(alignof(EJitCompileRequest), 8u);
  // The fixed request carries descriptors only; its size is independent of
  // every pointee's payload size. See the cross-pointer-width assertion in
  // EJitTaskPoolTypes.h.
  EXPECT_EQ(sizeof(EJitCompileRequest), sizeof(uintptr_t) == 8 ? 200u : 164u);
}

//===----------------------------------------------------------------------===//
// EJitAtomic
//===----------------------------------------------------------------------===//

TEST(EJitAtomicTest, U8CompareExchange) {
  EJitAtomicU8 A(0);
  uint8_t expected = 0;
  EXPECT_TRUE(A.compareExchange(expected, 1));
  EXPECT_EQ(A.loadAcquire(), 1u);
  expected = 0; // wrong expectation
  EXPECT_FALSE(A.compareExchange(expected, 2));
  EXPECT_EQ(expected, 1u); // updated to observed
  EXPECT_EQ(A.loadAcquire(), 1u);
}

TEST(EJitAtomicTest, U32LoadStoreAndArith) {
  EJitAtomicU32 A(5);
  EXPECT_EQ(A.loadAcquire(), 5u);
  A.storeRelease(8);
  EXPECT_EQ(A.fetchAdd(2), 8u);
  EXPECT_EQ(A.fetchSub(1), 10u);
  EXPECT_EQ(A.loadRelaxed(), 9u);
}

TEST(EJitAtomicTest, PointerRoundtrip) {
  EJitAtomicUPtr A(0);
  uintptr_t P = reinterpret_cast<uintptr_t>(&A);
  A.storeRelease(P);
  EXPECT_EQ(A.loadAcquire(), P);
}

TEST(EJitFuncRegistry, DenseDistinctIdempotentOrderIndependent) {
  auto &R = EJitFuncRegistry::instance();
  R.reset();
  // Dense, distinct, idempotent.
  uint32_t a = R.resolveAssign("a_func");
  uint32_t b = R.resolveAssign("b_func");
  EXPECT_NE(a, b);
  EXPECT_EQ(R.resolveAssign("a_func"), a); // idempotent
  EXPECT_EQ(R.lookup("b_func"), b);
  EXPECT_EQ(R.lookup("never"), kEJitInvalidFuncIndex);
  EXPECT_LT(a, kEJitMaxFuncIndex);
  EXPECT_LT(b, kEJitMaxFuncIndex);
  // Registering a NEW name never shifts an existing index.
  uint32_t c = R.resolveAssign("c_func");
  EXPECT_EQ(R.lookup("a_func"), a);
  EXPECT_EQ(R.lookup("b_func"), b);
  EXPECT_NE(c, a);
  EXPECT_NE(c, b);
  R.reset();
}

TEST(EJitFuncRegistry, CrossModuleSameNameSameIndexEitherOrder) {
  // Two registration sites (modules) in one process, opposite orders: each name
  // keeps a single stable dense index — the cross-module funcIndex agreement.
  auto &R = EJitFuncRegistry::instance();
  R.reset();
  uint32_t fAlpha = R.resolveAssign("alpha"); // module A first
  uint32_t fOmega = R.resolveAssign("omega");
  EXPECT_EQ(R.resolveAssign("omega"), fOmega); // module B, omega first
  EXPECT_EQ(R.resolveAssign("alpha"), fAlpha);
  EXPECT_NE(fAlpha, fOmega);
  R.reset();
}

TEST(EJitFuncRegistry, CapacityExhaustionRejectsCleanly) {
  auto &R = EJitFuncRegistry::instance();
  R.reset();
  for (uint32_t i = 0; i < kEJitMaxFuncIndex; ++i)
    ASSERT_EQ(R.resolveAssign("f" + std::to_string(i)), i);
  EXPECT_EQ(R.count(), kEJitMaxFuncIndex);
  // The next NEW name is rejected (invalid), never aliased onto an existing
  // one.
  EXPECT_EQ(R.resolveAssign("overflow"), kEJitInvalidFuncIndex);
  EXPECT_EQ(R.lookup("overflow"), kEJitInvalidFuncIndex);
  // An already-assigned name still resolves (idempotent) even when full.
  EXPECT_EQ(R.resolveAssign("f0"), 0u);
  R.reset();
}

TEST(EJitLifecycleRegistry, DenseAssignmentIdempotentAndLookup) {
  auto &R = EJitLifecycleRegistry::instance();
  R.reset();
  EXPECT_EQ(R.resolveAssign("cell"), 0u);
  EXPECT_EQ(R.resolveAssign("trp"), 1u);
  EXPECT_EQ(R.resolveAssign("cell"), 0u); // idempotent, no new slot
  EXPECT_EQ(R.count(), 2u);
  EXPECT_EQ(R.lookup("trp"), 1u);
  EXPECT_EQ(R.lookup("never-registered"), kEJitInvalidDimType);
  R.reset();
}

// The exact cross-module hazard: "cell" and "tenant" hash to the SAME 8-slot
// bucket (fnv1a%8 == 5 for both), which is why dimType cannot be a name hash.
// The registry hands them DISTINCT slots.
TEST(EJitLifecycleRegistry, HashCollidingNamesGetDistinctSlots) {
  auto &R = EJitLifecycleRegistry::instance();
  R.reset();
  uint32_t cell = R.resolveAssign("cell");
  uint32_t tenant = R.resolveAssign("tenant");
  EXPECT_NE(cell, tenant);
  EXPECT_NE(cell, kEJitInvalidDimType);
  EXPECT_NE(tenant, kEJitInvalidDimType);
  R.reset();
}

// Two modules (different registration sites in one process) that name the SAME
// lifecycle observe the SAME slot, in either registration order — the
// cross-module dimType agreement the fix guarantees.
TEST(EJitLifecycleRegistry, CrossModuleSameNameSameSlot) {
  auto &R = EJitLifecycleRegistry::instance();
  R.reset();
  uint32_t aCell = R.resolveAssign("cell"); // module A
  uint32_t aTrp = R.resolveAssign("trp");   // module A
  uint32_t bTrp = R.resolveAssign("trp");   // module B, registered later
  uint32_t bCell = R.resolveAssign("cell"); // module B
  EXPECT_EQ(aTrp, bTrp);
  EXPECT_EQ(aCell, bCell);
  EXPECT_NE(aCell, aTrp);
  R.reset();
}

// The 9th distinct lifecycle is rejected (kEJitInvalidDimType), never aliased
// onto an existing slot.
TEST(EJitLifecycleRegistry, NinthLifecycleRejected) {
  auto &R = EJitLifecycleRegistry::instance();
  R.reset();
  for (uint32_t i = 0; i < kEJitMaxDimTypes; ++i)
    EXPECT_EQ(R.resolveAssign("lc" + std::to_string(i)), i);
  EXPECT_EQ(R.count(), kEJitMaxDimTypes);
  EXPECT_EQ(R.resolveAssign("ninth"), kEJitInvalidDimType);
  EXPECT_EQ(R.lookup("ninth"), kEJitInvalidDimType);
  EXPECT_EQ(R.count(), kEJitMaxDimTypes); // unchanged
  R.reset();
}
