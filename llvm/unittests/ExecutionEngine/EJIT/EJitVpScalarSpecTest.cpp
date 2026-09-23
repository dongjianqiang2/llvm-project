//===-- EJitVpScalarSpecTest.cpp - guarded scalar specialization tests ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Tests for the scalar/loop-bound value-site discovery, annotation, and the
//  guarded loop-versioning transform (EJIT_VALUE_PROFILE.md §7):
//    - the guard (Bound == topValue) exists and selects hot/cold paths;
//    - the hot loop sees the constant bound + llvm.assume;
//    - the untouched cold clone is the generic fallback;
//    - below-threshold / unknown sites are left alone (honest no-op);
//    - SEMANTIC EQUIVALENCE: original and specialized functions produce
//      identical results across 0 / negative / boundary / random inputs,
//      executed for real through LLJIT.
//  Compiled only under EJIT_SRE_PGO_VALUE_PROFILE; empty otherwise.
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_SRE_PGO_VALUE_PROFILE

#include "llvm/ExecutionEngine/EJIT/EJitValueProfile.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

// LLJIT needs the native target registered once per process.
static const bool kTargetsInitialized = [] {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
  return true;
}();

const char *kLoopBoundIR = R"(
define i32 @hot(i32 %a) {
entry:
  br label %loop.header
loop.header:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop.latch ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop.latch ]
  %acc.next = add i32 %acc, %i
  %i.next = add i32 %i, 1
  %exit = icmp slt i32 %i.next, %a
  br i1 %exit, label %loop.latch, label %exit.bb
loop.latch:
  br label %loop.header
exit.bb:
  ret i32 %acc.next
}
)";

std::unique_ptr<Module> parseModule(LLVMContext &Ctx, const char *IR) {
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("EJitVpScalarSpecTest", errs());
  EXPECT_TRUE(M);
  return M;
}

struct Analyses {
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  Analyses() {
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  }
};

std::string irToString(Module &M) {
  std::string S;
  raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return S;
}

// Execute @hot over LLJIT and return the result table.
std::vector<int32_t> runViaJit(std::unique_ptr<Module> M,
                               ArrayRef<int32_t> inputs) {
  std::vector<int32_t> results;
  auto J = cantFail(orc::LLJITBuilder().create());
  auto TSM =
      orc::ThreadSafeModule(std::move(M), std::make_unique<LLVMContext>());
  cantFail(J->addIRModule(std::move(TSM)));
  auto Sym = cantFail(J->lookup("hot"));
  auto *Fn =
      jitTargetAddressToFunction<int32_t (*)(int32_t)>(Sym.getValue());
  for (int32_t in : inputs)
    results.push_back(Fn(in));
  return results;
}

} // namespace

TEST(EJitVpScalarSpec, InstrumentationCarriesExactSamplingSession) {
  LLVMContext Ctx;
  std::unique_ptr<Module> M = parseModule(Ctx, kLoopBoundIR);
  ASSERT_TRUE(M);
  Analyses A;
  Function &F = *M->getFunction("hot");
  runValueProfileOnFunction(F, A.FAM, EJitValueProfileMode::Instrument, nullptr,
                            0x1234u);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  const std::string IR = irToString(*M);
  EXPECT_NE(IR.find("@ejit_vp_record_scalar_session(i64 4660"),
            std::string::npos);
  EXPECT_EQ(IR.find("call void @ejit_vp_record_scalar("), std::string::npos);
}

TEST(EJitVpScalarSpec, GuardedVersioningPreservesSemantics) {
  LLVMContext Ctx;
  std::unique_ptr<Module> Orig = parseModule(Ctx, kLoopBoundIR);
  ASSERT_TRUE(Orig);
  ASSERT_FALSE(verifyModule(*Orig, &errs()));

  // Reference results from the untouched module.
  const int32_t inputs[] = {0, 1, 7, 99, 100, 101, 200, -5, -100, 42, 12345};
  std::vector<int32_t> ref = runViaJit(
      std::unique_ptr<Module>(CloneModule(*Orig).release()), inputs);

  // Annotate + specialize with topValue = 100 (dominant).
  Analyses A;
  Function &F = *Orig->getFunction("hot");
  const uint64_t hash = IndexedInstrProf::ComputeHash(getIRPGOFuncName(F));
  runValueProfileOnFunction(F, A.FAM, EJitValueProfileMode::Annotate);
  PgoScalarSite sites[] = {{hash, 0, /*topValue=*/100, /*topCount=*/990,
                            /*total=*/1000}};
  EJitScalarValueSpecPass Spec(sites);
  FunctionPassManager FPM;
  FPM.addPass(std::move(Spec));
  FPM.run(F, A.FAM);
  ASSERT_FALSE(verifyModule(*Orig, &errs()));

  const std::string IR = irToString(*Orig);
  // The guard must exist and select hot/cold paths...
  EXPECT_NE(IR.find("icmp eq i32 %a, 100"), std::string::npos);
  EXPECT_NE(IR.find("llvm.assume"), std::string::npos);
  // ...the hot loop sees the constant bound...
  EXPECT_NE(IR.find("icmp slt i32 %i.next, 100"), std::string::npos);
  // ...and the untouched cold clone is the generic fallback.
  EXPECT_NE(IR.find(".vp.cold"), std::string::npos);

  // Semantics: identical results for every input, including 0, negatives, the
  // specialized value itself and its neighbors.
  std::vector<int32_t> got = runViaJit(std::move(Orig), inputs);
  EXPECT_EQ(got, ref);
}

TEST(EJitVpScalarSpec, NegativeTopValueIsHandled) {
  LLVMContext Ctx;
  std::unique_ptr<Module> M = parseModule(Ctx, kLoopBoundIR);
  ASSERT_TRUE(M);

  Analyses A;
  Function &F = *M->getFunction("hot");
  const uint64_t hash = IndexedInstrProf::ComputeHash(getIRPGOFuncName(F));
  runValueProfileOnFunction(F, A.FAM, EJitValueProfileMode::Annotate);
  // Specialize on a NEGATIVE bound: the guard must compare against -100 and
  // semantics must hold (the hot loop is a zero-trip loop for that value).
  PgoScalarSite sites[] = {{hash, 0, static_cast<uint64_t>(-100), 990, 1000}};
  EJitScalarValueSpecPass Spec(sites);
  FunctionPassManager FPM;
  FPM.addPass(std::move(Spec));
  FPM.run(F, A.FAM);
  ASSERT_FALSE(verifyModule(*M, &errs()));

  const std::string IR = irToString(*M);
  EXPECT_NE(IR.find("icmp eq i32 %a, -100"), std::string::npos);

  const int32_t inputs[] = {-100, -99, 0, 5, 100};
  std::vector<int32_t> ref;
  {
    LLVMContext Ctx2;
    std::unique_ptr<Module> RefM = parseModule(Ctx2, kLoopBoundIR);
    ref = runViaJit(std::move(RefM), inputs);
  }
  std::vector<int32_t> got = runViaJit(std::move(M), inputs);
  EXPECT_EQ(got, ref);
}

TEST(EJitVpScalarSpec, BelowThresholdSiteIsIgnored) {
  LLVMContext Ctx;
  std::unique_ptr<Module> M = parseModule(Ctx, kLoopBoundIR);
  ASSERT_TRUE(M);

  Analyses A;
  Function &F = *M->getFunction("hot");
  const uint64_t hash = IndexedInstrProf::ComputeHash(getIRPGOFuncName(F));
  runValueProfileOnFunction(F, A.FAM, EJitValueProfileMode::Annotate);
  // 99 samples at 99% -> below EJIT_SRE_VP_MIN_SAMPLES(100): must NOT
  // specialize (honest no-op; no fake profit).
  PgoScalarSite sites[] = {{hash, 0, 100, 99, 100}};
  EJitScalarValueSpecPass Spec(sites);
  FunctionPassManager FPM;
  FPM.addPass(std::move(Spec));
  FPM.run(F, A.FAM);

  const std::string IR = irToString(*M);
  EXPECT_EQ(IR.find(".vp.cold"), std::string::npos);
  EXPECT_EQ(IR.find("icmp eq i32 %a, 100"), std::string::npos);
  // Metadata consumed/stripped.
  EXPECT_EQ(IR.find("ejit.vp"), std::string::npos);
}

TEST(EJitVpScalarSpec, UnknownSiteHashIsIgnored) {
  LLVMContext Ctx;
  std::unique_ptr<Module> M = parseModule(Ctx, kLoopBoundIR);
  ASSERT_TRUE(M);

  Analyses A;
  Function &F = *M->getFunction("hot");
  runValueProfileOnFunction(F, A.FAM, EJitValueProfileMode::Annotate);
  PgoScalarSite sites[] = {{0xDEADu, 0, 100, 990, 1000}}; // wrong hash
  EJitScalarValueSpecPass Spec(sites);
  FunctionPassManager FPM;
  FPM.addPass(std::move(Spec));
  FPM.run(F, A.FAM);

  const std::string IR = irToString(*M);
  EXPECT_EQ(IR.find(".vp.cold"), std::string::npos);
}

TEST(EJitVpScalarSpec, MultiExitLoopGetsNoSite) {
  LLVMContext Ctx;
  // Same loop plus an early break: the loop has TWO exiting blocks, so the
  // discovery must refuse to create a site (no clean single bound).
  const char *IR = R"(
define i32 @hot(i32 %a, i32 %lim) {
entry:
  br label %loop.header
loop.header:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop.latch ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop.latch ]
  %acc.next = add i32 %acc, %i
  %i.next = add i32 %i, 1
  %early = icmp sgt i32 %i.next, %lim
  br i1 %early, label %exit.bb, label %loop.cont
loop.cont:
  %exit = icmp slt i32 %i.next, %a
  br i1 %exit, label %loop.latch, label %exit.bb
loop.latch:
  br label %loop.header
exit.bb:
  ret i32 %acc.next
}
)";
  std::unique_ptr<Module> M = parseModule(Ctx, IR);
  ASSERT_TRUE(M);

  Analyses A;
  Function &F = *M->getFunction("hot");
  uint32_t count = 0;
  runValueProfileOnFunction(F, A.FAM, EJitValueProfileMode::Annotate,
                            [&](StringRef, uint32_t n) { count = n; });
  EXPECT_EQ(count, 0u);
  const std::string S = irToString(*M);
  EXPECT_EQ(S.find("ejit.vp"), std::string::npos);
}

#endif // EJIT_SRE_PGO_VALUE_PROFILE
