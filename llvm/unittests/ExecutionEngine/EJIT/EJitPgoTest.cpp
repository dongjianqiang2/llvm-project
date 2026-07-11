//===-- EJitPgoTest.cpp - online PGO pipeline unit tests ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfWriter.h"
#include "llvm/Support/Error.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

// foo(i32 %n): if (n > 0) call @ea(n); else call @eb(n); ret n.
// Both arms have external calls (unknown side effects) so SimplifyCFG cannot
// fold the if-then-else into a select - the conditional branch survives the
// optimization pipeline, letting the test observe whether !prof survives too
// (§11.1). @ea/@eb are declarations (resolved at JIT link in production).
static std::unique_ptr<Module> makeFooModule(LLVMContext &Ctx) {
  auto M = std::make_unique<Module>("pgo_test", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *Void = Type::getVoidTy(Ctx);
  auto *CallTy = FunctionType::get(Void, {I32}, false);
  M->getOrInsertFunction("ea", CallTy);
  M->getOrInsertFunction("eb", CallTy);
  auto *F = Function::Create(FunctionType::get(I32, {I32}, false),
                             Function::ExternalLinkage, "foo", M.get());
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *Then = BasicBlock::Create(Ctx, "then", F);
  BasicBlock *Else = BasicBlock::Create(Ctx, "else", F);
  IRBuilder<> B(Entry);
  Value *Cmp = B.CreateICmpSGT(F->getArg(0), ConstantInt::get(I32, 0));
  B.CreateCondBr(Cmp, Then, Else);
  B.SetInsertPoint(Then);
  B.CreateCall(M->getFunction("ea"), {F->getArg(0)});
  B.CreateRet(F->getArg(0));
  B.SetInsertPoint(Else);
  B.CreateCall(M->getFunction("eb"), {F->getArg(0)});
  B.CreateRet(F->getArg(0));
  return M;
}

// Tier-1 (Instrumented) must create __profc_foo/__profd_foo, force them
// ExternalLinkage (ORC lookup visibility, P0-3), and capture the pgoName.
TEST(EJitPgo, Tier1InstrumentsAndCapturesCounterNames) {
  LLVMContext Ctx;
  auto M0 = makeFooModule(Ctx);
  PeriodArrayRegistry reg;
  EJitOptimizer opt(reg);
  SpecializationContext sc;
  sc.fnName = "foo";
  sc.tier = CompileTier::Instrumented;
  opt.runPipeline(*M0, sc);

  GlobalVariable *Profc = M0->getGlobalVariable("__profc_foo", /*AllowLocal=*/true);
  GlobalVariable *Profd = M0->getGlobalVariable("__profd_foo", /*AllowLocal=*/true);
  ASSERT_NE(Profc, nullptr);
  ASSERT_NE(Profd, nullptr);
  EXPECT_FALSE(Profc->hasLocalLinkage());
  EXPECT_FALSE(Profd->hasLocalLinkage());

  bool hasFoo = false;
  for (const std::string &n : opt.getLastCounterNames())
    if (n == "foo")
      hasFoo = true;
  EXPECT_TRUE(hasFoo);
}

// Tier-2 (PGOUse) on a fresh clone of the SAME IR, fed a profile synthesized
// with Tier-1's FuncHash, must annotate !prof on the conditional branch.
// This proves (a) the Gen/Use-point CFG hash matches (else Use would skip the
// record) and (b) !prof survives the post-Use optimization pipeline (§11.1).
TEST(EJitPgo, Tier2PgoUseAnnotatesBranchWeights) {
  LLVMContext Ctx;
  auto M0 = makeFooModule(Ctx);
  PeriodArrayRegistry reg;
  EJitOptimizer opt(reg);

  // Tier-1 on a clone: get the FuncHash recorded in __profd_foo.
  auto M1 = CloneModule(*M0);
  SpecializationContext sc1;
  sc1.fnName = "foo";
  sc1.tier = CompileTier::Instrumented;
  opt.runPipeline(*M1, sc1);
  GlobalVariable *Profd = M1->getGlobalVariable("__profd_foo", /*AllowLocal=*/true);
  ASSERT_NE(Profd, nullptr);
  auto *ProfdInit = dyn_cast<ConstantStruct>(Profd->getInitializer());
  ASSERT_NE(ProfdInit, nullptr);
  ASSERT_GE(ProfdInit->getNumOperands(), 2u);
  uint64_t FuncHash =
      cast<ConstantInt>(ProfdInit->getOperand(1))->getZExtValue();
  GlobalVariable *Profc = M1->getGlobalVariable("__profc_foo", /*AllowLocal=*/true);
  ASSERT_NE(Profc, nullptr);
  unsigned NumCounters =
      cast<ArrayType>(Profc->getValueType())->getNumElements();

  // Synthesize an indexed profile (100/1 branch weights).
  InstrProfWriter Writer;
  consumeError(Writer.mergeProfileKind(InstrProfKind::IRInstrumentation));
  std::vector<uint64_t> Counts(NumCounters, 0);
  Counts[0] = 100;
  if (NumCounters > 1)
    Counts[1] = 1;
  if (NumCounters > 2)
    Counts[2] = 100;
  NamedInstrProfRecord Rec("foo", FuncHash, Counts);
  Writer.addRecord(std::move(Rec), 1, [](Error) {});
  auto Buf = Writer.writeBuffer();
  ASSERT_NE(Buf, nullptr);

  // Tier-2 on a fresh clone of the SAME original IR.
  opt.clearAnalyses();
  auto M2 = CloneModule(*M0);
  SpecializationContext sc2;
  sc2.fnName = "foo";
  sc2.tier = CompileTier::PGOUse;
  sc2.profileData = std::string(Buf->getBuffer());
  opt.runPipeline(*M2, sc2);

  Function *Foo = M2->getFunction("foo");
  ASSERT_NE(Foo, nullptr);
  bool foundProf = false;
  for (BasicBlock &BB : *Foo)
    for (Instruction &I : BB)
      if (auto *BI = dyn_cast<BranchInst>(&I))
        if (BI->isConditional() && BI->hasMetadata("prof"))
          foundProf = true;
  EXPECT_TRUE(foundProf);
}

namespace {
// foo(i32) calls bar(i32) twice; bar(i32) = x*3 + 2 (small, inlinable).
std::unique_ptr<Module> makeFooCallsBarModule(LLVMContext &Ctx) {
  auto M = std::make_unique<Module>("pgo_inline", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *FnTy = FunctionType::get(I32, {I32}, false);
  auto *Bar = Function::Create(FnTy, Function::ExternalLinkage, "bar", M.get());
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "b", Bar);
    IRBuilder<> B(BB);
    Value *M3 = B.CreateMul(Bar->getArg(0), ConstantInt::get(I32, 3));
    B.CreateRet(B.CreateAdd(M3, ConstantInt::get(I32, 2)));
  }
  auto *Foo = Function::Create(FnTy, Function::ExternalLinkage, "foo", M.get());
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "b", Foo);
    IRBuilder<> B(BB);
    Value *V1 = B.CreateCall(Bar, {Foo->getArg(0)});
    Value *V2 = B.CreateCall(Bar, {Foo->getArg(0)});
    B.CreateRet(B.CreateAdd(V1, V2));
  }
  return M;
}

// Read FuncHash (field 1) + NumCounters from __profd_/__profc_<name> after Gen.
bool readCounterInfo(const Module &M, const std::string &name, uint64_t &hash,
                     unsigned &numCounters) {
  const GlobalVariable *Profd = M.getGlobalVariable("__profd_" + name, true);
  const GlobalVariable *Profc = M.getGlobalVariable("__profc_" + name, true);
  if (!Profd || !Profc)
    return false;
  auto *Init = dyn_cast<ConstantStruct>(Profd->getInitializer());
  if (!Init || Init->getNumOperands() < 2)
    return false;
  if (auto *CI = dyn_cast<ConstantInt>(Init->getOperand(1)))
    hash = CI->getZExtValue();
  else
    return false;
  numCounters = cast<ArrayType>(Profc->getValueType())->getNumElements();
  return true;
}
} // namespace

// PGO stage 3: Tier-2 PGOUse + ModuleInlinerWrapperPass inlines a hot callee
// (bar) into its caller (foo). Verifies the CGSCC inline pass runs in the
// Tier-2 pipeline and inlines (foo no longer has a call to bar).
TEST(EJitPgo, Tier2PgoInlinesHotCallee) {
  LLVMContext Ctx;
  auto M0 = makeFooCallsBarModule(Ctx);
  PeriodArrayRegistry reg;
  EJitOptimizer opt(reg);

  // Tier-1 Gen -> get foo/bar FuncHash + NumCounters.
  auto M1 = CloneModule(*M0);
  SpecializationContext sc1;
  sc1.fnName = "foo";
  sc1.tier = CompileTier::Instrumented;
  opt.runPipeline(*M1, sc1);
  uint64_t fooHash = 0, barHash = 0;
  unsigned fooCnt = 0, barCnt = 0;
  ASSERT_TRUE(readCounterInfo(*M1, "foo", fooHash, fooCnt));
  ASSERT_TRUE(readCounterInfo(*M1, "bar", barHash, barCnt));

  // Synthesize profiles: foo entry=100, bar entry=200 (called 2x, hot).
  InstrProfWriter Writer;
  consumeError(Writer.mergeProfileKind(InstrProfKind::IRInstrumentation));
  auto addRec = [&](const char *name, uint64_t hash, unsigned cnt,
                    uint64_t val) {
    std::vector<uint64_t> C(cnt, 0);
    C[0] = val;
    NamedInstrProfRecord Rec(name, hash, C);
    Writer.addRecord(std::move(Rec), 1, [](Error) {});
  };
  addRec("foo", fooHash, fooCnt, 100);
  addRec("bar", barHash, barCnt, 200);
  auto Buf = Writer.writeBuffer();
  ASSERT_NE(Buf, nullptr);

  // Tier-2 PGOUse + inline.
  opt.clearAnalyses();
  auto M2 = CloneModule(*M0);
  SpecializationContext sc2;
  sc2.fnName = "foo";
  sc2.tier = CompileTier::PGOUse;
  sc2.profileData = std::string(Buf->getBuffer());
  opt.runPipeline(*M2, sc2);

  // Verify bar inlined into foo: foo no longer has a call to bar.
  Function *Foo = M2->getFunction("foo");
  ASSERT_NE(Foo, nullptr);
  bool hasCallToBar = false;
  for (BasicBlock &BB : *Foo)
    for (Instruction &I : BB)
      if (auto *CI = dyn_cast<CallInst>(&I))
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "bar")
          hasCallToBar = true;
  EXPECT_FALSE(hasCallToBar);
}
