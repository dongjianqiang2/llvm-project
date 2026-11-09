//===- EJitJitOptionsTest.cpp - EJIT_JIT_* option tests -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitJitOptions.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

constexpr unsigned idx(JitOption O) { return static_cast<unsigned>(O); }

ejit_jit_options_record_t makeRecord() {
  ejit_jit_options_record_t R = {
      EJIT_JIT_OPTIONS_MAGIC, EJIT_JIT_OPTION_COUNT, {}};
  for (int32_t &V : R.values)
    V = EJIT_JIT_DEFAULT;
  return R;
}

cl::Option *clOpt(StringRef Name) {
  auto &Opts = cl::getRegisteredOptions();
  auto It = Opts.find(Name);
  return It == Opts.end() ? nullptr : It->second;
}

class EJitJitOptions : public ::testing::Test {
protected:
  // Options are process-global; leave them at their defaults.
  void TearDown() override { apply({}); }

  static void apply(std::initializer_list<std::pair<JitOption, int32_t>> Set) {
    JitOptionValues V;
    for (int32_t &X : V)
      X = EJIT_JIT_DEFAULT;
    for (auto [O, X] : Set)
      V[idx(O)] = X;
    ASSERT_TRUE(applyJitOptions(V));
  }
};

TEST_F(EJitJitOptions, MergeCombinesRecords) {
  ejit_jit_options_record_t A = makeRecord(), B = makeRecord();
  A.values[idx(JitOption::EnableLoopDistribute)] = 1;
  B.values[idx(JitOption::AArch64LoadStoreScanLimit)] = 200;
  B.values[idx(JitOption::EnableLoopDistribute)] = 1;
  ejit_jit_options_record_t Records[] = {A, B};

  JitOptionValues V;
  ASSERT_TRUE(mergeJitOptionRecords(Records, V));
  EXPECT_EQ(V[idx(JitOption::EnableLoopDistribute)], 1);
  EXPECT_EQ(V[idx(JitOption::AArch64LoadStoreScanLimit)], 200);
  EXPECT_EQ(V[idx(JitOption::ExtraVectorizerPasses)], EJIT_JIT_DEFAULT);
}

TEST_F(EJitJitOptions, MergeRejectsConflictsAndBadValues) {
  JitOptionValues V;
  ejit_jit_options_record_t A = makeRecord(), B = makeRecord();
  A.values[idx(JitOption::EnableLoopDistribute)] = 1;
  B.values[idx(JitOption::EnableLoopDistribute)] = 0;
  ejit_jit_options_record_t Conflict[] = {A, B};
  EXPECT_FALSE(mergeJitOptionRecords(Conflict, V));

  ejit_jit_options_record_t Switch = makeRecord();
  Switch.values[idx(JitOption::EnablePartialInlining)] = 2;
  EXPECT_FALSE(mergeJitOptionRecords(Switch, V));

  ejit_jit_options_record_t Negative = makeRecord();
  Negative.values[idx(JitOption::AArch64UpdateScanLimit)] = -5;
  EXPECT_FALSE(mergeJitOptionRecords(Negative, V));

  ejit_jit_options_record_t BadMagic = makeRecord();
  BadMagic.magic = 0;
  EXPECT_FALSE(mergeJitOptionRecords(BadMagic, V));
}

TEST_F(EJitJitOptions, ApplySetsAndRestoresLLVMOption) {
  cl::Option *O = clOpt("enable-loop-distribute");
  ASSERT_NE(O, nullptr);
  ASSERT_EQ(O->getNumOccurrences(), 0);

  apply({{JitOption::EnableLoopDistribute, 1}});
  EXPECT_EQ(O->getNumOccurrences(), 1);
  EXPECT_TRUE(jitOptionEnabled(JitOption::EnableLoopDistribute));

  apply({});
  EXPECT_EQ(O->getNumOccurrences(), 0);
  EXPECT_FALSE(jitOptionEnabled(JitOption::EnableLoopDistribute));
}

TEST_F(EJitJitOptions, AArch64CodegenOptionReachesLLVM) {
  cl::Option *O = clOpt("aarch64-load-store-scan-limit");
  JitOptionValues V;
  for (int32_t &X : V)
    X = EJIT_JIT_DEFAULT;
  V[idx(JitOption::AArch64LoadStoreScanLimit)] = 200;
  if (!O) {
    EXPECT_FALSE(applyJitOptions(V));
    GTEST_SKIP() << "AArch64 backend not linked";
  }
  ASSERT_TRUE(applyJitOptions(V));
  EXPECT_EQ(O->getNumOccurrences(), 1);
}

// for (i = 0, j = 1; i < n; i++, j++) {
//   A[j] = A[i] * B[i];
//   C[i] = D[i] * E[i];
// }
// The second induction variable keeps GVN from turning the distance-1
// dependence into a register recurrence, so the loop is still distributable
// after simplification. Distributed blocks get a .ldist suffix.
static const char *const kDistributableLoop = R"(
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

define void @f(ptr noalias %a, ptr noalias %b, ptr noalias %c, ptr noalias %d,
               ptr noalias %e, i64 %n) !ejit.metadata !0 {
entry:
  %guard = icmp sgt i64 %n, 0
  br i1 %guard, label %for.body, label %for.end

for.body:
  %ind = phi i64 [ 0, %entry ], [ %add, %for.body ]
  %j = phi i64 [ 1, %entry ], [ %j.next, %for.body ]
  %arrayidxA = getelementptr inbounds i32, ptr %a, i64 %ind
  %loadA = load i32, ptr %arrayidxA, align 4
  %arrayidxB = getelementptr inbounds i32, ptr %b, i64 %ind
  %loadB = load i32, ptr %arrayidxB, align 4
  %mulA = mul i32 %loadB, %loadA
  %arrayidxA1 = getelementptr inbounds i32, ptr %a, i64 %j
  store i32 %mulA, ptr %arrayidxA1, align 4
  %arrayidxD = getelementptr inbounds i32, ptr %d, i64 %ind
  %loadD = load i32, ptr %arrayidxD, align 4
  %arrayidxE = getelementptr inbounds i32, ptr %e, i64 %ind
  %loadE = load i32, ptr %arrayidxE, align 4
  %mulC = mul i32 %loadD, %loadE
  %arrayidxC = getelementptr inbounds i32, ptr %c, i64 %ind
  store i32 %mulC, ptr %arrayidxC, align 4
  %add = add nuw nsw i64 %ind, 1
  %j.next = add nuw nsw i64 %j, 1
  %exitcond = icmp eq i64 %add, %n
  br i1 %exitcond, label %for.end, label %for.body

for.end:
  ret void
}

!0 = !{!1}
!1 = !{!"ejit_entry"}
)";

std::unique_ptr<Module> optimize(LLVMContext &Ctx, OptimizationLevel L) {
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(kDistributableLoop, Err, Ctx);
  if (!M)
    return nullptr;
  PeriodArrayRegistry Reg;
  EJitOptimizer Opt(Reg);
  SpecializationContext SCtx;
  SCtx.fnName = "f";
  SCtx.optLevel = L;
  Opt.runPipeline(*M, SCtx);
  return M;
}

bool isDistributed(Module &M) {
  for (BasicBlock &BB : *M.getFunction("f"))
    if (BB.getName().contains(".ldist"))
      return true;
  return false;
}

TEST_F(EJitJitOptions, LoopDistributeRunsOnlyWhenEnabled) {
  LLVMContext Ctx;
  auto Off = optimize(Ctx, OptimizationLevel::L2);
  ASSERT_NE(Off, nullptr);
  EXPECT_FALSE(isDistributed(*Off));

  apply({{JitOption::EnableLoopDistribute, 1}});
  auto On = optimize(Ctx, OptimizationLevel::L2);
  ASSERT_NE(On, nullptr);
  EXPECT_TRUE(isDistributed(*On));
}

TEST_F(EJitJitOptions, AllPipelineSwitchesProduceValidIR) {
  apply({{JitOption::EnablePartialInlining, 1},
         {JitOption::ExtraVectorizerPasses, 1},
         {JitOption::EnableLoopVersioningLicm, 1},
         {JitOption::EnableLoopDistribute, 1},
         {JitOption::SlpVectorizeHorStore, 1}});
  for (OptimizationLevel L :
       {OptimizationLevel::L1, OptimizationLevel::L2, OptimizationLevel::L3}) {
    LLVMContext Ctx;
    auto M = optimize(Ctx, L);
    ASSERT_NE(M, nullptr);
    EXPECT_FALSE(verifyModule(*M, &errs())) << "level " << int(L);
  }
}

} // namespace
