//===-- EJitLocalFreeDimTest.cpp - Local witness regressions --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitCompileDriver.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <chrono>
#include <thread>

using namespace llvm;
using namespace llvm::ejit;

namespace {
struct Slot {
  int32_t mode;
  int32_t payload;
};
using Cells = Slot[2][10];

std::unique_ptr<Module> makeLocalModule(LLVMContext &C, StringRef Setup = "",
                                        StringRef Input = "%raw",
                                        StringRef Index = "%local") {
  std::string IR = R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
    %Slot = type { i32, i32 }
    @local_cells = external global [2 x [10 x %Slot]], !ejit.metadata !0
    @local_runtime_slot = external global i64
    declare i64 @local_initializer(i64)
    declare i64 @__ejit_local_free_dim_v1_i64(i64) #0
    define i32 @local_work(i32 %cell, i64 %raw) !ejit.metadata !4 {
    entry:
  )";
  IR += Setup.str();
  IR += "\n%local = call i64 @__ejit_local_free_dim_v1_i64(i64 " + Input.str() +
        ")\n";
  IR += "%ci = zext i32 %cell to i64\n";
  IR += "%chosen = getelementptr inbounds [2 x [10 x %Slot]], ptr "
        "@local_cells, i64 0, i64 %ci, i64 " +
        Index.str() + "\n";
  IR += R"(
    %mode = load i32, ptr %chosen, align 4, !ejit.may_const !7
    %payloadp = getelementptr %Slot, ptr %chosen, i64 0, i32 1
    %payload = load i32, ptr %payloadp, align 4
    %siblingp = getelementptr inbounds [2 x [10 x %Slot]], ptr @local_cells, i64 0, i64 %ci, i64 %raw
    %sibling = load i32, ptr %siblingp, align 4, !ejit.may_const !7
    %scaled = mul i32 %mode, 100
    %a = add i32 %scaled, %payload
    %result = add i32 %a, %sibling
    ret i32 %result
    }
    attributes #0 = { nounwind willreturn memory(none) nocallback nosync "ejit.local_free_dim"="v1" }
    !0 = !{!1, !2}
    !1 = !{!"ejit_period_arr", !"cell", i64 2}
    !2 = !{!"ejit_may_const_field", i64 0}
    !4 = distinct !{!5, !6}
    !5 = !{!"ejit_entry"}
    !6 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !7 = !{}
  )";
  SMDiagnostic Error;
  auto M = parseAssemblyString(IR, Error, C);
  if (!M)
    Error.print("local-free-dim-test", errs());
  return M;
}

void initializeCells(Cells &Data) {
  for (unsigned Cell = 0; Cell != 2; ++Cell)
    for (unsigned I = 0; I != 10; ++I)
      Data[Cell][I] = {Cell ? 11 : 7, static_cast<int32_t>(100 * Cell + I)};
}

unsigned countLoads(Function &F) {
  unsigned Count = 0;
  for (Instruction &I : instructions(F))
    Count += isa<LoadInst>(I);
  return Count;
}

void expectNoMarkers(Module &M) {
  EXPECT_FALSE(verifyModule(M, &errs()));
  for (Function &F : M)
    EXPECT_FALSE(isEJitLocalFreeDimFunction(F));
}

void optimizeLocal(Module &M, PeriodArrayRegistry &Registry, unsigned Cell) {
  EJitOptimizer Optimizer(Registry);
  SpecializationContext Context;
  Context.fnName = "local_work";
  Context.dimensions.push_back({"cell", static_cast<uint8_t>(Cell)});
  Optimizer.runPipeline(M, Context);
}

static unsigned InitializerCalls;
extern "C" uint64_t localInitializer(uint64_t Slot) {
  ++InitializerCalls;
  return Slot;
}

std::string serialize(Module &M) {
  std::string Bitcode;
  raw_string_ostream OS(Bitcode);
  WriteBitcodeToFile(M, OS);
  return Bitcode;
}
} // namespace

TEST(EJitLocalFreeDim, DependencyQueryVisitsSharedDAGOnce) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("local_work");
  Instruction *Ret = F->getEntryBlock().getTerminator();
  Value *Root = F->getArg(1);
  for (unsigned I = 0; I != 16; ++I)
    Root = BinaryOperator::CreateAdd(Root, Root, "diamond", Ret->getIterator());

  // A validated marker exists elsewhere, but none is reachable from this DAG.
  // The old recursive walk made 131071 visits to these same 17 values.
  unsigned Visited = 0;
  EXPECT_FALSE(usesEJitLocalFreeDim(Root, true, &Visited));
  EXPECT_EQ(Visited, 17u);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(EJitLocalFreeDim, DependencyQuerySkipsMarkerFreeGraphs) {
  LLVMContext C;
  Module M("marker-free", C);
  auto *FT =
      FunctionType::get(Type::getInt64Ty(C), {Type::getInt64Ty(C)}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "plain", M);
  auto *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *Root = F->getArg(0);
  for (unsigned I = 0; I != 64; ++I)
    Root = B.CreateAdd(Root, Root);
  B.CreateRet(Root);

  // Parameter assumptions also take the no-local-marker fast path.
  EJitFreeDimAssumptions Assumed;
  Assumed.values[F->getArg(0)] = 0;
  unsigned Visited = 123;
  EXPECT_FALSE(usesEJitLocalFreeDim(Root, Assumed.hasLocalMarkers, &Visited));
  EXPECT_EQ(Visited, 0u);
  EXPECT_FALSE(verifyModule(M, &errs()));
}

TEST(EJitLocalFreeDim, DependencyQueryPreservesDepthAndShorterPaths) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("local_work");
  Instruction *Ret = F->getEntryBlock().getTerminator();
  Value *Marker = F->getValueSymbolTable()->lookup("local");
  ASSERT_TRUE(isEJitLocalFreeDim(Marker));

  Value *AtLimit = Marker;
  for (unsigned I = 0; I != 16; ++I)
    AtLimit =
        BinaryOperator::CreateAdd(AtLimit, AtLimit, "deep", Ret->getIterator());
  unsigned Visited = 0;
  EXPECT_TRUE(usesEJitLocalFreeDim(AtLimit, true, &Visited));
  EXPECT_EQ(Visited, 17u);
  Value *TooDeep = BinaryOperator::CreateAdd(AtLimit, AtLimit, "too.deep",
                                             Ret->getIterator());
  EXPECT_FALSE(usesEJitLocalFreeDim(TooDeep, true, &Visited));
  EXPECT_EQ(Visited, 17u);

  Value *Shared = BinaryOperator::CreateAdd(Marker, F->getArg(1), "shared",
                                            Ret->getIterator());
  Value *LongPath = Shared;
  for (unsigned I = 0; I != 15; ++I)
    LongPath = BinaryOperator::CreateAdd(LongPath, LongPath, "long.path",
                                         Ret->getIterator());
  Value *Mixed =
      BinaryOperator::CreateAdd(LongPath, Shared, "mixed", Ret->getIterator());
  // DFS reaches Shared at depth16 first. Caching that negative by Value alone
  // would hide the valid two-edge path through the second operand.
  EXPECT_TRUE(usesEJitLocalFreeDim(Mixed, true, &Visited));
  EXPECT_LE(Visited, 6u);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(EJitLocalFreeDim, DependencyQueryHandlesCyclicSSA) {
  LLVMContext C;
  Module M("cyclic", C);
  auto *I64 = Type::getInt64Ty(C);
  auto *FT = FunctionType::get(I64, {I64, Type::getInt1Ty(C)}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "loop", M);
  auto *Entry = BasicBlock::Create(C, "entry", F);
  auto *Loop = BasicBlock::Create(C, "loop", F);
  auto *Exit = BasicBlock::Create(C, "exit", F);
  IRBuilder<> B(Entry);
  Value *Marker = B.CreateCall(getEJitLocalFreeDim(M, I64), {F->getArg(0)});
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  auto *Plain = B.CreatePHI(I64, 2);
  auto *Marked = B.CreatePHI(I64, 2);
  Value *PlainNext = B.CreateAdd(Plain, Plain);
  Value *MarkedNext = B.CreateAdd(Marked, Marked);
  Plain->addIncoming(F->getArg(0), Entry);
  Plain->addIncoming(PlainNext, Loop);
  Marked->addIncoming(Marker, Entry);
  Marked->addIncoming(MarkedNext, Loop);
  B.CreateCondBr(F->getArg(1), Loop, Exit);
  B.SetInsertPoint(Exit);
  B.CreateRet(Plain);
  EXPECT_FALSE(verifyModule(M, &errs()));

  unsigned Visited = 0;
  EXPECT_FALSE(usesEJitLocalFreeDim(Plain, true, &Visited));
  EXPECT_EQ(Visited, 3u);
  EXPECT_TRUE(usesEJitLocalFreeDim(Marked, true, &Visited));
  EXPECT_EQ(Visited, 2u);
}

TEST(EJitLocalFreeDim, DependencyQueryRevalidatesAfterIRMutation) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("local_work");
  Instruction *Ret = F->getEntryBlock().getTerminator();
  auto *Marker = cast<CallInst>(F->getValueSymbolTable()->lookup("local"));
  auto *Root = BinaryOperator::CreateAdd(F->getArg(1), F->getArg(1), "root",
                                         Ret->getIterator());
  EXPECT_FALSE(usesEJitLocalFreeDim(Root, true));
  Root->setOperand(0, Marker);
  EXPECT_TRUE(usesEJitLocalFreeDim(Root, true));

  // An apparent marker with the wrong protocol version is not authorization.
  Function *Decl = Marker->getCalledFunction();
  Decl->addFnAttr("ejit.local_free_dim", "v2");
  EXPECT_FALSE(usesEJitLocalFreeDim(Root, true));
  Decl->addFnAttr("ejit.local_free_dim", "v1");
  EXPECT_TRUE(usesEJitLocalFreeDim(Root, true));

  Marker->replaceAllUsesWith(F->getArg(1));
  Marker->eraseFromParent();
  EXPECT_FALSE(usesEJitLocalFreeDim(Root, true));
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST(EJitLocalFreeDim, ReadsLengthDelimitedNonterminatedEmbeddedBitcode) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  M->getFunction("local_work")->setName("local_raw_buffer_246");
  std::string Bitcode = serialize(*M);
  std::vector<uint8_t> Bytes(Bitcode.begin(), Bitcode.end());
  Bytes.push_back(0x7f); // The byte after the registered span is NOT a NUL.
  EJitLifecycleRegistry::instance().resolveAssign("cell");
  EJitModuleLoader Loader;
  ASSERT_TRUE(Loader.registerBitcode("local_raw_buffer_246", Bytes.data(),
                                     Bitcode.size()));
  uint32_t Index =
      EJitFuncRegistry::instance().resolveAssign("local_raw_buffer_246");
  const auto &Meta = Loader.getOrCacheFuncMeta(Index);
  EXPECT_EQ(Meta.dimCount, 1u);
  EXPECT_EQ(Meta.periodNames[0], "cell");
}

TEST(EJitLocalFreeDim, BaselineMixedSameSSAAndCellIndependence) {
  Cells Data;
  initializeCells(Data);
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "local_cells", Data, 2);
  for (unsigned Cell = 0; Cell != 2; ++Cell) {
    LLVMContext C;
    auto M = makeLocalModule(C);
    ASSERT_NE(M, nullptr);
    optimizeLocal(*M, Registry, Cell);
    expectNoMarkers(*M);
    auto *F = M->getFunction("local_work");
    ASSERT_NE(F, nullptr);
    EXPECT_EQ(countLoads(*F),
              2u); // payload AND unmarked sibling mode stay live
    EXPECT_GT(F->getArg(1)->getNumUses(), 0u);
    bool Baked = false;
    for (Instruction &I : instructions(*F))
      for (Value *Op : I.operand_values())
        if (auto *CI = dyn_cast<ConstantInt>(Op))
          Baked |= CI->getSExtValue() == (Cell ? 1100 : 700);
    EXPECT_TRUE(Baked);
  }
}

#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
TEST(EJitLocalFreeDim, AuditCleanupRemovesNonzeroConstantGEPSites) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  PeriodArrayRegistry Registry;
  EJitStructFieldPass Pass(Registry);
  Pass.initFromModule(*M);
  EXPECT_EQ(Pass.instrumentMayConstLoadSites(*M).size(), 2u);
  EJitStructFieldPass::removeMayConstLoadInstrumentation(*M);
  EXPECT_EQ(M->getGlobalVariable(EJitStructFieldPass::MayConstCounterName),
            nullptr);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}
#endif

TEST(EJitLocalFreeDim, RealO2ExtractionKeepsBoundaryAndNoMarkerRegistration) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerLoopAnalyses(LAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  EJitRegisterBitcodePass().run(*M, MAM);
  auto *GV = M->getGlobalVariable("__ejit_bitcode", true);
  ASSERT_NE(GV, nullptr);
  auto *Bytes = dyn_cast<ConstantDataArray>(GV->getInitializer());
  ASSERT_NE(Bytes, nullptr);
  auto Embedded = parseBitcodeFile(
      MemoryBufferRef(Bytes->getRawDataValues(), "local-embedded"), C);
  ASSERT_TRUE(static_cast<bool>(Embedded));
  EXPECT_NE((*Embedded)->getFunction("__ejit_local_free_dim_v1_i64"), nullptr);
  Cells Data;
  initializeCells(Data);
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "local_cells", Data, 2);
  optimizeLocal(**Embedded, Registry, 1);
  EXPECT_EQ(countLoads(*(*Embedded)->getFunction("local_work")), 2u);
  EJitLowerFreeDimPass().run(*M, MAM);
  expectNoMarkers(*M);
}

TEST(EJitLocalFreeDim, OrdinaryStoreCallAndBranchKeepRealOffset) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("local_work");
  auto *Marker = cast<Instruction>(F->getValueSymbolTable()->lookup("local"));
  auto *Payload =
      cast<Instruction>(F->getValueSymbolTable()->lookup("payloadp"));
  auto *Ret = cast<ReturnInst>(F->back().getTerminator());
  Value *Result = Ret->getReturnValue();
  BasicBlock *StoreBB = BasicBlock::Create(C, "write", F);
  BasicBlock *ExitBB = BasicBlock::Create(C, "exit", F);
  IRBuilder<> B(Ret);
  auto Sink = M->getOrInsertFunction(
      "local_sink", FunctionType::get(B.getVoidTy(), {B.getInt64Ty()}, false));
  B.CreateCall(Sink, {Marker});
  B.CreateCondBr(B.CreateICmpNE(Marker, B.getInt64(0)), StoreBB, ExitBB);
  Ret->eraseFromParent();
  B.SetInsertPoint(StoreBB);
  B.CreateStore(B.getInt32(41), Payload);
  B.CreateBr(ExitBB);
  B.SetInsertPoint(ExitBB);
  B.CreateRet(Result);
  Cells Data;
  initializeCells(Data);
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "local_cells", Data, 2);
  optimizeLocal(*M, Registry, 1);
  expectNoMarkers(*M);
  unsigned Stores = 0, Calls = 0, Branches = 0;
  for (Instruction &I : instructions(*F)) {
    if (auto *Store = dyn_cast<StoreInst>(&I)) {
      ++Stores;
      EXPECT_FALSE(isa<Constant>(Store->getPointerOperand()));
    }
    if (auto *Call = dyn_cast<CallInst>(&I))
      if (Call->getCalledFunction() &&
          Call->getCalledFunction()->getName() == "local_sink") {
        ++Calls;
        EXPECT_EQ(Call->getArgOperand(0), F->getArg(1));
      }
    if (auto *Branch = dyn_cast<BranchInst>(&I))
      Branches += Branch->isConditional();
  }
  EXPECT_EQ(Stores, 1u);
  EXPECT_EQ(Calls, 1u);
  EXPECT_EQ(Branches, 1u);
}

TEST(EJitLocalFreeDim, SideEffectVolatileAtomicInitializersRemain) {
  Cells Data;
  initializeCells(Data);
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "local_cells", Data, 2);
  for (StringRef Setup :
       {"%seed = call i64 @local_initializer(i64 %raw)",
        "%seed = load volatile i64, ptr @local_runtime_slot, align 8",
        "%seed = load atomic i64, ptr @local_runtime_slot monotonic, align "
        "8"}) {
    LLVMContext C;
    auto M = makeLocalModule(C, Setup, "%seed");
    ASSERT_NE(M, nullptr);
    optimizeLocal(*M, Registry, 0);
    expectNoMarkers(*M);
    auto *F = M->getFunction("local_work");
    bool Survives = false;
    for (Instruction &I : instructions(*F)) {
      if (auto *Call = dyn_cast<CallInst>(&I))
        Survives |= Call->getCalledFunction() &&
                    Call->getCalledFunction()->getName() == "local_initializer";
      if (auto *Load = dyn_cast<LoadInst>(&I))
        Survives |= Load->isVolatile() || Load->isAtomic();
    }
    EXPECT_TRUE(Survives);
  }
}

TEST(EJitLocalFreeDim, UntrackedConfigurationLifecycleKeepsLoad) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("local_work");
  F->getArg(0)->replaceAllUsesWith(
      ConstantInt::get(F->getArg(0)->getType(), 1));
  IRBuilder<> B(C);
  auto *Entry = MDNode::get(C, {MDString::get(C, TAG_EJIT_ENTRY)});
  auto *Other = MDNode::get(C, {MDString::get(C, TAG_EJIT_PERIOD_ARR_IND),
                                MDString::get(C, "other"),
                                ConstantAsMetadata::get(B.getInt32(0))});
  F->setMetadata(MD_EJIT_METADATA, MDNode::get(C, {Entry, Other}));
  Cells Data;
  initializeCells(Data);
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "local_cells", Data, 2);
  optimizeLocal(*M, Registry, 1);
  expectNoMarkers(*M);
  EXPECT_EQ(countLoads(*F), 3u);
}

TEST(EJitLocalFreeDim, InvalidWitnessCannotCrossIntoPreviousCell) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("local_work");
  auto *Marker = cast<Instruction>(F->getValueSymbolTable()->lookup("local"));
  auto *Ptr =
      cast<GetElementPtrInst>(F->getValueSymbolTable()->lookup("chosen"));
  IRBuilder<> B(Ptr);
  Ptr->setOperand(3, B.CreateSub(Marker, B.getInt64(1), "bad"));
  Cells Data;
  initializeCells(Data);
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "local_cells", Data, 2);
  optimizeLocal(*M, Registry, 1);
  expectNoMarkers(*M);
  EXPECT_EQ(countLoads(*M->getFunction("local_work")), 3u);
}

TEST(EJitLocalFreeDim, UnknownExtentAndOverflowSafelyMiss) {
  for (bool Overflow : {false, true}) {
    LLVMContext C;
    auto M = makeLocalModule(C);
    ASSERT_NE(M, nullptr);
    Cells Data;
    initializeCells(Data);
    PeriodArrayRegistry Registry;
    if (Overflow)
      Registry.registerArray("cell", "local_cells", Data, 2);
    if (Overflow) {
      Function *F = M->getFunction("local_work");
      auto *Marker =
          cast<Instruction>(F->getValueSymbolTable()->lookup("local"));
      auto *Ptr =
          cast<GetElementPtrInst>(F->getValueSymbolTable()->lookup("chosen"));
      IRBuilder<> B(Ptr);
      Ptr->setOperand(3, B.CreateAdd(Marker, B.getInt64(INT64_MAX)));
    }
    optimizeLocal(*M, Registry, 1);
    expectNoMarkers(*M);
    EXPECT_EQ(countLoads(*M->getFunction("local_work")), 3u);
  }
}

TEST(EJitLocalFreeDim, LocalAndParameterWitnessesCombineWithCellTrpBounds) {
  LLVMContext C;
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
    declare i64 @__ejit_local_free_dim_v1_i64(i64) #0
    define i32 @local_bound(i8 %cell, i8 %trp, i32 %free,
                            i64 %raw, ptr %cp, ptr %tp) !ejit.metadata !0 {
      %a = call i64 @__ejit_local_free_dim_v1_i64(i64 %raw)
      %b = call i64 @__ejit_local_free_dim_v1_i64(i64 %raw)
      %sum = add i64 %a, %b
      %param = zext i32 %free to i64
      %idx = add i64 %sum, %param
      %p = getelementptr [5 x i32], ptr %cp, i64 0, i64 %idx
      %q = getelementptr [5 x i32], ptr %tp, i64 0, i64 %idx
      %cv = load i32, ptr %p, align 4, !ejit.may_const !8
      %tv = load i32, ptr %q, align 4, !ejit.may_const !8
      %frozen = add i32 %cv, %tv
      %live = trunc i64 %raw to i32
      %out = add i32 %frozen, %live
      ret i32 %out
    }
    attributes #0 = { nounwind willreturn memory(none) nocallback nosync "ejit.local_free_dim"="v1" }
    !0 = distinct !{!1, !2, !3, !4, !5, !6}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
    !4 = !{!"ejit_free_dim", !"", i32 2}
    !5 = !{!"ejit_bound_ptr", !"cell", i32 4, i64 20, !7}
    !6 = !{!"ejit_bound_ptr", !"trp", i32 5, i64 20, !7}
    !7 = !{i64 0, i64 4}
    !8 = !{}
  )",
                               Err, C);
  ASSERT_NE(M, nullptr) << Err.getMessage().str();
  uint32_t Cell[5] = {13, 13, 13, 13, 13};
  uint32_t Trp[5] = {29, 29, 29, 29, 29};
  PeriodArrayRegistry Registry;
  EJitOptimizer Optimizer(Registry);
  SpecializationContext Context;
  Context.fnName = "local_bound";
  Context.dimensions.push_back({"cell", 1});
  Context.dimensions.push_back({"trp", 2});
  Context.boundPointers.push_back(
      {reinterpret_cast<const uint8_t *>(Cell), sizeof(Cell), 4, 1});
  Context.boundPointers.push_back(
      {reinterpret_cast<const uint8_t *>(Trp), sizeof(Trp), 5, 2});
  Optimizer.runPipeline(*M, Context);
  expectNoMarkers(*M);
  Function *F = M->getFunction("local_bound");
  EXPECT_EQ(countLoads(*F), 0u);
  EXPECT_GT(F->getArg(3)->getNumUses(), 0u);
  bool Baked42 = false;
  for (Instruction &I : instructions(*F))
    for (Value *Op : I.operand_values())
      if (auto *CI = dyn_cast<ConstantInt>(Op))
        Baked42 |= CI->getZExtValue() == 42u;
  EXPECT_TRUE(Baked42);
}

TEST(EJitLocalFreeDim, MisalignedOrUnmarkedFieldWitnessKeepsLoad) {
  for (unsigned Offset : {1u, 4u}) {
    LLVMContext C;
    auto M = makeLocalModule(C);
    ASSERT_NE(M, nullptr);
    auto *F = M->getFunction("local_work");
    auto *Mode = cast<LoadInst>(F->getValueSymbolTable()->lookup("mode"));
    IRBuilder<> B(Mode);
    Mode->setOperand(0, B.CreateInBoundsGEP(B.getInt8Ty(),
                                            Mode->getPointerOperand(),
                                            B.getInt64(Offset)));
    Cells Data;
    initializeCells(Data);
    PeriodArrayRegistry Registry;
    Registry.registerArray("cell", "local_cells", Data, 2);
    F->getArg(0)->replaceAllUsesWith(
        ConstantInt::get(F->getArg(0)->getType(), 1));
    EJitStructFieldPass Pass(Registry);
    Pass.initFromModule(*M);
    FunctionAnalysisManager AM;
    EXPECT_TRUE(Pass.run(*F, AM).areAllPreserved());
    lowerEJitLocalFreeDims(*M);
    expectNoMarkers(*M);
    EXPECT_EQ(countLoads(*F), 3u);
  }
}

TEST(EJitLocalFreeDim, UnsupportedMixedSelectKeepsRealLoad) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("local_work");
  auto *Marker = cast<Instruction>(F->getValueSymbolTable()->lookup("local"));
  auto *Ptr =
      cast<GetElementPtrInst>(F->getValueSymbolTable()->lookup("chosen"));
  IRBuilder<> B(Ptr);
  Value *Cond = B.CreateICmpEQ(F->getArg(1), B.getInt64(1));
  Ptr->setOperand(3, B.CreateSelect(Cond, Marker, F->getArg(1)));
  Cells Data;
  initializeCells(Data);
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "local_cells", Data, 2);
  optimizeLocal(*M, Registry, 1);
  expectNoMarkers(*M);
  EXPECT_EQ(countLoads(*M->getFunction("local_work")), 3u);
}

#ifdef EJIT_SRE_SHARED_TASKPOOL
TEST(EJitLocalFreeDim, NormalOnlineWorkerPGOAndLifecycleRefresh) {
  LLVMContext C;
  auto Original = makeLocalModule(C);
  ASSERT_NE(Original, nullptr);
  // Use a distinct registration identity: this test owns its loader and state.
  Original->getFunction("local_work")->setName("local_online_246");
  Original->setDataLayout(""); // ORC installs the real native target layout.
  std::string Bitcode = serialize(*Original);
  EJitModuleLoader Loader;
  ASSERT_TRUE(Loader.registerBitcode(
      "local_online_246", reinterpret_cast<const uint8_t *>(Bitcode.data()),
      Bitcode.size()));
  uint32_t Index =
      EJitFuncRegistry::instance().resolveAssign("local_online_246");
  uint32_t Dim = EJitLifecycleRegistry::instance().resolveAssign("cell");
  Cells Data;
  initializeCells(Data);
  EJitRuntimeState Runtime;
  Runtime.getRegistry().registerArray("cell", "local_cells", Data, 2);
  Runtime.activate("cell", 1);
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  Config Cfg;
  Cfg.enablePgo = true;
  Cfg.compileMode = CompileMode::Async;
  EJitCompileDriver Driver(Cfg, Runtime, Loader);
  auto Created = EJitOrcEngine::Create(Cfg, Runtime.getRegistry(), Runtime);
  ASSERT_TRUE(static_cast<bool>(Created));
  auto Engine = std::move(*Created);
  Engine->addUserSymbol("local_cells", Data);
  Driver.setJitEngine(std::move(Engine));
  auto *Pool = Driver.sharedTaskPool();
  Pool->bind(Shared.get());
  Pool->setPgoEnabled(true, 64);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  Pool->setInstanceEnabled(Dim, 1, true);
  EJitDimPair Pair{Dim, 1};
  using Fn = int32_t (*)(uint32_t, uint64_t);
  unsigned Executions = 0;
  auto RunGeneration = [&](int32_t ExpectedMode, uint64_t Completed) {
    auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < Deadline) {
      auto Result = Pool->compileOrGet(Index, &Pair, 1, nullptr);
      if (Result.fnPtr) {
        unsigned Slot = Executions++ % 10;
        int32_t Actual = reinterpret_cast<Fn>(Result.fnPtr)(1, Slot);
        // Release the real reader token before any assertion or mutation.
        if (Result.hasReadToken)
          Pool->releaseRead(Result.bucketIndex);
        EXPECT_EQ(Actual,
                  ExpectedMode * 101 + 100 + static_cast<int32_t>(Slot));
      }
      if (Shared->pgoCompletedFunctions.loadAcquire() >= Completed)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  EXPECT_TRUE(RunGeneration(11, 1)) << "normal queue never published Tier-2";
  EXPECT_GE(Executions, 64u);
  EXPECT_EQ(Shared->counters.asyncCompiles.loadAcquire(), 2u)
      << "offset introduced an unexpected specialization axis";
  Pool->setInstanceEnabled(Dim, 1, false);
  Runtime.deactivate("cell", 1);
  for (Slot &S : Data[1])
    S.mode = 19;
  Runtime.activate("cell", 1);
  Pool->setInstanceEnabled(Dim, 1, true);
  EXPECT_TRUE(RunGeneration(19, 2)) << "new generation never refreshed Tier-2";
  EXPECT_EQ(Shared->counters.asyncCompiles.loadAcquire(), 4u);
  Pool->ownerShutdown();
}
#endif

#ifdef EJIT_SRE_SHARED_TASKPOOL
TEST(EJitLocalFreeDim, UnsupportedBoundaryVersionFailsCleanlyToFallback) {
  LLVMContext C;
  auto M = makeLocalModule(C);
  ASSERT_NE(M, nullptr);
  M->getFunction("local_work")->setName("local_bad_boundary_246");
  M->getFunction("__ejit_local_free_dim_v1_i64")
      ->addFnAttr("ejit.local_free_dim", "v2");
  M->setDataLayout("");
  std::string Bitcode = serialize(*M);
  EJitLifecycleRegistry::instance().resolveAssign("cell");
  uint32_t Dim = EJitLifecycleRegistry::instance().lookup("cell");
  EJitModuleLoader Loader;
  ASSERT_TRUE(Loader.registerBitcode(
      "local_bad_boundary_246",
      reinterpret_cast<const uint8_t *>(Bitcode.data()), Bitcode.size()));
  uint32_t Index =
      EJitFuncRegistry::instance().resolveAssign("local_bad_boundary_246");
  Cells Data;
  initializeCells(Data);
  EJitRuntimeState Runtime;
  Runtime.getRegistry().registerArray("cell", "local_cells", Data, 2);
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  Config Cfg;
  Cfg.compileMode = CompileMode::Sync;
  EJitCompileDriver Driver(Cfg, Runtime, Loader);
  Driver.registerSymbol("local_cells", Data);
  auto *Pool = Driver.sharedTaskPool();
  Pool->bind(Shared.get());
  ASSERT_TRUE(Driver.startSharedTaskPool());
  Pool->setInstanceEnabled(Dim, 1, true);
  EJitDimPair Pair{Dim, 1};
  auto Result = Pool->compileOrGet(Index, &Pair, 1, nullptr);
  EXPECT_EQ(Result.fnPtr, nullptr);
  EXPECT_GT(Shared->counters.compileFailed.loadAcquire(), 0u);
  Pool->ownerShutdown();
}
#endif

TEST(EJitLocalFreeDim, RealOrcExecutionCountersPGOAndGenerationRefresh) {
  LLVMContext C;
  auto Original = makeLocalModule(
      C, "%seed = call i64 @local_initializer(i64 %raw)", "%seed");
  ASSERT_NE(Original, nullptr);
  Original->setDataLayout(""); // ORC installs the real native target layout.
  std::string Bitcode = serialize(*Original);
  Cells Data;
  initializeCells(Data);
  EJitRuntimeState Runtime;
  Runtime.getRegistry().registerArray("cell", "local_cells", Data, 2);
  Runtime.activate("cell", 1);
  Config Cfg;
  auto Created = EJitOrcEngine::Create(Cfg, Runtime.getRegistry(), Runtime);
  ASSERT_TRUE(static_cast<bool>(Created));
  auto Engine = std::move(*Created);
  Engine->addUserSymbol("local_cells", Data);
  Engine->addUserSymbol("local_initializer",
                        reinterpret_cast<void *>(&localInitializer));
  InitializerCalls = 0;
  SpecializationContext Gen;
  Gen.fnName = "local_work";
  Gen.cacheKey = 246;
  Gen.dimensions.push_back({"cell", 1});
  Gen.tier = CompileTier::Instrumented;
  Engine->setActiveContext(&Gen);
  EXPECT_FALSE(
      errorToBool(Engine->loadBitcodeModule(Bitcode, 246, "local_work")));
  auto Tier1 = Engine->lookup(246, "local_work");
  ASSERT_TRUE(static_cast<bool>(Tier1));
  EXPECT_EQ(InitializerCalls, 0u) << "compile worker executed the initializer";
  using Fn = int32_t (*)(uint32_t, uint64_t);
  auto T1 = reinterpret_cast<Fn>(*Tier1);
  for (unsigned I = 0; I != 100; ++I)
    EXPECT_EQ(T1(1, I % 10), 1211 + static_cast<int32_t>(I % 10));
  EXPECT_EQ(InitializerCalls, 100u);
  std::vector<std::string> Names(Engine->getLastCounterNames().begin(),
                                 Engine->getLastCounterNames().end());
  ASSERT_FALSE(Names.empty());
  std::vector<PgoCounterRef> Refs;
  for (const std::string &Name : Names) {
    auto Counts = Engine->lookup(246, "__profc_" + Name);
    auto Profile = Engine->lookup(246, "__profd_" + Name);
    ASSERT_TRUE(static_cast<bool>(Counts));
    ASSERT_TRUE(static_cast<bool>(Profile));
    Refs.push_back({Name.c_str(), reinterpret_cast<uintptr_t>(*Counts),
                    reinterpret_cast<uintptr_t>(*Profile)});
  }
  std::string Profile = synthesizeProfileBuffer(Refs);
  ASSERT_FALSE(
      Profile.empty()); // counters come from EXECUTED T1, not fabrication
  SpecializationContext Use = Gen;
  Use.cacheKey = 247;
  Use.tier = CompileTier::PGOUse;
  Use.profileData = Profile;
  Engine->setActiveContext(&Use);
  EXPECT_FALSE(
      errorToBool(Engine->loadBitcodeModule(Bitcode, 247, "local_work")));
  auto Tier2 = Engine->lookup(247, "local_work");
  ASSERT_TRUE(static_cast<bool>(Tier2));
  EXPECT_EQ(InitializerCalls, 100u);
  auto T2 = reinterpret_cast<Fn>(*Tier2);
  for (unsigned I = 0; I != 10; ++I)
    EXPECT_EQ(T2(1, I), 1211 + static_cast<int32_t>(I));
  EXPECT_EQ(InitializerCalls, 110u);

  Runtime.deactivate("cell", 1);
  for (Slot &S : Data[1])
    S.mode = 19;
  Runtime.activate("cell", 1);
  SpecializationContext Fresh = Gen;
  Fresh.cacheKey = 248;
  Fresh.tier = CompileTier::Baseline;
  Engine->setActiveContext(&Fresh);
  EXPECT_FALSE(
      errorToBool(Engine->loadBitcodeModule(Bitcode, 248, "local_work")));
  auto Refreshed = Engine->lookup(248, "local_work");
  ASSERT_TRUE(static_cast<bool>(Refreshed));
  auto New = reinterpret_cast<Fn>(*Refreshed);
  for (unsigned I = 0; I != 10; ++I)
    EXPECT_EQ(New(1, I), 2019 + static_cast<int32_t>(I));
  Engine->setActiveContext(nullptr);
}
