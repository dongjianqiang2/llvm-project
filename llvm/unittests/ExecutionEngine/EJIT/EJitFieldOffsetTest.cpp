//===-- EJitFieldOffsetTest.cpp - ejitMayConstFieldOffset unit tests ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ejitMayConstFieldOffset decides *which field* a load names, and that answer is
// the only gate on whether the load is treated as may_const. Getting it wrong is
// silent: the generated code stays correct-looking while either losing a
// specialization (offset not matched) or folding a field that is free to change
// (offset matched for the wrong field). Neither shows up as a test failure
// anywhere else, so the accepted and rejected GEP shapes are pinned here.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// %S is 7 x i32 = 28 bytes, so field offsets are 0,4,8,12,16,20,24 and the
/// period array @g has an element stride of 28. Field 4 (offset 16) is the
/// one every accepted shape below must resolve to.
constexpr uint64_t kElemSize = 28;
constexpr uint64_t kField4Offset = 16;

class FieldOffsetTest : public testing::Test {
protected:
  LLVMContext Ctx;
  std::unique_ptr<Module> M;

  /// Parse \p Body as the entire body of `@f(i64 %ci, i64 %j)`.
  ///
  ///   %S     7 x i32, 28 bytes -- the period element
  ///   %N/%O  a nested record, whose first field shares offset 0 with it
  ///   @g1    [16 x [1 x i32]]  -- element and its own element are both 4 bytes,
  ///                               so two dynamic indices can both satisfy the
  ///                               stride test
  bool parse(StringRef Body) {
    std::string Src = R"(
      target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
      %S = type { i32, i32, i32, i32, i32, i32, i32 }
      %N = type { i32, i32 }
      %O = type { %N, i32 }
      @g = external global [16 x %S]
      @scalar = external global %S
      @outer = external global [4 x %O]
      @g1 = external global [16 x [1 x i32]]
      define void @f(i64 %ci, i64 %j) {
      )" + Body.str() + R"(
        ret void
      }
    )";
    SMDiagnostic Err;
    M = parseAssemblyString(Src, Err, Ctx);
    if (!M)
      Err.print("FieldOffsetTest", errs());
    return M != nullptr;
  }

  const LoadInst *firstLoad() {
    for (Instruction &I : instructions(M->getFunction("f")))
      if (auto *LI = dyn_cast<LoadInst>(&I))
        return LI;
    return nullptr;
  }

  std::optional<uint64_t> offsetOf(StringRef Body,
                                   const GlobalVariable **OutGV = nullptr) {
    if (!parse(Body))
      return std::nullopt;
    const LoadInst *LI = firstLoad();
    EXPECT_NE(LI, nullptr);
    if (!LI)
      return std::nullopt;
    const GlobalVariable *GV = nullptr;
    auto Off =
        ejitMayConstFieldOffset(LI->getPointerOperand(), M->getDataLayout(), GV);
    if (OutGV)
      *OutGV = GV;
    return Off;
  }

  /// A module with only the globals, for the layout-only helpers.
  void parseGlobalsOnly() { ASSERT_TRUE(parse("%v = load i32, ptr @g")); }

  std::optional<uint64_t> fieldSizeAt(StringRef GVName, uint64_t Off) {
    parseGlobalsOnly();
    const GlobalVariable *GV = M->getNamedGlobal(GVName);
    EXPECT_NE(GV, nullptr);
    return ejitMayConstFieldSize(GV, Off, M->getDataLayout());
  }

  bool accessFits(StringRef GVName, uint64_t Off, uint64_t AccessSize) {
    parseGlobalsOnly();
    const GlobalVariable *GV = M->getNamedGlobal(GVName);
    EXPECT_NE(GV, nullptr);
    return ejitAccessFitsMayConstField(GV, Off, AccessSize,
                                       M->getDataLayout());
  }
};

//===----------------------------------------------------------------------===//
// Accepted shapes: every one names field 4 and must yield its element-relative
// offset, independent of which element is selected.
//===----------------------------------------------------------------------===//

TEST_F(FieldOffsetTest, DynamicElementIndex) {
  // g[ci].f4 -- the AOT shape, before the JIT substitutes ci.
  const GlobalVariable *GV = nullptr;
  auto Off = offsetOf(R"(
    %p = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci, i32 4
    %v = load i32, ptr %p
  )", &GV);
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset);
  ASSERT_NE(GV, nullptr);
  EXPECT_EQ(GV->getName(), "g");
}

TEST_F(FieldOffsetTest, ConstantElementIndexReducesModuloStride) {
  // g[3].f4 -- total offset 3*28 + 16 = 100, which must reduce to 16.
  auto Off = offsetOf(R"(
    %p = getelementptr [16 x %S], ptr @g, i64 0, i64 3, i32 4
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset);
  EXPECT_EQ(3 * kElemSize + kField4Offset, 100u); // the offset that used to miss
}

TEST_F(FieldOffsetTest, FlatByteGEP) {
  // What InstCombine leaves behind once the index is a constant: @g + 156.
  auto Off = offsetOf(R"(
    %p = getelementptr i8, ptr @g, i64 156
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset); // 156 % 28
}

TEST_F(FieldOffsetTest, DecayedArrayToPointer) {
  // &g[ci] with the array decayed: the element selector is the only index.
  auto Off = offsetOf(R"(
    %p = getelementptr %S, ptr @g, i64 %ci
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, 0u); // names field 0
}

TEST_F(FieldOffsetTest, LoadDirectlyFromGlobal) {
  auto Off = offsetOf(R"(
    %v = load i32, ptr @g
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, 0u);
}

TEST_F(FieldOffsetTest, ChainedConstantByteGEPsInsideElement) {
  // g[ci] then +16: the shape clang emits after canonicalizing field GEPs.
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci
    %p = getelementptr i8, ptr %e, i64 16
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset);
}

TEST_F(FieldOffsetTest, NonArrayGlobalKeepsStructOffset) {
  // A scalar `ejit_period` global: no array, so no modulo is applied.
  auto Off = offsetOf(R"(
    %p = getelementptr %S, ptr @scalar, i32 0, i32 4
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset);
}

//===----------------------------------------------------------------------===//
// Rejected shapes. Each of these could otherwise resolve to a plausible field
// offset and cause a load the frontend never marked to be annotated may_const.
//===----------------------------------------------------------------------===//

TEST_F(FieldOffsetTest, RejectsTypedPointerDynamicIndex) {
  // ((int *)&g[ci])[j] -- %j walks *within* an element with stride 4, not 28.
  // Skipping it would resolve to offset 0 and wrongly claim field 0, letting the
  // JIT later fold a field that is free to change.
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci
    %p = getelementptr i32, ptr %e, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsDynamicByteOffset) {
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci
    %p = getelementptr i8, ptr %e, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsDynamicIndexNotRootedAtGlobal) {
  // Same stride as the element, but applied to a derived pointer rather than to
  // the global, so it is not the element selector.
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 3
    %p = getelementptr %S, ptr %e, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsTwoDynamicIndices) {
  auto Off = offsetOf(R"(
    %p = getelementptr [16 x %S], ptr @g, i64 %j, i64 %ci
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsNegativeConstantIndex) {
  // getZExtValue() would turn -4 into 2^64-4, wrap the accumulator, and leave a
  // plausible-looking field offset behind.
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 3
    %p = getelementptr i8, ptr %e, i64 -4
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsDynamicIndexOnNonArrayGlobal) {
  // No element stride exists, so nothing may be skipped.
  auto Off = offsetOf(R"(
    %p = getelementptr %S, ptr @scalar, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsPointerNotRootedAtGlobal) {
  auto Off = offsetOf(R"(
    %a = alloca %S
    %p = getelementptr %S, ptr %a, i32 0, i32 4
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsTwoQualifyingDynamicIndices) {
  // An array has exactly one element selector. @g1's element ([1 x i32]) and its
  // own element (i32) are both 4 bytes, so BOTH dynamic indices pass the
  // stride test on the root GEP. Only the first may be skipped.
  auto Off = offsetOf(R"(
    %p = getelementptr [1 x i32], ptr @g1, i64 %ci, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

//===----------------------------------------------------------------------===//
// Access-width containment. The metadata records a field's offset and nothing
// else, so an offset match alone cannot bound how many bytes an access may read.
//===----------------------------------------------------------------------===//

TEST_F(FieldOffsetTest, FieldSizeIsRecoveredFromLayout) {
  EXPECT_EQ(fieldSizeAt("g", 0), std::optional<uint64_t>(4));  // %S.0 : i32
  EXPECT_EQ(fieldSizeAt("g", 16), std::optional<uint64_t>(4)); // %S.4 : i32
  EXPECT_EQ(fieldSizeAt("scalar", 8), std::optional<uint64_t>(4));
}

TEST_F(FieldOffsetTest, FieldSizeDescendsIntoNestedRecord) {
  // %O = { %N, i32 } and %N = { i32, i32 }: offset 0 names %N (8 bytes) *and*
  // its first field (4 bytes). The innermost one bounds a may_const access, so
  // an i64 load at offset 0 must not be treated as reading one field.
  EXPECT_EQ(fieldSizeAt("outer", 0), std::optional<uint64_t>(4));
  EXPECT_EQ(fieldSizeAt("outer", 4), std::optional<uint64_t>(4));
  EXPECT_EQ(fieldSizeAt("outer", 8), std::optional<uint64_t>(4));
}

TEST_F(FieldOffsetTest, FieldSizeRejectsOffsetInsideAField) {
  EXPECT_FALSE(fieldSizeAt("g", 2).has_value());  // mid-field
  EXPECT_FALSE(fieldSizeAt("g", 28).has_value()); // past the element
}

TEST_F(FieldOffsetTest, AcceptsAccessNoWiderThanTheField) {
  EXPECT_TRUE(accessFits("g", 16, 4)); // i32 load of the i32 field
  EXPECT_TRUE(accessFits("g", 16, 1)); // narrower access stays inside
}

//===----------------------------------------------------------------------===//
// Volatile / atomic. The offset list records a may_const field regardless of
// volatility, so an offset match must not resurrect a marker the frontend
// deliberately withheld. ejitMayConstFieldOffset is a pure pointer computation
// and stays width- and volatility-agnostic; the callers own that rejection, and
// these pin the pointer arithmetic they rely on.
//===----------------------------------------------------------------------===//

TEST_F(FieldOffsetTest, OffsetIsComputedForVolatileAndAtomicLoadsToo) {
  // The helper still resolves the field; it is reAnnotateMayConst and
  // isMayConstLoad that must refuse to act on the result.
  auto VolOff = offsetOf(R"(
    %p = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci, i32 4
    %v = load volatile i32, ptr %p
  )");
  ASSERT_TRUE(VolOff.has_value());
  EXPECT_EQ(*VolOff, kField4Offset);

  auto AtomOff = offsetOf(R"(
    %p = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci, i32 4
    %v = load atomic i32, ptr %p monotonic, align 4
  )");
  ASSERT_TRUE(AtomOff.has_value());
  EXPECT_EQ(*AtomOff, kField4Offset);
}

TEST_F(FieldOffsetTest, RejectsWideLoadOverlappingAdjacentField) {
  // The case that matters: an 8-byte access starting at a 4-byte may_const field
  // also covers the field beside it, which is free to change. Substituting it
  // would freeze both.
  EXPECT_FALSE(accessFits("g", 16, 8));
  EXPECT_FALSE(accessFits("outer", 0, 8)); // spans %N.0 and %N.1
  EXPECT_FALSE(accessFits("g", 16, 0));    // degenerate width
}

} // namespace
