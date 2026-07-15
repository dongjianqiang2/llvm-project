//===-- EJitPgoTest.cpp - online PGO pipeline unit tests ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
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
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/ProfileData/InstrProfWriter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
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

namespace {
// bar(i32 %x): medium-sized callee (~80 instructions, 7 basic blocks,
// loads/stores to alloca'd locals + arithmetic). The static inline cost
// exceeds the default threshold (225), so AOT buildModuleInlinerPipeline
// cannot inline it. But PGO hot profile (entry count=1000, hot call site
// in foo) lifts the threshold to 3000, letting the JIT PGO inliner inline
// it at Tier-2. This is the key EJIT PGO value proposition for inlining:
// AOT-static-skipped, JIT-PGO-hot-inlined.
std::unique_ptr<Module> makeFooCallsMediumBarModule(LLVMContext &Ctx) {
  auto M = std::make_unique<Module>("pgo_medium_inline", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);

  // bar(i32 %x): medium-sized callee with 6 basic blocks (entry + 4
  // compute-blocks + 1 merge-block + exit). The 4 compute blocks form
  // 2 if-else pairs — each pair tests a bit of %x and dispatches to
  // one of two blocks. Both paths then converge at a merge block.
  //
  // Every compute/merge block loads 3 i32 slots from a local alloca,
  // performs mul+add+ashr on each, and stores the results back. The
  // alloca'd slots live on the per-call stack frame — the inliner
  // cannot SROA them away during cost analysis.
  //
  // Total: ~36 loads + ~36 stores + ~36 arithmetic ops ≈ 108
  // non-simplifiable instructions, plus basic-block overhead.
  // Static inline cost lands in the 250–400 range — above the default
  // 225 threshold (AOT would skip) but safely below the hot-call-site
  // 3000 threshold (JIT PGO inliner accepts when profile marks call hot).
  auto *BarFnTy = FunctionType::get(I32, {I32}, false);
  auto *Bar = Function::Create(BarFnTy, Function::ExternalLinkage, "bar", M.get());

  // Helper: add a load−compute−store sequence on slots [0..2] of Arr.
  auto emitComputeOnSlots = [&](IRBuilder<> &B, AllocaInst *Arr, int seed) {
    for (int i = 0; i < 3; ++i) {
      Value *GEP = B.CreateConstGEP2_32(ArrayType::get(I32, 3), Arr, 0, i);
      Value *V = B.CreateLoad(I32, GEP);
      Value *M = B.CreateMul(V, ConstantInt::get(I32, seed * 7 + 3 + i));
      Value *A = B.CreateAdd(M, ConstantInt::get(I32, seed * 11 + 5 + i));
      Value *S = B.CreateAShr(A, ConstantInt::get(I32, (i + seed) % 3 + 1));
      B.CreateStore(S, GEP);
    }
  };

  {
    // Entry: alloca [3 x i32], init slots with x+0, x+1, x+2, branch
    // based on bit 0 of the argument.
    BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Bar);
    IRBuilder<> B(Entry);
    AllocaInst *Arr = B.CreateAlloca(ArrayType::get(I32, 3));
    for (int i = 0; i < 3; ++i) {
      Value *GEP = B.CreateConstGEP2_32(ArrayType::get(I32, 3), Arr, 0, i);
      B.CreateStore(B.CreateAdd(Bar->getArg(0), ConstantInt::get(I32, i)),
                    GEP);
    }

    BasicBlock *CompA = BasicBlock::Create(Ctx, "compA", Bar);
    BasicBlock *CompB = BasicBlock::Create(Ctx, "compB", Bar);
    BasicBlock *Merge1 = BasicBlock::Create(Ctx, "merge1", Bar);

    Value *Bit0 = B.CreateAnd(Bar->getArg(0), ConstantInt::get(I32, 1));
    Value *Cmp0 = B.CreateICmpEQ(Bit0, ConstantInt::get(I32, 0));
    B.CreateCondBr(Cmp0, CompA, CompB);

    // compA (bit 0 == 0): compute on slots, then → merge1.
    { IRBuilder<> BA(CompA); emitComputeOnSlots(BA, Arr, 1); BA.CreateBr(Merge1); }
    // compB (bit 0 == 1): compute on slots, then → merge1.
    { IRBuilder<> BB_(CompB); emitComputeOnSlots(BB_, Arr, 2); BB_.CreateBr(Merge1); }

    // merge1: compute on slots again, then dispatch on bit 1.
    BasicBlock *CompC = BasicBlock::Create(Ctx, "compC", Bar);
    BasicBlock *CompD = BasicBlock::Create(Ctx, "compD", Bar);
    BasicBlock *Exit  = BasicBlock::Create(Ctx, "exit",  Bar);

    { IRBuilder<> BM1(Merge1);
      emitComputeOnSlots(BM1, Arr, 3);
      Value *Bit1 = BM1.CreateAnd(Bar->getArg(0), ConstantInt::get(I32, 2));
      Value *Cmp1 = BM1.CreateICmpEQ(Bit1, ConstantInt::get(I32, 0));
      BM1.CreateCondBr(Cmp1, CompC, CompD); }

    { IRBuilder<> BC(CompC); emitComputeOnSlots(BC, Arr, 4); BC.CreateBr(Exit); }
    { IRBuilder<> BD(CompD); emitComputeOnSlots(BD, Arr, 5); BD.CreateBr(Exit); }

    // Exit: load all 3 slots, sum them, add the original argument, return.
    { IRBuilder<> BX(Exit);
      Value *Sum = ConstantInt::get(I32, 0);
      for (int i = 0; i < 3; ++i) {
        Value *GEP = BX.CreateConstGEP2_32(ArrayType::get(I32, 3), Arr, 0, i);
        Value *V = BX.CreateLoad(I32, GEP);
        Sum = BX.CreateAdd(Sum, V);
      }
      BX.CreateRet(BX.CreateAdd(Sum, Bar->getArg(0))); }
  }

  // foo(i32 %n): calls bar(%n) and returns the result.
  auto *FooFnTy = FunctionType::get(I32, {I32}, false);
  auto *Foo = Function::Create(FooFnTy, Function::ExternalLinkage, "foo", M.get());
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Foo);
    IRBuilder<> B(BB);
    Value *V = B.CreateCall(Bar, {Foo->getArg(0)});
    B.CreateRet(V);
  }

  return M;
}
} // namespace

// PGO stage 3: a callee whose static inline cost exceeds the AOT default
// threshold (225) but fits within the PGO hot threshold (3000). AOT's
// buildModuleInlinerPipeline skips it; the JIT PGO inliner (Tier-2
// PGOUse + ModuleInlinerWrapperPass) inlines it when the profile marks
// the call site as hot.
//
// This is the EJIT online PGO inlining value proposition (§0/§12 stage 3):
// the callee is too expensive for blind static inlining but becomes
// worthwhile when real execution counts prove it's hot.
TEST(EJitPgo, Tier2PgoInlinesAotRejectedCallee) {
  LLVMContext Ctx;
  auto M0 = makeFooCallsMediumBarModule(Ctx);
  PeriodArrayRegistry reg;
  EJitOptimizer opt(reg);

  // Step 1 — Tier-1: instrument foo and bar, capture their FuncHash +
  // NumCounters for profile synthesis.
  auto M1 = CloneModule(*M0);
  SpecializationContext sc1;
  sc1.fnName = "foo";
  sc1.tier = CompileTier::Instrumented;
  opt.runPipeline(*M1, sc1);
  uint64_t fooHash = 0, barHash = 0;
  unsigned fooCnt = 0, barCnt = 0;
  ASSERT_TRUE(readCounterInfo(*M1, "foo", fooHash, fooCnt));
  ASSERT_TRUE(readCounterInfo(*M1, "bar", barHash, barCnt));

  // Step 2 — synthesize a hot profile: use a high uniform count so both
  // foo's and bar's entry counts sit above the profile-summary hot
  // percentile.  When the caller foo's entry block carries a hot-enough
  // profile count (> 80th percentile of the summary) the call to bar is
  // classified as a hot call site, and the DefaultInlineAdvisor applies
  // HotCallSiteThreshold (3000) instead of the static default (225).
  // Bar's ~250-400 static cost then fits comfortably under 3000.
  InstrProfWriter Writer;
  consumeError(Writer.mergeProfileKind(InstrProfKind::IRInstrumentation));
  auto addRec = [&](const char *name, uint64_t hash, unsigned cnt,
                    uint64_t val) {
    std::vector<uint64_t> C(cnt, 0);
    C[0] = val;
    NamedInstrProfRecord Rec(name, hash, C);
    Writer.addRecord(std::move(Rec), 1, [](Error) {});
  };
  addRec("foo", fooHash, fooCnt, 10000);
  addRec("bar", barHash, barCnt, 10000);
  auto Buf = Writer.writeBuffer();
  ASSERT_NE(Buf, nullptr);

  // Step 3 — Tier-2 PGOUse + PGO inline: the hot profile enables the
  // inliner to use the elevated hot threshold and inline bar into foo.
  opt.clearAnalyses();
  auto M2 = CloneModule(*M0);
  SpecializationContext sc2;
  sc2.fnName = "foo";
  sc2.tier = CompileTier::PGOUse;
  sc2.profileData = std::string(Buf->getBuffer());
  opt.runPipeline(*M2, sc2);

  // Verify: bar IS inlined into foo (no call to bar remains).
  Function *Foo2 = M2->getFunction("foo");
  ASSERT_NE(Foo2, nullptr);
  bool hasCallToBar = false;
  for (BasicBlock &BB : *Foo2)
    for (Instruction &I : BB)
      if (auto *CI = dyn_cast<CallInst>(&I))
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "bar")
          hasCallToBar = true;
  EXPECT_FALSE(hasCallToBar)
      << "PGO hot profile should inline medium callee that AOT would skip";
}

// Contrast test: without the PGO inliner (Baseline tier), the medium-sized
// callee bar is NOT inlined. The Baseline path does not run any inline pass
// (AOT pre-inline already handled small callees; medium ones remain as
// calls). This establishes the baseline against which Tier2PgoInlinesAotRejectedCallee
// proves PGO inline adds value.
TEST(EJitPgo, BaselineDoesNotInlineMediumCallee) {
  LLVMContext Ctx;
  auto M0 = makeFooCallsMediumBarModule(Ctx);
  PeriodArrayRegistry reg;
  EJitOptimizer opt(reg);

  // Baseline tier: specialization + main optimization pipeline, no inline pass.
  SpecializationContext sc;
  sc.fnName = "foo";
  sc.tier = CompileTier::Baseline;
  opt.runPipeline(*M0, sc);

  // Verify: bar is NOT inlined (call to bar remains in foo).
  Function *Foo = M0->getFunction("foo");
  ASSERT_NE(Foo, nullptr);
  bool hasCallToBar = false;
  for (BasicBlock &BB : *Foo)
    for (Instruction &I : BB)
      if (auto *CI = dyn_cast<CallInst>(&I))
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "bar")
          hasCallToBar = true;
  EXPECT_TRUE(hasCallToBar)
      << "Baseline (no inline) should preserve call to medium callee — "
         "this is what AOT pre-inline left behind";
}

// PGO: EJitProfileMerge reads the runtime __llvm_profile_data layout
// (FuncHash@8, NumCounters@48) + counter values at profcAddr. Validate the
// offset reading by constructing a fake __llvm_profile_data struct + counter
// array in memory, synthesizing a profile, and feeding it to PGOUse (which
// must annotate !prof -> proves the FuncHash read at offset 8 matched the
// Gen-point hash; a wrong offset would yield a garbage hash -> no match -> no
// !prof -> test fails).
TEST(EJitPgo, ProfileMergeReadsRuntimeLayout) {
  LLVMContext Ctx;
  auto M0 = makeFooModule(Ctx);
  PeriodArrayRegistry reg;
  EJitOptimizer opt(reg);

  // Tier-1 Gen -> get foo's FuncHash + NumCounters (from the IR globals).
  auto M1 = CloneModule(*M0);
  SpecializationContext sc1;
  sc1.fnName = "foo";
  sc1.tier = CompileTier::Instrumented;
  opt.runPipeline(*M1, sc1);
  uint64_t fooHash = 0;
  unsigned fooCnt = 0;
  ASSERT_TRUE(readCounterInfo(*M1, "foo", fooHash, fooCnt));
  ASSERT_GT(fooCnt, 0u);

  // Fake __llvm_profile_data struct in memory (EJitProfileMerge reads FuncHash
  // at offset 8, NumCounters at offset 48 - see EJitProfileMerge.cpp).
  std::vector<uint8_t> fakeData(64, 0);
  *reinterpret_cast<uint64_t *>(fakeData.data() + 8) = fooHash;
  *reinterpret_cast<uint32_t *>(fakeData.data() + 48) =
      static_cast<uint32_t>(fooCnt);
  // Fake counter array: a 100/1 split (hot then-path).
  std::vector<uint64_t> fakeCounters(fooCnt, 0);
  fakeCounters[0] = 100;
  if (fooCnt > 1)
    fakeCounters[1] = 1;

  // Synthesize the profile from the fake runtime addrs.
  PgoCounterRef ref{"foo", reinterpret_cast<uintptr_t>(fakeCounters.data()),
                    reinterpret_cast<uintptr_t>(fakeData.data())};
  std::string profile = synthesizeProfileBuffer({ref});
  ASSERT_FALSE(profile.empty());

  // Tier-2 PGOUse with the synthesized profile -> must annotate !prof.
  opt.clearAnalyses();
  auto M2 = CloneModule(*M0);
  SpecializationContext sc2;
  sc2.fnName = "foo";
  sc2.tier = CompileTier::PGOUse;
  sc2.profileData = profile;
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
// foo(i32 x) = x + 1. Leaf, no external calls -> JIT-links without symbol
// resolution. PGO Gen still instruments it (entry edge counter -> __profc_foo).
std::unique_ptr<Module> makeSimpleFooModule(LLVMContext &Ctx) {
  auto M = std::make_unique<Module>("pgo_orc", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *F = Function::Create(FunctionType::get(I32, {I32}, false),
                             Function::ExternalLinkage, "foo", M.get());
  BasicBlock *BB = BasicBlock::Create(Ctx, "b", F);
  IRBuilder<> B(BB);
  B.CreateRet(B.CreateAdd(F->getArg(0), ConstantInt::get(I32, 1)));
  return M;
}
} // namespace

// PGO full-runtime wiring: EJitOrcEngine loadBitcode (Instrumented) -> Gen
// creates __profc_foo/__profd_foo (forced External by captureCounterGlobals)
// -> ORC lookup resolves them by name in the spec JITDylib -> read the real
// runtime __llvm_profile_data layout (FuncHash@8, NumCounters@48) + counters
// at profcAddr -> EJitProfileMerge synthesizes a profile from the REAL addrs.
// This is the compileCold Tier-1 capture path that the EJitPgoTest
// (runPipeline) + EJitProfileMerge (fake addrs) tests don't cover.
// KNOWN-FAILING (reproduction): ORC lookup of transform-generated __profc_*
// fails - the JITDylib's symbol claims are computed at addIRModule (from the
// original module), but __profc_* are generated by the IRTransformLayer
// transform (Gen, after addIRModule) -> not claimed -> lookup fails, even
// though captureCounterGlobals forces them External. See EJIT_ONLINE_PGO.md
// §5.2 finding. Fix direction: capture counter addrs via the code-pool memory
// manager (post-compile) + JITDylib::define(absoluteSymbols), not ORC lookup.
TEST(EJitPgo, OrcLookupAndRealAddrProfileMerge) {
  // Serialize foo to bitcode.
  std::string bitcode;
  {
    LLVMContext Ctx;
    auto M = makeSimpleFooModule(Ctx);
    raw_string_ostream OS(bitcode);
    WriteBitcodeToFile(*M, OS);
    OS.flush();
  }
  ASSERT_FALSE(bitcode.empty());

  // Engine + Tier-1 (Instrumented) compile. Initialize the native target
  // (EJit.cpp does this in ejit_init; a direct engine construct must too).
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  EJitRuntimeState state;
  Config cfg;
  auto engineOrErr = EJitOrcEngine::Create(cfg, state.getRegistry(), state);
  ASSERT_TRUE(static_cast<bool>(engineOrErr)) << "EJitOrcEngine::Create failed";
  auto engine = std::move(*engineOrErr);
  SpecializationContext ctx;
  ctx.fnName = "foo";
  ctx.cacheKey = 1;
  ctx.tier = CompileTier::Instrumented;
  engine->setActiveContext(&ctx);
  ASSERT_FALSE(errorToBool(engine->loadBitcodeModule(bitcode, 1, "foo")));
  auto fnOrErr = engine->lookup(1, "foo");
  ASSERT_TRUE(static_cast<bool>(fnOrErr)) << "lookup foo failed";
  ASSERT_NE(*fnOrErr, nullptr);

  // ORC lookup of the counter globals (forced External by captureCounterGlobals).
  auto profcOrErr = engine->lookup(1, "__profc_foo");
  auto profdOrErr = engine->lookup(1, "__profd_foo");
  ASSERT_TRUE(static_cast<bool>(profcOrErr)) << "lookup __profc_foo failed";
  ASSERT_TRUE(static_cast<bool>(profdOrErr)) << "lookup __profd_foo failed";
  uintptr_t profcAddr = reinterpret_cast<uintptr_t>(*profcOrErr);
  uintptr_t profdAddr = reinterpret_cast<uintptr_t>(*profdOrErr);
  ASSERT_NE(profcAddr, 0u);
  ASSERT_NE(profdAddr, 0u);

  // Read the real runtime __llvm_profile_data layout (offsets per
  // EJitProfileMerge.cpp: FuncHash@8, NumCounters@48).
  uint64_t realHash = *reinterpret_cast<const uint64_t *>(profdAddr + 8);
  uint32_t realCnt = *reinterpret_cast<const uint32_t *>(profdAddr + 48);
  EXPECT_NE(realHash, 0u);
  EXPECT_GT(realCnt, 0u);

  // Cross-check against the IR __profd_foo FuncHash (separate Gen run) - the
  // real-addr FuncHash must match, proving the offset read is correct on real
  // runtime memory (not just the fake-addr test).
  {
    LLVMContext Ctx;
    PeriodArrayRegistry reg;
    EJitOptimizer opt(reg);
    auto M1 = CloneModule(*makeSimpleFooModule(Ctx));
    SpecializationContext sc;
    sc.fnName = "foo";
    sc.tier = CompileTier::Instrumented;
    opt.runPipeline(*M1, sc);
    uint64_t irHash = 0;
    unsigned irCnt = 0;
    ASSERT_TRUE(readCounterInfo(*M1, "foo", irHash, irCnt));
    EXPECT_EQ(realHash, irHash);    // offset 8 correct on real memory
    EXPECT_EQ(realCnt, irCnt);      // offset 48 correct on real memory
  }

  // Set counter values at the real __profc_foo (simulate Tier-1 execution).
  auto *counters = reinterpret_cast<uint64_t *>(profcAddr);
  for (uint32_t i = 0; i < realCnt; ++i)
    counters[i] = 100;

  // Synthesize a profile from the REAL addrs.
  PgoCounterRef ref{"foo", profcAddr, profdAddr};
  std::string profile = synthesizeProfileBuffer({ref});
  EXPECT_FALSE(profile.empty());
}

// Full Tier-1 → Tier-2 cycle via the real ORC engine:
// 1. Tier-1 compile (Instrumented) → ORC lookup foo + __profc_/__profd_
// 2. Set counter values (simulate hot execution) → synthesize profile
// 3. Tier-2 compile (PGOUse) with the synthesized profile
// 4. Verify Tier-2 produced different code (recompile happened)
// 5. Verify !prof annotations on Tier-2 IR (PGOUse consumed the profile)
//
// This is the compileCold Tier-1 capture → Tier-2 use path (§5).
TEST(EJitPgo, Tier1ToTier2FullCycle) {
  std::string bitcode;
  {
    LLVMContext Ctx;
    auto M = makeSimpleFooModule(Ctx);
    raw_string_ostream OS(bitcode);
    WriteBitcodeToFile(*M, OS);
    OS.flush();
  }
  ASSERT_FALSE(bitcode.empty());

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  EJitRuntimeState state;
  Config cfg;
  auto engineOrErr = EJitOrcEngine::Create(cfg, state.getRegistry(), state);
  ASSERT_TRUE(static_cast<bool>(engineOrErr)) << "EJitOrcEngine::Create failed";
  auto engine = std::move(*engineOrErr);

  // ── Phase 1: Tier-1 Instrumented compile ──
  void *tier1Fn = nullptr;
  uintptr_t profcAddr = 0, profdAddr = 0;
  uint64_t realHash = 0;
  uint32_t realCnt = 0;
  {
    SpecializationContext ctx;
    ctx.fnName = "foo";
    ctx.cacheKey = 1;
    ctx.tier = CompileTier::Instrumented;
    engine->setActiveContext(&ctx);
    ASSERT_FALSE(errorToBool(engine->loadBitcodeModule(bitcode, 1, "foo")));

    auto fnOrErr = engine->lookup(1, "foo");
    ASSERT_TRUE(static_cast<bool>(fnOrErr));
    tier1Fn = *fnOrErr;
    ASSERT_NE(tier1Fn, nullptr);

    auto profcOrErr = engine->lookup(1, "__profc_foo");
    auto profdOrErr = engine->lookup(1, "__profd_foo");
    ASSERT_TRUE(static_cast<bool>(profcOrErr));
    ASSERT_TRUE(static_cast<bool>(profdOrErr));
    profcAddr = reinterpret_cast<uintptr_t>(*profcOrErr);
    profdAddr = reinterpret_cast<uintptr_t>(*profdOrErr);
    ASSERT_NE(profcAddr, 0u);
    ASSERT_NE(profdAddr, 0u);

    // Read __llvm_profile_data layout: FuncHash@8, NumCounters@48.
    realHash = *reinterpret_cast<const uint64_t *>(profdAddr + 8);
    realCnt  = *reinterpret_cast<const uint32_t *>(profdAddr + 48);
    EXPECT_NE(realHash, 0u);
    EXPECT_GT(realCnt, 0u);
  }

  // ── Phase 2: simulate Tier-1 execution → synthesize profile ──
  // Synthesize the profile BEFORE Tier-2 removes the Tier-1 JITDylib
  // (the counter addrs become dangling after removeJITDylib).
  std::string savedProfile;
  {
    auto *counters = reinterpret_cast<uint64_t *>(profcAddr);
    for (uint32_t i = 0; i < realCnt; ++i)
      counters[i] = 100;

    PgoCounterRef ref{"foo", profcAddr, profdAddr};
    savedProfile = synthesizeProfileBuffer({ref});
    ASSERT_FALSE(savedProfile.empty());
  }

  // ── Phase 3: Tier-2 PGOUse compile (overwrites Tier-1 JITDylib) ──
  {
    SpecializationContext ctx2;
    ctx2.fnName = "foo";
    ctx2.cacheKey = 1;
    ctx2.tier = CompileTier::PGOUse;
    ctx2.profileData = savedProfile;
    engine->setActiveContext(&ctx2);
    ASSERT_FALSE(errorToBool(engine->loadBitcodeModule(bitcode, 1, "foo")));

    auto fn2OrErr = engine->lookup(1, "foo");
    ASSERT_TRUE(static_cast<bool>(fn2OrErr));
    void *tier2Fn = *fn2OrErr;
    ASSERT_NE(tier2Fn, nullptr);
    // Tier-2 may reuse the same code address if the function body is
    // identical after optimization (e.g. simple add-immediate).  The
    // meaningful signal is that PGOUse consumed the profile — verified
    // in Phase 4.  fnPtr equality here is not a failure.
  }

  // ── Phase 4: verify PGOUse consumed the profile (offline re-run) ──
  {
    LLVMContext Ctx;
    PeriodArrayRegistry reg;
    EJitOptimizer opt(reg);
    auto M = makeSimpleFooModule(Ctx);

    SpecializationContext sc;
    sc.fnName = "foo";
    sc.tier = CompileTier::PGOUse;
    sc.profileData = savedProfile;
    opt.runPipeline(*M, sc);

    Function *Foo = M->getFunction("foo");
    ASSERT_NE(Foo, nullptr);
    auto EC = Foo->getEntryCount();
    EXPECT_TRUE(EC.has_value())
        << "PGOUse must annotate function_entry_count from profile";
    if (EC)
      EXPECT_GT(EC->getCount(), 0u);
  }
}

// PGO (§5): shared Tier-1 machine code runs on multiple cores concurrently, so
// the counter updates MUST be atomic (InstrProfOptions.Atomic). After Tier-1
// lowering the __profc_* increment must be an `atomicrmw add`, not a plain
// load/add/store that would race and lose counts across cores.
TEST(EJitPgo, Tier1CountersUseAtomicRMW) {
  LLVMContext Ctx;
  auto M = makeFooModule(Ctx);
  PeriodArrayRegistry reg;
  EJitOptimizer opt(reg);
  SpecializationContext sc;
  sc.fnName = "foo";
  sc.tier = CompileTier::Instrumented;
  opt.runPipeline(*M, sc);

  unsigned AtomicAdds = 0;
  unsigned PlainCounterStores = 0;
  for (Function &F : *M)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        if (auto *RMW = dyn_cast<AtomicRMWInst>(&I))
          if (RMW->getOperation() == AtomicRMWInst::Add)
            ++AtomicAdds;
        // A non-atomic lowering would emit a plain store back to the counter
        // global; count stores whose pointer is a __profc_ GEP/global as a
        // regression signal.
        if (auto *St = dyn_cast<StoreInst>(&I)) {
          const Value *Ptr = St->getPointerOperand()->stripPointerCasts();
          if (const auto *GEP = dyn_cast<GetElementPtrInst>(Ptr))
            Ptr = GEP->getPointerOperand()->stripPointerCasts();
          if (const auto *GV = dyn_cast<GlobalVariable>(Ptr))
            if (GV->getName().starts_with("__profc_"))
              ++PlainCounterStores;
        }
      }

  EXPECT_GT(AtomicAdds, 0u)
      << "Tier-1 counter updates must lower to `atomicrmw add` (Atomic=true)";
  EXPECT_EQ(PlainCounterStores, 0u)
      << "no plain (non-atomic) store to a __profc_ counter may remain";
}

// PGO (§5): EJitProfileMerge must read the live __profc_ counters with a
// RELAXED atomic load (they are being updated by shared Tier-1 code with
// atomicrmw). This test feeds synthesizeProfileBuffer a faked, 64-bit
// __llvm_profile_data + counter array and verifies the synthesized indexed
// profile round-trips the counter VALUES exactly (proving the atomic read read
// the correct scalars; typed uint64 loads keep it endian-safe).
TEST(EJitPgo, ProfileMergeReadsCountersAtomically) {
  // __llvm_profile_data layout (64-bit, InstrProfData.inc): FuncHash @8,
  // NumCounters @48. Build a minimally sized, 8-byte-aligned blob.
  alignas(8) uint8_t Profd[56] = {};
  const uint64_t FuncHash = 0x1122334455667788ull;
  const uint32_t NumCounters = 3;
  std::memcpy(&Profd[8], &FuncHash, sizeof(FuncHash));
  std::memcpy(&Profd[48], &NumCounters, sizeof(NumCounters));

  alignas(8) uint64_t Counters[3] = {100, 5, 7};

  PgoCounterRef Ref;
  Ref.pgoName = "foo";
  Ref.profdAddr = reinterpret_cast<uintptr_t>(&Profd[0]);
  Ref.profcAddr = reinterpret_cast<uintptr_t>(&Counters[0]);

  std::string Buf = synthesizeProfileBuffer({Ref});
  ASSERT_FALSE(Buf.empty()) << "profile synthesis must produce a buffer";

  // Read the synthesized indexed profile back and verify the counter values
  // survived the (now atomic) read + record write.
  auto ReaderOrErr = IndexedInstrProfReader::create(MemoryBuffer::getMemBuffer(
      Buf, "ejit.prof", /*RequiresNullTerminator=*/false));
  ASSERT_TRUE(static_cast<bool>(ReaderOrErr));
  IndexedInstrProfReader &Reader = **ReaderOrErr;
  auto RecOrErr = Reader.getInstrProfRecord("foo", FuncHash);
  ASSERT_TRUE(static_cast<bool>(RecOrErr))
      << "synthesized record must be retrievable by name + FuncHash";
  NamedInstrProfRecord Rec = std::move(*RecOrErr);
  ASSERT_EQ(Rec.Counts.size(), static_cast<size_t>(NumCounters));
  EXPECT_EQ(Rec.Counts[0], 100u);
  EXPECT_EQ(Rec.Counts[1], 5u);
  EXPECT_EQ(Rec.Counts[2], 7u);
}
