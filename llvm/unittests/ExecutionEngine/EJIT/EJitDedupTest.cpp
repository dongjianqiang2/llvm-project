//===-- EJitDedupTest.cpp - Specialization dedup unit tests ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception.
//
//===----------------------------------------------------------------------===//
//
//  Tests for the post-pipeline IR fingerprint + dedup index
//  (EJIT_SPECIALIZATION_DEDUP.md):
//
//    * fingerprint determinism / divergence (constants, the initializer
//      blind spot StructuralHash leaves)
//    * engine-level merge: equal-content cells reuse one fnPtr (and the
//      merged pointer really executes the specialized body)
//    * soundness: an idx-dependent branch must NOT merge even when the two
//      cells' contents are equal (the design's key counterexample)
//    * content change -> fingerprint change -> no merge
//    * DryRun counts would-be merges without changing behavior
//    * recompiling an identity re-merges against its own earlier entry
//    * releaser hard gate: hasReleaser() accessors + effectiveDedupMode()
//      force-lowering
//
//===----------------------------------------------------------------------===//

#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitCompileDriver.h"
#include "llvm/ExecutionEngine/EJIT/EJitDedupIndex.h"
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#ifdef EJIT_SRE_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitTaskPool.h"
#endif
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

//===----------------------------------------------------------------------===//
// Module builders
//===----------------------------------------------------------------------===//

/// The merge workload: `i32 @entry(i32 %cellIdx) { ret i32 g_arr[%cellIdx] }`
/// where g_arr is a period array of the "cell" lifecycle and the load is
/// may_const. After specialization with cellIdx = C the body folds to
/// `ret i32 <g_arr[C]>`: two cells with equal contents produce identical IR.
static void buildCellLoadModule(LLVMContext &Ctx, Module &M) {
  M.setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  IRBuilder<> B(Ctx);
  auto *I32 = B.getInt32Ty();

  auto *ArrTy = ArrayType::get(I32, 4);
  auto *GVar = new GlobalVariable(
      M, ArrTy, /*isConstant*/ false, GlobalValue::InternalLinkage,
      ConstantAggregateZero::get(ArrTy), "g_arr");
  Metadata *ArrMDOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(I32, 4)),
  };
  GVar->setMetadata(MD_EJIT_METADATA, MDNode::get(Ctx, {MDNode::get(Ctx, ArrMDOps)}));

  FunctionType *FT = FunctionType::get(I32, {I32}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "dedup_entry", &M);
  F->arg_begin()->setName("cell_idx");

  Metadata *IndMDOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(I32, 0)),
  };
  // The entry tag matters: the JIT pipeline internalizes every non-entry
  // definition (IPSCCP propagation prep), and ORC's IR layer never registers
  // local-linkage symbols - without it lookup fails with "Symbols not found".
  Metadata *EntryMDOps[] = {MDString::get(Ctx, TAG_EJIT_ENTRY)};
  F->setMetadata(MD_EJIT_METADATA,
                 MDNode::get(Ctx, {MDNode::get(Ctx, IndMDOps),
                                   MDNode::get(Ctx, EntryMDOps)}));

  B.SetInsertPoint(BasicBlock::Create(Ctx, "entry", F));
  Value *IdxList[] = {B.getInt32(0), F->arg_begin()};
  auto *GEP = B.CreateInBoundsGEP(ArrTy, GVar, IdxList, "cell.gep");
  auto *Load = B.CreateLoad(I32, GEP, "cell.load");
  Load->setMetadata("ejit.may_const", MDNode::get(Ctx, MDString::get(Ctx, "ejit")));
  B.CreateRet(Load);
}

/// The soundness counterexample: `i32 @entry(i32 %cellIdx)` branching on the
/// index itself. Equal cell contents are irrelevant - the folded branch
/// differs per cellIdx, so the fingerprints must differ.
static void buildIdxBranchModule(LLVMContext &Ctx, Module &M) {
  M.setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  IRBuilder<> B(Ctx);
  auto *I32 = B.getInt32Ty();

  FunctionType *FT = FunctionType::get(I32, {I32}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "dedup_entry", &M);

  Metadata *IndMDOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(I32, 0)),
  };
  // The entry tag matters: the JIT pipeline internalizes every non-entry
  // definition (IPSCCP propagation prep), and ORC's IR layer never registers
  // local-linkage symbols - without it lookup fails with "Symbols not found".
  Metadata *EntryMDOps[] = {MDString::get(Ctx, TAG_EJIT_ENTRY)};
  F->setMetadata(MD_EJIT_METADATA,
                 MDNode::get(Ctx, {MDNode::get(Ctx, IndMDOps),
                                   MDNode::get(Ctx, EntryMDOps)}));

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *Zero = BasicBlock::Create(Ctx, "is_zero", F);
  BasicBlock *Other = BasicBlock::Create(Ctx, "is_other", F);
  B.SetInsertPoint(Entry);
  B.CreateCondBr(B.CreateICmpEQ(F->arg_begin(), B.getInt32(0)), Zero, Other);
  B.SetInsertPoint(Zero);
  B.CreateRet(B.getInt32(111));
  B.SetInsertPoint(Other);
  B.CreateRet(B.getInt32(222));
}

/// Fingerprint-level builder: one function plus one module-internal constant
/// global whose initializer is a parameter (the StructuralHash blind spot:
/// module-level global update hashes only the type, never the initializer).
static std::unique_ptr<Module> buildConstGlobalModule(LLVMContext &Ctx,
                                                      uint32_t InitVal) {
  auto M = std::make_unique<Module>("fp_const_global", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  IRBuilder<> B(Ctx);
  auto *I32 = B.getInt32Ty();

  auto *CG = new GlobalVariable(*M, I32, /*isConstant*/ true,
                                GlobalValue::InternalLinkage,
                                ConstantInt::get(I32, InitVal), "cg");
  (void)CG;

  FunctionType *FT = FunctionType::get(I32, {I32}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "f", M.get());
  B.SetInsertPoint(BasicBlock::Create(Ctx, "entry", F));
  B.CreateRet(B.CreateAdd(F->arg_begin(), B.getInt32(1)));
  return M;
}

static std::string toBitcode(const Module &M) {
  std::string BC;
  raw_string_ostream OS(BC);
  WriteBitcodeToFile(M, OS);
  OS.flush();
  return BC;
}

//===----------------------------------------------------------------------===//
// Fingerprint unit tests (no engine)
//===----------------------------------------------------------------------===//

TEST(EJitDedupFingerprint, IdenticalBuildsAreEqual) {
  LLVMContext C1, C2;
  auto FP1 = computeModuleFingerprint(*buildConstGlobalModule(C1, 42));
  auto FP2 = computeModuleFingerprint(*buildConstGlobalModule(C2, 42));
  EXPECT_EQ(FP1, FP2);
}

TEST(EJitDedupFingerprint, InstructionConstantDiverges) {
  // Two builds differing only in an instruction-level constant.
  LLVMContext C1, C2;
  auto M1 = buildConstGlobalModule(C1, 42);
  auto M2 = buildConstGlobalModule(C2, 42);
  // Rewrite the add's constant in M2: +1 -> +2.
  for (Function &F : *M2)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *BO = dyn_cast<BinaryOperator>(&I))
          BO->setOperand(1, ConstantInt::get(Type::getInt32Ty(C2), 2));
  auto FP1 = computeModuleFingerprint(*M1);
  auto FP2 = computeModuleFingerprint(*M2);
  EXPECT_NE(FP1, FP2);
}

TEST(EJitDedupFingerprint, InitializerBlindSpotIsCovered) {
  // Identical instructions, module-defined global initializer differs.
  // StructuralHash's module-level global update would NOT see this; the
  // fingerprint's FNV pass over initializers must.
  LLVMContext C1, C2;
  auto FP1 = computeModuleFingerprint(*buildConstGlobalModule(C1, 5));
  auto FP2 = computeModuleFingerprint(*buildConstGlobalModule(C2, 7));
  EXPECT_NE(FP1, FP2);
}

TEST(EJitDedupIndex, InsertFindFullClear) {
  EJitDedupIndex Index;
  DedupFingerprint FP;
  FP.fp1 = 0x1234;
  FP.fp2 = 0x5678;
  FP.irUnits = 42;
  int Code = 1;
  ASSERT_TRUE(Index.insert(7, FP, &Code));
  EXPECT_EQ(Index.find(7, FP), &Code);
  // Same fingerprint, different funcIndex: no cross-function merge (v1).
  EXPECT_EQ(Index.find(8, FP), nullptr);
  EXPECT_EQ(Index.stats().entries, 1u);

  DedupFingerprint Other = FP;
  Other.fp2 = 0xaaaa;
  EXPECT_EQ(Index.find(7, Other), nullptr);

  Index.clear();
  EXPECT_EQ(Index.size(), 0u);
  EXPECT_EQ(Index.find(7, FP), nullptr);
}

//===----------------------------------------------------------------------===//
// Engine-level dedup tests
//===----------------------------------------------------------------------===//

class EJitDedupEngineTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
  }

  struct EngineFixture {
    EJitRuntimeState State;
    std::unique_ptr<EJitOrcEngine> Engine;
  };

  // Registry arrays live in the fixture; the mock cells must outlive it.
  struct Cells {
    int32_t Data[4] = {7, 7, 9, 11};
  };

  // EJitRuntimeState holds a mutex (non-movable), so the fixture is heap-held.
  std::unique_ptr<EngineFixture> makeEngine(Cells &cells, Config Cfg = Config()) {
    auto Fx = std::make_unique<EngineFixture>();
    Fx->State.getRegistry().registerArray("cell", "g_arr", cells.Data, 4);
    auto Eng = EJitOrcEngine::Create(Cfg, Fx->State.getRegistry(), Fx->State);
    EXPECT_TRUE(static_cast<bool>(Eng));
    Fx->Engine = std::move(*Eng);
    return Fx;
  }

  // cacheKey layout matches production: (funcIndex << 32) | packed dims.
  static uint64_t key(uint32_t funcIdx, uint8_t cell) {
    return (uint64_t(funcIdx) << 32) | cell;
  }

  EJitOrcEngine::SpecializeResult
  compileCell(EJitOrcEngine &Eng, uint64_t CacheKey, uint32_t funcIdx,
              uint8_t Cell, StringRef Bitcode, DedupMode Mode) {
    SpecializationContext Ctx;
    Ctx.fnName = "dedup_entry";
    Ctx.cacheKey = CacheKey;
    Ctx.dimensions.push_back({"cell", Cell});
    Eng.setActiveContext(&Ctx);
    auto R = Eng.specializeAndResolve(Bitcode, CacheKey, funcIdx,
                                      "dedup_entry", Mode);
    Eng.setActiveContext(nullptr);
    if (!R) {
      ADD_FAILURE() << "specializeAndResolve failed: "
                    << toString(R.takeError());
      return {nullptr, false};
    }
    return *R;
  }
};

TEST_F(EJitDedupEngineTest, EqualContentCellsMerge) {
  Cells cells; // cell 0 and cell 1 hold the same value 7
  auto Fx = makeEngine(cells);
  std::string BC;
  {
    LLVMContext Ctx;
    Module M("dedup_cellload", Ctx);
    buildCellLoadModule(Ctx, M);
    BC = toBitcode(M);
  }

  auto R0 = compileCell(*Fx->Engine, key(1, 0), 1, 0, BC, DedupMode::On);
  ASSERT_NE(R0.fnPtr, nullptr);
  EXPECT_FALSE(R0.deduped);

  auto R1 = compileCell(*Fx->Engine, key(1, 1), 1, 1, BC, DedupMode::On);
  ASSERT_NE(R1.fnPtr, nullptr);
  EXPECT_TRUE(R1.deduped);
  EXPECT_EQ(R1.fnPtr, R0.fnPtr);

  EXPECT_EQ(Fx->Engine->dedupIndex().stats().merges, 1u);
  EXPECT_EQ(Fx->Engine->dedupIndex().stats().entries, 1u);

  // The merged pointer really runs the specialized body: both cells folded
  // to `ret i32 7`.
  auto *Fn = reinterpret_cast<int32_t (*)(int32_t)>(R1.fnPtr);
  EXPECT_EQ(Fn(123), 7);
  EXPECT_EQ(reinterpret_cast<int32_t (*)(int32_t)>(R0.fnPtr)(456), 7);
}

TEST_F(EJitDedupEngineTest, IdxBranchDoesNotMerge) {
  // Design doc 5.4 counterexample: the function branches on the cell index
  // itself. Cell contents are irrelevant; the folded control flow differs.
  Cells cells;
  auto Fx = makeEngine(cells);
  std::string BC;
  {
    LLVMContext Ctx;
    Module M("dedup_idxbranch", Ctx);
    buildIdxBranchModule(Ctx, M);
    BC = toBitcode(M);
  }

  auto R0 = compileCell(*Fx->Engine, key(2, 0), 2, 0, BC, DedupMode::On);
  auto R1 = compileCell(*Fx->Engine, key(2, 1), 2, 1, BC, DedupMode::On);
  ASSERT_NE(R0.fnPtr, nullptr);
  ASSERT_NE(R1.fnPtr, nullptr);
  EXPECT_FALSE(R1.deduped);
  EXPECT_NE(R1.fnPtr, R0.fnPtr);
  EXPECT_EQ(Fx->Engine->dedupIndex().stats().merges, 0u);

  // Cell 0 folds to 111, cell 1 to 222 - merging these would be wrong-code.
  EXPECT_EQ(reinterpret_cast<int32_t (*)(int32_t)>(R0.fnPtr)(0), 111);
  EXPECT_EQ(reinterpret_cast<int32_t (*)(int32_t)>(R1.fnPtr)(0), 222);
}

TEST_F(EJitDedupEngineTest, ContentChangeDiverges) {
  Cells cells;
  auto Fx = makeEngine(cells);
  std::string BC;
  {
    LLVMContext Ctx;
    Module M("dedup_cellload2", Ctx);
    buildCellLoadModule(Ctx, M);
    BC = toBitcode(M);
  }

  auto R0 = compileCell(*Fx->Engine, key(3, 0), 3, 0, BC, DedupMode::On);
  ASSERT_NE(R0.fnPtr, nullptr);

  // Same cells, then cell 1's content changes: the specialization for cell 1
  // must NOT merge with the cell 0 entry anymore.
  cells.Data[1] = 8;
  auto R1 = compileCell(*Fx->Engine, key(3, 1), 3, 1, BC, DedupMode::On);
  ASSERT_NE(R1.fnPtr, nullptr);
  EXPECT_FALSE(R1.deduped);
  EXPECT_NE(R1.fnPtr, R0.fnPtr);
  EXPECT_EQ(reinterpret_cast<int32_t (*)(int32_t)>(R1.fnPtr)(0), 8);
}

TEST_F(EJitDedupEngineTest, DryRunCountsButDoesNotMerge) {
  Cells cells;
  auto Fx = makeEngine(cells);
  std::string BC;
  {
    LLVMContext Ctx;
    Module M("dedup_cellload3", Ctx);
    buildCellLoadModule(Ctx, M);
    BC = toBitcode(M);
  }

  auto R0 = compileCell(*Fx->Engine, key(4, 0), 4, 0, BC, DedupMode::DryRun);
  auto R1 = compileCell(*Fx->Engine, key(4, 1), 4, 1, BC, DedupMode::DryRun);
  ASSERT_NE(R0.fnPtr, nullptr);
  ASSERT_NE(R1.fnPtr, nullptr);
  EXPECT_FALSE(R0.deduped);
  EXPECT_FALSE(R1.deduped);
  EXPECT_NE(R1.fnPtr, R0.fnPtr);

  const auto &St = Fx->Engine->dedupIndex().stats();
  EXPECT_EQ(St.wouldMerge, 1u);
  EXPECT_EQ(St.merges, 0u);
  // One entry, not two: the second (identical) fingerprint refreshes the
  // first entry rather than adding a duplicate.
  EXPECT_EQ(St.entries, 1u);
}

TEST_F(EJitDedupEngineTest, RecompileOfSameIdentityReMerges) {
  // Eviction re-drives compilation of an identity whose canonical code is
  // already indexed: the recompile re-merges (idempotently) instead of
  // burning another code copy.
  Cells cells;
  auto Fx = makeEngine(cells);
  std::string BC;
  {
    LLVMContext Ctx;
    Module M("dedup_cellload4", Ctx);
    buildCellLoadModule(Ctx, M);
    BC = toBitcode(M);
  }

  auto R0 = compileCell(*Fx->Engine, key(5, 0), 5, 0, BC, DedupMode::On);
  ASSERT_NE(R0.fnPtr, nullptr);
  auto R0b = compileCell(*Fx->Engine, key(5, 0), 5, 0, BC, DedupMode::On);
  EXPECT_TRUE(R0b.deduped);
  EXPECT_EQ(R0b.fnPtr, R0.fnPtr);
}

TEST_F(EJitDedupEngineTest, OffModeKeepsIndexEmpty) {
  Cells cells;
  auto Fx = makeEngine(cells);
  std::string BC;
  {
    LLVMContext Ctx;
    Module M("dedup_cellload5", Ctx);
    buildCellLoadModule(Ctx, M);
    BC = toBitcode(M);
  }

  auto R0 = compileCell(*Fx->Engine, key(6, 0), 6, 0, BC, DedupMode::Off);
  auto R1 = compileCell(*Fx->Engine, key(6, 1), 6, 1, BC, DedupMode::Off);
  ASSERT_NE(R0.fnPtr, nullptr);
  ASSERT_NE(R1.fnPtr, nullptr);
  EXPECT_NE(R1.fnPtr, R0.fnPtr);
  EXPECT_EQ(Fx->Engine->dedupIndex().size(), 0u);
}

//===----------------------------------------------------------------------===//
// Releaser hard gate (EJIT_SPECIALIZATION_DEDUP.md 5.5)
//===----------------------------------------------------------------------===//

#ifdef EJIT_SRE_TASKPOOL

static void mockRelease(void *, void *) {}

TEST(EJitDedupReleaserGate, TaskPoolHasReleaserTracksWiring) {
  EJitSwitchController Dummy;
  EJitTaskPoolCache Cache(Dummy);
  EXPECT_FALSE(Cache.hasReleaser());
  Cache.setReleaser(&mockRelease, nullptr);
  EXPECT_TRUE(Cache.hasReleaser());
  Cache.setReleaser(nullptr, nullptr);
  EXPECT_FALSE(Cache.hasReleaser());
}

TEST(EJitDedupReleaserGate, EffectiveDedupModeLoweredWhenReleaserWired) {
  EJitRuntimeState State;
  EJitModuleLoader Loader;
  Config Cfg;
  Cfg.dedupMode = DedupMode::On;
  EJitCompileDriver Driver(Cfg, State, Loader);
  EXPECT_EQ(Driver.effectiveDedupMode(), DedupMode::On);

  Driver.taskPool()->setReleaser(&mockRelease, nullptr);
  EXPECT_EQ(Driver.effectiveDedupMode(), DedupMode::Off);
  // Unwiring re-arms dedup.
  Driver.taskPool()->setReleaser(nullptr, nullptr);
  EXPECT_EQ(Driver.effectiveDedupMode(), DedupMode::On);
}

#endif // EJIT_SRE_TASKPOOL

} // namespace
