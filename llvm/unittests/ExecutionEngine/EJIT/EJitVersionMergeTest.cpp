//===-- EJitVersionMergeTest.cpp - Final-version switch assembly
//------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_SWITCH_CASE

#include "llvm/ExecutionEngine/EJIT/EJitVersionMerge.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {
constexpr const char *IR = R"(
declare i32 @merge_aot(i32, i32, i32, ptr)
define internal i32 @arm_a(i32 %cell, i32 %trp, i32 %x, ptr %out) {
  %c = mul i32 %cell, 100
  %t = mul i32 %trp, 10
  %a = add i32 %x, 7
  %b = add i32 %a, %c
  %r = add i32 %b, %t
  store i32 %r, ptr %out
  ret i32 %r
}
define internal i32 @arm_b(i32 %cell, i32 %trp, i32 %x, ptr %out) {
  %c = mul i32 %cell, 100
  %t = mul i32 %trp, 10
  %a = add i32 %x, 11
  %b = add i32 %a, %c
  %r = add i32 %b, %t
  store i32 %r, ptr %out
  ret i32 %r
}
)";

std::unique_ptr<Module> parse(LLVMContext &Ctx, StringRef Text = IR) {
  SMDiagnostic Diag;
  auto M = parseAssemblyString(Text, Diag, Ctx);
  EXPECT_TRUE(M) << Diag.getMessage().str();
  return M;
}

SmallVector<EJitFinalMergeArm, 2> arms(Module &M) {
  return {{M.getFunction("arm_a"), {0, 2}}, {M.getFunction("arm_b"), {3, 5}}};
}

extern "C" uint32_t mergeAot(uint32_t Cell, uint32_t Trp, uint32_t X,
                             uint32_t *Out) {
  *Out = Cell * 100u + Trp * 10u + X + 1000u;
  return *Out;
}
} // namespace

TEST(EJitVersionMerge, UnequalConfigurationsHaveOneBodyNotMemberCalls) {
  LLVMContext Ctx;
  auto M = parse(Ctx);
  auto Arms = arms(*M);
  auto Result = assembleFinalVersionSwitch(
      "merged", *M->getFunction("merge_aot"), {0, 1}, Arms);
  ASSERT_TRUE(bool(Result)) << toString(Result.takeError());
  unsigned Switches = 0, Stores = 0, AotCalls = 0;
  for (Instruction &I : instructions(**Result)) {
    Switches += isa<SwitchInst>(I);
    Stores += isa<StoreInst>(I);
    if (auto *Call = dyn_cast<CallBase>(&I)) {
      EXPECT_EQ(Call->getCalledFunction(), M->getFunction("merge_aot"));
      ++AotCalls;
    }
  }
  EXPECT_EQ(Switches, 3u);
  EXPECT_EQ(Stores, 2u);
  EXPECT_EQ(AotCalls, 1u);
  EXPECT_FALSE(verifyModule(*M, &errs()));
  EXPECT_FALSE(M->getFunction("arm_a")->empty());
  EXPECT_FALSE(M->getFunction("arm_b")->empty());
}

TEST(EJitVersionMerge, OneNativeEntryExecutesBothArmsAndExactTupleFallback) {
  LLVMContext Ctx;
  auto M = parse(Ctx);
  auto Arms = arms(*M);
  auto Result = assembleFinalVersionSwitch(
      "merged", *M->getFunction("merge_aot"), {0, 1}, Arms);
  ASSERT_TRUE(bool(Result)) << toString(Result.takeError());
  // The assembler does not own inputs. A preparation owner can discard them
  // after successful assembly: emitted code then contains no member entries.
  for (auto &Arm : Arms) {
    ASSERT_TRUE(Arm.body->use_empty());
    Arm.body->eraseFromParent();
  }
  unsigned Definitions = 0;
  for (Function &F : *M)
    Definitions += !F.isDeclaration();
  EXPECT_EQ(Definitions, 1u);
  std::string Bitcode;
  raw_string_ostream OS(Bitcode);
  WriteBitcodeToFile(*M, OS);
  OS.flush();
  EJitRuntimeState Runtime;
  Config Cfg;
  auto Engine = EJitOrcEngine::Create(Cfg, Runtime.getRegistry(), Runtime);
  ASSERT_TRUE(bool(Engine)) << toString(Engine.takeError());
  (*Engine)->addUserSymbol("merge_aot", reinterpret_cast<void *>(&mergeAot));
  Error Err = (*Engine)->loadBitcodeModule(Bitcode, 777, "merged");
  ASSERT_FALSE(bool(Err)) << toString(std::move(Err));
  auto Address = (*Engine)->lookup(777, "merged");
  ASSERT_TRUE(bool(Address)) << toString(Address.takeError());
#ifdef EJIT_SRE_CODE_POOL
  Err = (*Engine)->flushPendingCode();
  ASSERT_FALSE(bool(Err)) << toString(std::move(Err));
#endif
  using Entry = uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t *);
  Entry Fn = reinterpret_cast<Entry>(*Address);
  for (uint32_t Cell : {0u, 3u, 7u, 256u, 259u})
    for (uint32_t Trp : {2u, 5u, 258u, 261u})
      for (uint32_t X : {0u, 17u}) {
        const uint32_t Config = Cell == 0 && Trp == 2   ? 7u
                                : Cell == 3 && Trp == 5 ? 11u
                                                        : 1000u;
        const uint32_t Expected = Cell * 100u + Trp * 10u + X + Config;
        uint32_t Out = 0;
        EXPECT_EQ(Fn(Cell, Trp, X, &Out), Expected);
        EXPECT_EQ(Out, Expected);
      }
}

TEST(EJitVersionMerge, SingleDimensionKeepsOtherArgumentsLive) {
  LLVMContext Ctx;
  auto M = parse(Ctx);
  auto Arms = arms(*M);
  Arms[0].dimensions = {2};
  Arms[1].dimensions = {5};
  auto Result = assembleFinalVersionSwitch(
      "merged", *M->getFunction("merge_aot"), {1}, Arms);
  ASSERT_TRUE(bool(Result)) << toString(Result.takeError());
  unsigned Switches = 0;
  for (Instruction &I : instructions(**Result))
    if (auto *S = dyn_cast<SwitchInst>(&I)) {
      ++Switches;
      EXPECT_EQ(S->getCondition(), (*Result)->getArg(1));
    }
  EXPECT_EQ(Switches, 1u);
  EXPECT_FALSE((*Result)->getArg(0)->use_empty());
  EXPECT_FALSE((*Result)->getArg(2)->use_empty());
}

TEST(EJitVersionMerge, RejectsDuplicatesAndBudgetsWithoutChangingInputs) {
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    LLVMContext Ctx;
    auto M = parse(Ctx);
    auto Arms = arms(*M);
    EJitFinalMergeLimits Limits;
    SmallVector<unsigned, 2> Dims{0, 1};
    if (Mode == 0)
      Arms[1].dimensions = Arms[0].dimensions;
    if (Mode == 1)
      Limits.maxArms = 1;
    if (Mode == 2)
      Limits.maxInstructions = 1;
    if (Mode == 3)
      Dims = {0, 0};
    auto Result = assembleFinalVersionSwitch(
        "merged", *M->getFunction("merge_aot"), Dims, Arms, Limits);
    ASSERT_FALSE(bool(Result));
    consumeError(Result.takeError());
    EXPECT_EQ(M->getFunction("merged"), nullptr);
    EXPECT_EQ(M->getFunction("arm_a")->getInstructionCount(), 7u);
    EXPECT_FALSE(verifyModule(*M, &errs()));
  }
}

TEST(EJitVersionMerge, NarrowDimensionsRejectUnrepresentableMembers) {
  LLVMContext Ctx;
  auto M = parse(Ctx, R"(
    declare i32 @aot(i8)
    define internal i32 @a(i8 %cell) { ret i32 7 }
  )");
  SmallVector<EJitFinalMergeArm, 1> Arms{{M->getFunction("a"), {256}}};
  auto Result =
      assembleFinalVersionSwitch("merged", *M->getFunction("aot"), {0}, Arms);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(toString(Result.takeError()).find("does-not-fit"),
            std::string::npos);
  EXPECT_EQ(M->getFunction("merged"), nullptr);
}

TEST(EJitVersionMerge, MemberInferenceDoesNotApplyToAotOrWholeDispatcher) {
  LLVMContext Ctx;
  auto M = parse(Ctx, R"(
    declare signext i8 @aot(i8 zeroext)
    define internal noundef signext i8 @a(i8 zeroext %cell) memory(none) {
      ret i8 7
    }
  )");
  SmallVector<EJitFinalMergeArm, 1> Arms{{M->getFunction("a"), {3}}};
  auto Result =
      assembleFinalVersionSwitch("merged", *M->getFunction("aot"), {0}, Arms);
  ASSERT_TRUE(bool(Result)) << toString(Result.takeError());
  EXPECT_FALSE((*Result)->doesNotAccessMemory());
  EXPECT_FALSE((*Result)->getAttributes().getRetAttrs().hasAttribute(
      Attribute::NoUndef));
  EXPECT_TRUE(
      (*Result)->getAttributes().getRetAttrs().hasAttribute(Attribute::SExt));
  EXPECT_TRUE((*Result)->getAttributes().getParamAttrs(0).hasAttribute(
      Attribute::ZExt));
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(EJitVersionMerge, KeepsMemberBranchProfileAndDiagnosticCalls) {
  LLVMContext Ctx;
  auto M = parse(Ctx, R"(
    declare i32 @aot(i8, i32)
    declare void @diagnostic(i32)
    define internal i32 @a(i8 %cell, i32 %value) {
      %test = icmp eq i32 %value, 0
      br i1 %test, label %yes, label %no, !prof !0
    yes:
      call void @diagnostic(i32 17)
      ret i32 7
    no:
      call void @diagnostic(i32 18)
      ret i32 11
    }
    !0 = !{!"branch_weights", i32 63, i32 1}
  )");
  SmallVector<EJitFinalMergeArm, 1> Arms{{M->getFunction("a"), {3}}};
  auto Result =
      assembleFinalVersionSwitch("merged", *M->getFunction("aot"), {0}, Arms);
  ASSERT_TRUE(bool(Result)) << toString(Result.takeError());
  unsigned Profiles = 0, Checks = 0;
  for (Instruction &I : instructions(**Result)) {
    if (auto *Br = dyn_cast<BranchInst>(&I))
      if (Br->isConditional() && Br->getMetadata(LLVMContext::MD_prof))
        ++Profiles;
    if (auto *Call = dyn_cast<CallBase>(&I))
      if (Call->getCalledFunction() == M->getFunction("diagnostic"))
        ++Checks;
  }
  EXPECT_EQ(Profiles, 1u);
  EXPECT_EQ(Checks, 2u);
}

TEST(EJitVersionMerge, RecursiveMemberIsNotDisguisedAsOneMergedBody) {
  LLVMContext Ctx;
  auto M = parse(Ctx, R"(
    declare i32 @aot(i8)
    define internal i32 @a(i8 %cell) {
      %r = call i32 @helper(i8 %cell)
      ret i32 %r
    }
    define internal i32 @helper(i8 %cell) {
      %r = call i32 @a(i8 %cell)
      ret i32 %r
    }
  )");
  SmallVector<EJitFinalMergeArm, 1> Arms{{M->getFunction("a"), {3}}};
  auto Result =
      assembleFinalVersionSwitch("merged", *M->getFunction("aot"), {0}, Arms);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(toString(Result.takeError()).find("recursion"), std::string::npos);
}

#endif
