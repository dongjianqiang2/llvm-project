//===-- EJitProfileLoweringTest.cpp - PGO lowering definitions tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Common Tier-1 materialization must claim definitions actually emitted by
// instrumentation lowering. In particular, Linux does not emit the profile
// runtime-hook user, while COFF does. Run both real lowering paths as IR-only
// tests on any host; no alternate runtime symbols or target skips are needed.
// Native SmallTableHost/Wrapper tests separately exercise real ORC claims.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Instrumentation/InstrProfiling.h"
#include "llvm/Transforms/Instrumentation/PGOInstrumentation.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

void checkLoweredDefinitions(StringRef TargetTriple, StringRef Layout,
                             bool ExpectsHookUser) {
  SCOPED_TRACE(TargetTriple.str());
  LLVMContext Context;
  Module M("profile-lowering-target-contract", Context);
  M.setTargetTriple(Triple(TargetTriple));
  M.setDataLayout(Layout);

  Type *I32 = Type::getInt32Ty(Context);
  auto *F = Function::Create(FunctionType::get(I32, {I32}, false),
                             GlobalValue::ExternalLinkage,
                             "profile_lowering_entry", M);
  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  BasicBlock *Positive = BasicBlock::Create(Context, "positive", F);
  BasicBlock *Other = BasicBlock::Create(Context, "other", F);
  IRBuilder<> Builder(Entry);
  Value *X = F->getArg(0);
  Builder.CreateCondBr(Builder.CreateICmpSGT(X, Builder.getInt32(0)), Positive,
                       Other);
  Builder.SetInsertPoint(Positive);
  Builder.CreateRet(Builder.CreateAdd(X, Builder.getInt32(1)));
  Builder.SetInsertPoint(Other);
  Builder.CreateRet(Builder.CreateSub(X, Builder.getInt32(1)));
  ASSERT_FALSE(verifyModule(M, &errs()));

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager Gen;
  Gen.addPass(PGOInstrumentationGen(PGOInstrumentationType::FDO));
  Gen.run(M, MAM);
  ASSERT_FALSE(verifyModule(M, &errs()));

  uint64_t CounterCount = 0;
  unsigned IncrementSites = 0;
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB)
      if (auto *Increment = dyn_cast<InstrProfIncrementInst>(&I)) {
        uint64_t SiteCount = Increment->getNumCounters()->getZExtValue();
        if (IncrementSites++ == 0)
          CounterCount = SiteCount;
        EXPECT_EQ(SiteCount, CounterCount);
        EXPECT_LT(Increment->getIndex()->getZExtValue(), CounterCount);
      }
  ASSERT_GT(IncrementSites, 0u);
  ASSERT_GT(CounterCount, 0u);

  // Match the actual EJIT Tier-1 policy: concurrent callers update counters
  // atomically, and the full generated counter array must survive lowering.
  InstrProfOptions Options;
  Options.Atomic = true;
  ModulePassManager Lower;
  Lower.addPass(InstrProfilingLoweringPass(Options));
  Lower.run(M, MAM);
  ASSERT_FALSE(verifyModule(M, &errs()));

  auto *Counters = M.getNamedGlobal("__profc_profile_lowering_entry");
  ASSERT_NE(Counters, nullptr);
  EXPECT_FALSE(Counters->isDeclarationForLinker());
  ASSERT_TRUE(Counters->hasInitializer());
  auto *CounterArray = dyn_cast<ArrayType>(Counters->getValueType());
  ASSERT_NE(CounterArray, nullptr);
  EXPECT_EQ(CounterArray->getNumElements(), CounterCount);
  EXPECT_TRUE(CounterArray->getElementType()->isIntegerTy(64));
  EXPECT_TRUE(Counters->getInitializer()->isNullValue());

  auto *Data = M.getNamedGlobal("__profd_profile_lowering_entry");
  ASSERT_NE(Data, nullptr);
  EXPECT_FALSE(Data->isDeclarationForLinker());
  EXPECT_TRUE(Data->hasInitializer());

  unsigned AtomicUpdates = 0;
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB) {
      EXPECT_FALSE(isa<InstrProfIncrementInst>(I));
      if (auto *Update = dyn_cast<AtomicRMWInst>(&I)) {
        ++AtomicUpdates;
        EXPECT_EQ(Update->getOperation(), AtomicRMWInst::Add);
      }
    }
  EXPECT_EQ(AtomicUpdates, IncrementSites);

  Function *HookUser = M.getFunction("__llvm_profile_runtime_user");
  GlobalVariable *Hook = M.getNamedGlobal("__llvm_profile_runtime");
  if (!ExpectsHookUser) {
    EXPECT_EQ(HookUser, nullptr);
    EXPECT_EQ(Hook, nullptr);
    return;
  }

  ASSERT_NE(HookUser, nullptr);
  EXPECT_FALSE(HookUser->isDeclarationForLinker());
  EXPECT_FALSE(HookUser->hasLocalLinkage());
  EXPECT_TRUE(HookUser->hasComdat());
  ASSERT_NE(Hook, nullptr);
  EXPECT_TRUE(Hook->isDeclarationForLinker());
  EXPECT_FALSE(Hook->hasInitializer());
  unsigned HookLoads = 0;
  for (BasicBlock &BB : *HookUser)
    for (Instruction &I : BB)
      if (auto *Load = dyn_cast<LoadInst>(&I)) {
        ++HookLoads;
        EXPECT_EQ(Load->getPointerOperand(), Hook);
      }
  EXPECT_EQ(HookLoads, 1u);
}

TEST(EJitProfileLoweringTest, LinuxDoesNotEmitRuntimeHookUser) {
  checkLoweredDefinitions("aarch64-unknown-linux-gnu",
                          "e-m:e-i64:64-i128:128-n32:64-S128", false);
}

TEST(EJitProfileLoweringTest, CoffEmitsRuntimeHookUserDefinition) {
  checkLoweredDefinitions(
      "x86_64-pc-windows-msvc",
      "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-"
      "n8:16:32:64-S128",
      true);
}

} // namespace
