//===-- EJitVerifySiteTest.cpp - verifier site identity tests
//---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/ExecutionEngine/EJIT/EJitVerify.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

class VerifySiteTest : public testing::Test {
protected:
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  PeriodArrayRegistry Registry;
  uint32_t Left[2] = {3, 7}, Right[2] = {3, 9};
  void *LeftPtr = Left, *RightPtr = Right;

  bool parse(StringRef IR) {
    SMDiagnostic Err;
    M = parseAssemblyString(IR, Err, Ctx);
    if (!M) {
      Err.print("VerifySiteTest", errs());
      return false;
    }
    M->setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");
    return true;
  }

  SmallVector<std::string> names(Function &F) {
    SmallVector<std::string> Result;
    for (Instruction &I : instructions(F)) {
      auto *Call = dyn_cast<CallInst>(&I);
      if (!Call || !Call->getCalledFunction() ||
          Call->getCalledFunction()->getName() != "__ejit_verify_check")
        continue;
      StringRef Name;
      EXPECT_TRUE(getConstantStringInfo(Call->getArgOperand(0), Name));
      EXPECT_LT(Name.size(), kVerifySiteNameMax);
      Result.push_back(Name.str());
    }
    return Result;
  }

  SmallVector<std::string> runBound() {
    SmallVector<EJitBoundPointerView> Views = {
        {reinterpret_cast<const uint8_t *>(Left), sizeof(Left), 1, 0},
        {reinterpret_cast<const uint8_t *>(Right), sizeof(Right), 2, 0}};
    EJitStructFieldPass Pass(Registry, Views, "bound", true);
    Pass.initFromModule(*M);
    Function &F = *M->getFunction("bound");
    FunctionAnalysisManager FAM;
    Pass.run(F, FAM);
    EXPECT_FALSE(verifyModule(*M, &errs()));
    auto First = names(F);
    Pass.run(F, FAM);
    EXPECT_EQ(names(F), First) << "second pass must not instrument twice";
    return First;
  }

  bool boundModule() {
    return parse(R"(
      define i32 @bound(i32 %cell, ptr %left, ptr %right) !ejit.metadata !0 {
        %lp = getelementptr i32, ptr %left, i64 1
        %l = load i32, ptr %lp
        %rp = getelementptr i32, ptr %right, i64 1
        %r = load i32, ptr %rp
        %sum = add i32 %l, %r
        ret i32 %sum
      }
      !0 = !{!1, !2, !3, !4}
      !1 = !{!"ejit_entry"}
      !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
      !3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 8, !5}
      !4 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 8, !5}
      !5 = !{i64 0, i64 8}
    )");
  }
};

TEST_F(VerifySiteTest, BoundArgumentsAtSameOffsetStaySeparate) {
  ASSERT_TRUE(boundModule());
  auto Names = runBound();
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_EQ(Names[0], "bound:arg1(left)+4");
  EXPECT_EQ(Names[1], "bound:arg2(right)+4");

  ejitVerifyResetStats();
  __ejit_verify_check(Names[0].c_str(), 7, 7);
  __ejit_verify_check(Names[1].c_str(), 9, 10);
  ASSERT_EQ(ejitVerifySiteCount(), 2u);
  VerifySite A, B;
  ASSERT_TRUE(ejitVerifyGetSite(0, &A));
  ASSERT_TRUE(ejitVerifyGetSite(1, &B));
  EXPECT_EQ(A.mismatches, 0u);
  EXPECT_EQ(B.mismatches, 1u);
  ejitVerifyResetStats();
}

TEST_F(VerifySiteTest, PointerGlobalsSurviveBaseMaterialization) {
  ASSERT_TRUE(parse(R"(
    @left = external global ptr, !ejit.metadata !0
    @right = external global ptr, !ejit.metadata !1
    define i32 @probe() {
      %lp = load ptr, ptr @left
      %lf = getelementptr i32, ptr %lp, i64 1
      %l = load i32, ptr %lf, !ejit.may_const !5
      %rp = load ptr, ptr @right
      %rf = getelementptr i32, ptr %rp, i64 1
      %r = load i32, ptr %rf, !ejit.may_const !5
      %sum = add i32 %l, %r
      ret i32 %sum
    }
    !0 = !{!2, !4}
    !1 = !{!3, !4}
    !2 = !{!"ejit_period", !"left"}
    !3 = !{!"ejit_period", !"right"}
    !4 = !{!"ejit_may_const_field", i64 4}
    !5 = !{}
  )"));
  Registry.registerStaticVar("left", &LeftPtr);
  Registry.registerStaticVar("right", &RightPtr);
  EJitStructFieldPass Pass(Registry, nullptr, 0, 0, true);
  Pass.initFromModule(*M);
  FunctionAnalysisManager FAM;
  Function &F = *M->getFunction("probe");
  Pass.run(F, FAM);
  auto Names = names(F);
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_EQ(Names[0], "probe:*left->+4");
  EXPECT_EQ(Names[1], "probe:*right->+4");
  Pass.run(F, FAM);
  EXPECT_EQ(names(F), Names);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(VerifySiteTest, PointerArraySlotsAtSameFieldOffsetStaySeparate) {
  ASSERT_TRUE(parse(R"(
    @ptrs = external global [2 x ptr], !ejit.metadata !0
    define i32 @probe() {
      %lp = load ptr, ptr @ptrs
      %lf = getelementptr i32, ptr %lp, i64 1
      %l = load i32, ptr %lf, !ejit.may_const !3
      %slot = getelementptr [2 x ptr], ptr @ptrs, i64 0, i64 1
      %rp = load ptr, ptr %slot
      %rf = getelementptr i32, ptr %rp, i64 1
      %r = load i32, ptr %rf, !ejit.may_const !3
      %sum = add i32 %l, %r
      ret i32 %sum
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 2}
    !3 = !{}
  )"));
  void *Slots[] = {Left, Right};
  Registry.registerArray("cell", "ptrs", Slots, 2);
  EJitStructFieldPass Pass(Registry, nullptr, 0, 0, true);
  Pass.initFromModule(*M);
  FunctionAnalysisManager FAM;
  Function &F = *M->getFunction("probe");
  Pass.run(F, FAM);
  auto Names = names(F);
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_EQ(Names[0], "probe:*ptrs+0->+4");
  EXPECT_EQ(Names[1], "probe:*ptrs+8->+4");
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(VerifySiteTest, AmbiguousPointerRegistrationsUseAddress) {
  ASSERT_TRUE(parse(R"(
    @left = external global ptr, !ejit.metadata !0
    @alias = external global ptr, !ejit.metadata !1
    define i32 @probe() {
      %p = load ptr, ptr @left
      %field = getelementptr i32, ptr %p, i64 1
      %value = load i32, ptr %field, !ejit.may_const !5
      ret i32 %value
    }
    !0 = !{!2, !4}
    !1 = !{!3, !4}
    !2 = !{!"ejit_period", !"left"}
    !3 = !{!"ejit_period", !"alias"}
    !4 = !{!"ejit_may_const_field", i64 4}
    !5 = !{}
  )"));
  Registry.registerStaticVar("left", &LeftPtr);
  Registry.registerStaticVar("alias", &LeftPtr);
  EJitStructFieldPass Pass(Registry, nullptr, 0, 0, true);
  Pass.initFromModule(*M);
  FunctionAnalysisManager FAM;
  Function &F = *M->getFunction("probe");
  Pass.run(F, FAM);
  auto Names = names(F);
  ASSERT_EQ(Names.size(), 1u);
  EXPECT_EQ(Names[0].find("probe:addr=0x"), 0u);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(VerifySiteTest, LongCommonPrefixesRetainDifferentIdentities) {
  ASSERT_TRUE(boundModule());
  Function &F = *M->getFunction("bound");
  F.getArg(1)->setName(std::string(100, 'x') + "left");
  // Same long root, different field offsets AFTER the truncation boundary.
  auto *OtherGEP =
      cast<GetElementPtrInst>(F.getValueSymbolTable()->lookup("rp"));
  OtherGEP->setOperand(0, F.getArg(1));
  OtherGEP->setOperand(1, ConstantInt::get(Type::getInt64Ty(Ctx), 0));
  auto Names = runBound();
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_NE(Names[0], Names[1]);
  EXPECT_NE(Names[0].find('#'), std::string::npos);
  EXPECT_NE(Names[1].find('#'), std::string::npos);
  EXPECT_EQ(Names[0].substr(0, Names[0].find('#')),
            Names[1].substr(0, Names[1].find('#')));
}

TEST_F(VerifySiteTest, SourceLocationWhenAvailable) {
  ASSERT_TRUE(boundModule());
  DIBuilder Debug(*M);
  DIFile *File = Debug.createFile("settings.c", "/src");
  Debug.createCompileUnit(dwarf::DW_LANG_C, File, "test", false, "", 0);
  auto *SP = Debug.createFunction(
      File, "bound", "bound", File, 1,
      Debug.createSubroutineType(Debug.getOrCreateTypeArray({})), 1,
      DINode::FlagZero, DISubprogram::SPFlagDefinition);
  Function &F = *M->getFunction("bound");
  F.setSubprogram(SP);
  for (Instruction &I : instructions(F))
    I.setDebugLoc(DILocation::get(Ctx, 42, 7, SP));
  Debug.finalize();
  auto Names = runBound();
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_EQ(Names[0], "bound:arg1(left)+4@settings.c:42:7");
  EXPECT_EQ(Names[1], "bound:arg2(right)+4@settings.c:42:7");
}

} // namespace
