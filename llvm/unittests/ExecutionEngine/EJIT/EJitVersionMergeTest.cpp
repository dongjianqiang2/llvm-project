//===-- EJitVersionMergeTest.cpp - Final-version switch assembly ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_SWITCH_CASE

#include "llvm/ExecutionEngine/EJIT/EJitVersionMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitCompileDriver.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

#if defined(EJIT_SRE_SHARED_TASKPOOL) && defined(EJIT_SRE_TASKPOOL_NO_RECLAIM) && \
    defined(EJIT_SRE_CODE_POOL)
namespace {
uint32_t runtimeMergeAot(uint8_t Cell, uint8_t Trp, uint32_t X) {
  return Cell * 100u + Trp * 10u + X;
}
struct FinalSnapshot {
  EJitCompileRequest request{};
  void *pointer = nullptr;
  uint32_t cell = 0;
  uint32_t trp = 0;
  uint32_t tier = 0;
};
void snapshotFinal(const EJitSharedCacheSlot &S, void *Opaque) {
  auto &Out = *static_cast<FinalSnapshot *>(Opaque);
  if (S.numDims != 2 || S.dims[0].instanceId != Out.cell ||
      S.dims[1].instanceId != Out.trp)
    return;
  Out.pointer = reinterpret_cast<void *>(S.fnPtr.loadAcquire());
  Out.tier = S.tier.loadRelaxed();
  Out.request.funcIndex = encodeReqTier(S.funcIndex, Out.tier);
  Out.request.numDims = S.numDims;
  Out.request.generation = S.generation;
  for (unsigned I = 0; I < 2; ++I) {
    Out.request.dims[I] = S.dims[I];
    Out.request.versions[I] = S.versions[I];
  }
}
} // namespace

TEST(EJitVersionMerge, ColdIndependentPgoToSixBySixFinalAndLifecycle) {
  EJitFuncRegistry::instance().reset();
  EJitLifecycleRegistry::instance().reset();
  const uint32_t CellType = EJitLifecycleRegistry::instance().resolveAssign("cell");
  const uint32_t TrpType = EJitLifecycleRegistry::instance().resolveAssign("trp");
  LLVMContext C;
  SMDiagnostic D;
  auto M = parseAssemblyString(R"(
    define i32 @merge_runtime(i8 %cell, i8 %trp, i32 %x) !ejit.metadata !0 {
      %c = zext i8 %cell to i32
      %t = zext i8 %trp to i32
      %a = mul i32 %c, 100
      %b = mul i32 %t, 10
      %s = add i32 %a, %b
      %r = add i32 %s, %x
      ret i32 %r
    }
    !0 = !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
  )", D, C);
  ASSERT_TRUE(M);
  std::string Bitcode;
  raw_string_ostream OS(Bitcode);
  WriteBitcodeToFile(*M, OS);
  EJitModuleLoader Loader;
  ASSERT_TRUE(Loader.registerBitcode("merge_runtime",
      reinterpret_cast<const uint8_t *>(Bitcode.data()), Bitcode.size()));
  const uint32_t Func = EJitFuncRegistry::instance().resolveAssign("merge_runtime");
  Config Cfg;
  Cfg.enablePgo = true;
  Cfg.enableFinalVersionMerge = true;
  EJitRuntimeState State;
  EJitCompileDriver Driver(Cfg, State, Loader, nullptr);
  Driver.registerSymbol(std::string(FINAL_MERGE_AOT_PREFIX) + "merge_runtime",
                        reinterpret_cast<void *>(&runtimeMergeAot));
  auto *Pool = Driver.sharedTaskPool();
  // Run the real worker state machine deterministically, without a second
  // host thread. No compiler, publication or PGO callback is replaced.
  Pool->setWorkerHooks(nullptr, nullptr, nullptr);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  Pool->setPgoEnabled(true, 64);
  for (uint32_t V : {0u, 3u, 7u})
    Pool->setInstanceEnabled(CellType, V, true);
  for (uint32_t V : {2u, 5u, 9u})
    Pool->setInstanceEnabled(TrpType, V, true);
  using Fn = uint32_t (*)(uint8_t, uint8_t, uint32_t);
  auto Snapshot = [&](uint32_t Cell, uint32_t Trp) {
    FinalSnapshot S;
    S.cell = Cell;
    S.trp = Trp;
    Pool->forEachCompiled(&snapshotFinal, &S);
    if (S.pointer && S.tier == kEJitTierPgoUse &&
        !Pool->finalVersionCurrent(S.request, S.pointer))
      S.pointer = nullptr;
    return S;
  };
  auto Call = [&](uint32_t Cell, uint32_t Trp) {
    EJitDimPair Dims[] = {{CellType, Cell}, {TrpType, Trp}};
    // Generated wrappers use the C API, whose queue request has no fallback.
    auto R = Pool->compileOrGet(Func, Dims, 2, nullptr);
    auto F = reinterpret_cast<Fn>(R.fnPtr ? R.fnPtr :
                                  reinterpret_cast<void *>(&runtimeMergeAot));
    EXPECT_EQ(F(Cell, Trp, 7), runtimeMergeAot(Cell, Trp, 7));
    if (R.hasReadToken)
      Pool->releaseRead(R.bucketIndex);
  };
  auto Train = [&](uint32_t Cell, uint32_t Trp) {
    unsigned T1Calls = 0;
    Call(Cell, Trp);
    for (unsigned I = 0; I < 400; ++I) {
      auto S = Snapshot(Cell, Trp);
      if (S.pointer && S.tier == kEJitTierPgoUse)
        break;
      T1Calls += S.pointer && S.tier == kEJitTierInstrumented;
      Call(Cell, Trp);
      Pool->workerPollOnce();
      EXPECT_TRUE(Pool->flushCodeBatch());
    }
    EXPECT_GE(T1Calls, 64u);
    EXPECT_EQ(Snapshot(Cell, Trp).tier, kEJitTierPgoUse);
    Pool->workerPollOnce();
  };
  Train(0, 2);
  auto First = Snapshot(0, 2);
  ASSERT_NE(First.pointer, nullptr);
  Train(3, 5);
  auto A = Snapshot(0, 2), B = Snapshot(3, 5);
  ASSERT_EQ(A.pointer, B.pointer);
  ASSERT_NE(A.pointer, First.pointer);
  EXPECT_EQ(reinterpret_cast<Fn>(A.pointer)(0, 2, 8), 28u);
  EXPECT_EQ(reinterpret_cast<Fn>(A.pointer)(3, 5, 8), 358u);
  EXPECT_EQ(reinterpret_cast<Fn>(A.pointer)(7, 9, 8), 798u); // unvisited AOT
  EXPECT_FALSE(Snapshot(7, 9).pointer);
  Train(7, 9); // late member rebuild, not a second independent permanent entry
  A = Snapshot(0, 2);
  EXPECT_EQ(A.pointer, Snapshot(7, 9).pointer);
  EXPECT_EQ(A.pointer, Snapshot(3, 5).pointer);
  EJitCompiledCodeInfo Info;
  ASSERT_TRUE(Driver.getJitEngine()->findCodeRange(A.pointer, Info));
  auto BadVersion = A.request;
  ++BadVersion.versions[0];
  EXPECT_FALSE(Pool->replaceFinalVersion(BadVersion, A.pointer, First.pointer, Info));
  auto BadGeneration = A.request;
  ++BadGeneration.generation;
  EXPECT_FALSE(Pool->replaceFinalVersion(BadGeneration, A.pointer, First.pointer, Info));
  EJitCompiledCodeInfo Unsealed;
  EXPECT_FALSE(Pool->replaceFinalVersion(A.request, A.pointer, First.pointer, Unsealed));
  EXPECT_EQ(Snapshot(0, 2).pointer, A.pointer);
  EXPECT_FALSE(Pool->replaceFinalVersion(A.request, First.pointer, A.pointer, Info));
  Pool->setInstanceEnabled(CellType, 0, false);
  EXPECT_FALSE(Pool->finalVersionCurrent(A.request, A.pointer));
  EXPECT_FALSE(Pool->replaceFinalVersion(A.request, A.pointer, First.pointer, Info));
  Call(0, 2); // disabled remains AOT
  EXPECT_EQ(reinterpret_cast<Fn>(Snapshot(3, 5).pointer)(3, 5, 8), 358u);
  Pool->setInstanceEnabled(CellType, 0, true);
  Train(0, 2);
  EXPECT_EQ(Snapshot(0, 2).pointer, Snapshot(3, 5).pointer);
  // The product has six values per dimension, not six total tuples. Exercise
  // all 36 through real independent T1 execution and the worker merge path.
  const uint32_t Cells[] = {0, 3, 7, 9, 11, 13};
  const uint32_t Trps[] = {2, 5, 9, 10, 12, 15};
  for (uint32_t Cell : Cells)
    Pool->setInstanceEnabled(CellType, Cell, true);
  for (uint32_t Trp : Trps)
    Pool->setInstanceEnabled(TrpType, Trp, true);
  for (uint32_t Cell : Cells)
    for (uint32_t Trp : Trps)
      if (!Snapshot(Cell, Trp).pointer)
        Train(Cell, Trp);
  void *Grid = Snapshot(13, 15).pointer;
  ASSERT_NE(Grid, nullptr);
  for (uint32_t Cell : Cells)
    for (uint32_t Trp : Trps) {
      SCOPED_TRACE(std::to_string(Cell) + "," + std::to_string(Trp));
      ASSERT_EQ(Snapshot(Cell, Trp).pointer, Grid);
      EXPECT_EQ(reinterpret_cast<Fn>(Grid)(Cell, Trp, 19),
                runtimeMergeAot(Cell, Trp, 19));
    }
  EXPECT_EQ(reinterpret_cast<Fn>(Grid)(15, 0, 19), runtimeMergeAot(15, 0, 19));
  RecordProperty("shared_grid_members", 36);
  Pool->ownerShutdown();
  EJitFuncRegistry::instance().reset();
  EJitLifecycleRegistry::instance().reset();
}
#endif

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
  return {{M.getFunction("arm_a"), {0, 2}},
          {M.getFunction("arm_b"), {3, 5}}};
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
  auto Result = assembleFinalVersionSwitch("merged", *M->getFunction("merge_aot"),
                                           {0, 1}, Arms);
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
  auto Result = assembleFinalVersionSwitch("merged", *M->getFunction("merge_aot"),
                                           {0, 1}, Arms);
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
        const uint32_t Config = Cell == 0 && Trp == 2 ? 7u
                               : Cell == 3 && Trp == 5 ? 11u : 1000u;
        const uint32_t Expected = Cell * 100u + Trp * 10u + X + Config;
        uint32_t Out = 0;
        EXPECT_EQ(Fn(Cell, Trp, X, &Out), Expected);
        EXPECT_EQ(Out, Expected);
      }
}

TEST(EJitVersionMerge, ReplayUsesFrozenFieldWithoutOriginalBoundObject) {
  LLVMContext LC;
  auto M = parse(LC, R"(
    define i32 @replay_bound(i32 %cell, ptr %cfg) !ejit.metadata !0 {
      %field = getelementptr i8, ptr %cfg, i64 4
      %v = load i32, ptr %field, !ejit.may_const !5
      %live = getelementptr i8, ptr %cfg, i64 8
      %d = load i32, ptr %live
      %r = add i32 %v, %d
      ret i32 %r
    }
    !0 = distinct !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 12, !4}
    !4 = !{i64 0, i64 8}
    !5 = !{}
  )");
  std::string Source, Original;
  raw_string_ostream OS(Source);
  WriteBitcodeToFile(*M, OS);
  EJitRuntimeState State;
  auto Engine = EJitOrcEngine::Create(Config{}, State.getRegistry(), State);
  ASSERT_TRUE(bool(Engine)) << toString(Engine.takeError());
  EJitFinalReplay Recipe;
  SpecializationContext C;
  C.fnName = "replay_bound";
  C.cacheKey = 371;
  C.tier = CompileTier::PGOUse;
  C.dimensions.push_back({"cell", 3});
  SmallVector<unsigned, 2> Args;
  C.finalMergeBitcode = &Original;
  C.finalMergeDimensionArgs = &Args;
  C.captureFolds = &Recipe;
  {
    uint32_t Borrowed[] = {3, 17, 99};
    C.boundPointers.push_back(
        {reinterpret_cast<const uint8_t *>(Borrowed), sizeof(Borrowed), 1});
    (*Engine)->setActiveContext(&C);
    auto Err = (*Engine)->loadBitcodeModule(Source, C.cacheKey, C.fnName);
    ASSERT_FALSE(bool(Err)) << toString(std::move(Err));
    auto Address = (*Engine)->lookup(C.cacheKey, C.fnName);
    ASSERT_TRUE(bool(Address)) << toString(Address.takeError());
    (*Engine)->setActiveContext(nullptr);
  }
  ASSERT_FALSE(Original.empty());
  ASSERT_TRUE(Recipe.usable);
  ASSERT_FALSE(Recipe.folds.empty());
  Recipe.bounds.push_back({1, 12});
  Recipe.sourceHash = EJitFinalReplay::digest(Source);
  Recipe.outputHash = EJitFinalReplay::digest(Original);
  SpecializationContext Fresh;
  Fresh.cacheKey = C.cacheKey;
  Fresh.fnName = C.fnName;
  Fresh.dimensions = C.dimensions;
  Fresh.tier = CompileTier::PGOUse;
  auto Rebuilt = (*Engine)->rebuildFinalMember(Source, Fresh, Recipe);
  ASSERT_TRUE(bool(Rebuilt)) << toString(Rebuilt.takeError());
  EXPECT_EQ(*Rebuilt, Original);
  auto WrongSource =
      (*Engine)->rebuildFinalMember("not that module", Fresh, Recipe);
  ASSERT_FALSE(bool(WrongSource));
  EXPECT_NE(toString(WrongSource.takeError()).find("source mismatch"),
            std::string::npos);
  Recipe.folds.front().bits ^= 1;
  auto WrongValue = (*Engine)->rebuildFinalMember(Source, Fresh, Recipe);
  ASSERT_FALSE(bool(WrongValue));
  EXPECT_NE(toString(WrongValue.takeError()).find("digest mismatch"),
            std::string::npos);
}

TEST(EJitVersionMerge, SixtyFourModuleArmsExecuteAndSixtyFiveReject) {
  LLVMContext C;
  std::vector<std::string> Storage;
  SmallVector<EJitFinalModuleArm, 8> Arms;
  Storage.reserve(64);
  for (unsigned Cell = 0; Cell < 8; ++Cell)
    for (unsigned Trp = 0; Trp < 8; ++Trp) {
      auto M = std::make_unique<Module>("grid", C);
      auto *I8 = Type::getInt8Ty(C);
      auto *I32 = Type::getInt32Ty(C);
      auto *F = Function::Create(FunctionType::get(I32, {I8, I8, I32}, false),
                                 Function::ExternalLinkage, "grid_entry", *M);
      IRBuilder<> B(BasicBlock::Create(C, "entry", F));
      B.CreateRet(B.CreateAdd(F->getArg(2),
                              B.getInt32(1000 + Cell * 100 + Trp * 10)));
      Storage.emplace_back();
      raw_string_ostream OS(Storage.back());
      WriteBitcodeToFile(*M, OS);
      Arms.push_back({Storage.back(), {Cell, Trp}});
    }
  auto Aot = +[](uint8_t Cell, uint8_t Trp, uint32_t X) -> uint32_t {
    return Cell * 100u + Trp * 10u + X;
  };
  auto Prepared = assembleFinalVersionModules(
      "grid_entry", reinterpret_cast<uintptr_t>(Aot), {0, 1}, Arms);
  ASSERT_TRUE(bool(Prepared)) << toString(Prepared.takeError());
  auto Parsed = parseBitcodeFile(MemoryBufferRef(*Prepared, "grid"), C);
  ASSERT_TRUE(bool(Parsed)) << toString(Parsed.takeError());
  unsigned Definitions = 0;
  for (auto &F : **Parsed)
    Definitions += !F.isDeclaration();
  EXPECT_EQ(Definitions, 1u);
  EJitRuntimeState State;
  Config Cfg;
  auto Engine = EJitOrcEngine::Create(Cfg, State.getRegistry(), State);
  ASSERT_TRUE(bool(Engine)) << toString(Engine.takeError());
  auto E = (*Engine)->loadBitcodeModule(*Prepared, 778, "grid_entry");
  ASSERT_FALSE(bool(E)) << toString(std::move(E));
  auto Address = (*Engine)->lookup(778, "grid_entry");
  ASSERT_TRUE(bool(Address)) << toString(Address.takeError());
#ifdef EJIT_SRE_CODE_POOL
  E = (*Engine)->flushPendingCode();
  ASSERT_FALSE(bool(E)) << toString(std::move(E));
#endif
  auto Fn = reinterpret_cast<uint32_t (*)(uint8_t, uint8_t, uint32_t)>(*Address);
  for (unsigned Cell = 0; Cell < 8; ++Cell)
    for (unsigned Trp = 0; Trp < 8; ++Trp)
      EXPECT_EQ(Fn(Cell, Trp, 23), 1000 + Aot(Cell, Trp, 23));
  EXPECT_EQ(Fn(8, 0, 23), Aot(8, 0, 23));
  EXPECT_EQ(Fn(0, 8, 23), Aot(0, 8, 23));
  Arms.push_back({Storage.front(), {8, 0}});
  auto TooMany = assembleFinalVersionModules(
      "grid_entry", reinterpret_cast<uintptr_t>(Aot), {0, 1}, Arms);
  ASSERT_FALSE(bool(TooMany));
  EXPECT_NE(toString(TooMany.takeError()).find("cohort"), std::string::npos);
}

TEST(EJitVersionMerge, SixBySixExceedsFormerByteAndInstructionBudgets) {
  LLVMContext C;
  std::vector<std::string> Storage;
  SmallVector<EJitFinalModuleArm, 8> Arms;
  Storage.reserve(36);
  uint64_t Bytes = 0;
  for (unsigned I = 0; I < 36; ++I) {
    auto M = std::make_unique<Module>("grid_budget", C);
    auto *I32 = Type::getInt32Ty(C);
    auto *Ptr = PointerType::getUnqual(C);
    std::vector<uint8_t> Data(128 * 1024);
    for (unsigned J = 0; J < Data.size(); ++J)
      Data[J] = static_cast<uint8_t>(J * 37u + (J >> 8) + I * 13u);
    auto *Init = ConstantDataArray::get(C, Data);
    auto *Table = new GlobalVariable(*M, Init->getType(), true,
                                     GlobalValue::PrivateLinkage, Init, "table");
    Table->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    auto Consume = M->getOrInsertFunction("consume", Type::getVoidTy(C), Ptr);
    auto *F = Function::Create(FunctionType::get(I32, {I32, I32}, false),
                               Function::ExternalLinkage, "entry", *M);
    IRBuilder<> B(BasicBlock::Create(C, "entry", F));
    for (unsigned J = 0; J < 250; ++J)
      B.CreateCall(Consume, {Table});
    B.CreateRet(B.getInt32(I));
    Storage.emplace_back();
    raw_string_ostream OS(Storage.back());
    WriteBitcodeToFile(*M, OS);
    ASSERT_LE(Storage.back().size(), kEJitFinalMergeMaxMemberBytes);
    Bytes += Storage.back().size();
    Arms.push_back({Storage.back(), {I / 6, I % 6}});
  }
  ASSERT_GT(Bytes, 4u * 1024 * 1024);
  auto Prepared = assembleFinalVersionModules("entry", 4096, {0, 1}, Arms);
  ASSERT_TRUE(bool(Prepared)) << toString(Prepared.takeError());
  auto Parsed = parseBitcodeFile(MemoryBufferRef(*Prepared, "grid_budget"), C);
  ASSERT_TRUE(bool(Parsed)) << toString(Parsed.takeError());
  auto *Entry = (*Parsed)->getFunction("entry");
  ASSERT_TRUE(Entry);
  EXPECT_GT(Entry->getInstructionCount(), 8000u);
  EXPECT_LE(Entry->getInstructionCount(), kEJitFinalMergeMaxInstructions);
  std::string Oversize(kEJitFinalMergeMaxMemberBytes + 1, 'x');
  Arms[0].bitcode = Oversize;
  auto Rejected = assembleFinalVersionModules("entry", 4096, {0, 1}, Arms);
  ASSERT_FALSE(bool(Rejected));
  EXPECT_NE(toString(Rejected.takeError()).find("artifact budget"),
            std::string::npos);
}

TEST(EJitVersionMerge, SingleDimensionKeepsOtherArgumentsLive) {
  LLVMContext Ctx;
  auto M = parse(Ctx);
  auto Arms = arms(*M);
  Arms[0].dimensions = {2};
  Arms[1].dimensions = {5};
  auto Result = assembleFinalVersionSwitch("merged", *M->getFunction("merge_aot"),
                                           {1}, Arms);
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
    if (Mode == 0) Arms[1].dimensions = Arms[0].dimensions;
    if (Mode == 1) Limits.maxArms = 1;
    if (Mode == 2) Limits.maxInstructions = 1;
    if (Mode == 3) Dims = {0, 0};
    auto Result = assembleFinalVersionSwitch("merged", *M->getFunction("merge_aot"),
                                             Dims, Arms, Limits);
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
  auto Result = assembleFinalVersionSwitch("merged", *M->getFunction("aot"),
                                           {0}, Arms);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(toString(Result.takeError()).find("does-not-fit"), std::string::npos);
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
  auto Result = assembleFinalVersionSwitch("merged", *M->getFunction("aot"),
                                           {0}, Arms);
  ASSERT_TRUE(bool(Result)) << toString(Result.takeError());
  EXPECT_FALSE((*Result)->doesNotAccessMemory());
  EXPECT_FALSE((*Result)->getAttributes().getRetAttrs().hasAttribute(Attribute::NoUndef));
  EXPECT_TRUE((*Result)->getAttributes().getRetAttrs().hasAttribute(Attribute::SExt));
  EXPECT_TRUE((*Result)->getAttributes().getParamAttrs(0).hasAttribute(Attribute::ZExt));
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
  auto Result = assembleFinalVersionSwitch("merged", *M->getFunction("aot"),
                                           {0}, Arms);
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
  auto Result = assembleFinalVersionSwitch("merged", *M->getFunction("aot"),
                                           {0}, Arms);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(toString(Result.takeError()).find("recursion"), std::string::npos);
}

TEST(EJitVersionMerge, ModuleImportKeepsPrivateHelpersAndConstantsSeparate) {
  auto Make = [](unsigned Value) {
    LLVMContext C;
    auto M = parse(C, "@data = internal constant i32 " +
        std::to_string(Value) + R"(
      define internal i32 @helper() {
        %v = load i32, ptr @data
        ret i32 %v
      }
      define i32 @entry(i32 %cell) {
        %r = call i32 @helper()
        ret i32 %r
      })");
    std::string BC;
    raw_string_ostream OS(BC);
    WriteBitcodeToFile(*M, OS);
    return BC;
  };
  std::string A = Make(7), B = Make(11);
  SmallVector<EJitFinalModuleArm, 2> Arms{{A, {0}}, {B, {3}}};
  auto Result = assembleFinalVersionModules("entry", 4096, {0}, Arms);
  ASSERT_TRUE(bool(Result)) << toString(Result.takeError());
  LLVMContext C;
  auto M = parseBitcodeFile(MemoryBufferRef(*Result, "merged"), C);
  ASSERT_TRUE(bool(M)) << toString(M.takeError());
  EXPECT_TRUE((*M)->getFunction("entry"));
  EXPECT_TRUE((*M)->getFunction("__ejit_merge_0_helper"));
  EXPECT_TRUE((*M)->getFunction("__ejit_merge_1_helper"));
  EXPECT_EQ(cast<ConstantInt>((*M)->getNamedGlobal("__ejit_merge_0_data")
                                ->getInitializer())->getZExtValue(), 7u);
  EXPECT_EQ(cast<ConstantInt>((*M)->getNamedGlobal("__ejit_merge_1_data")
                                ->getInitializer())->getZExtValue(), 11u);
  EXPECT_FALSE(verifyModule(**M, &errs()));
}

TEST(EJitVersionMerge, ModuleImportPreservesScalarAbiOnAotEdge) {
  LLVMContext C;
  SMDiagnostic D;
  auto M = parseAssemblyString(R"(
    define signext i8 @entry(i8 zeroext %cell, i8 signext %x) {
      ret i8 %x
    }
    !llvm.module.flags = !{!0}
    !0 = !{i32 1, !"wchar_size", i32 4}
  )", D, C);
  ASSERT_TRUE(M);
  std::string BC;
  raw_string_ostream OS(BC);
  WriteBitcodeToFile(*M, OS);
  EJitFinalModuleArm Arms[] = {{BC, {0}}, {BC, {3}}};
  auto Result = assembleFinalVersionModules("entry", 4096, {0}, Arms);
  ASSERT_TRUE(static_cast<bool>(Result)) << toString(Result.takeError());
  auto Parsed = parseBitcodeFile(MemoryBufferRef(*Result, "merged"), C);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  auto *Entry = (*Parsed)->getFunction("entry");
  ASSERT_TRUE(Entry);
  EXPECT_TRUE((*Parsed)->getModuleFlag("wchar_size"));
  EXPECT_TRUE(Entry->hasRetAttribute(Attribute::SExt));
  EXPECT_TRUE(Entry->hasParamAttribute(0, Attribute::ZExt));
  bool Found = false;
  for (auto &I : instructions(Entry))
    if (auto *Call = dyn_cast<CallInst>(&I)) {
      Found = true;
      EXPECT_TRUE(Call->hasRetAttr(Attribute::SExt));
      EXPECT_TRUE(Call->paramHasAttr(0, Attribute::ZExt));
      EXPECT_TRUE(Call->paramHasAttr(1, Attribute::SExt));
    }
  EXPECT_TRUE(Found);
}

TEST(EJitVersionMerge, MutableAndAddressObservablePrivateStateStayIndependent) {
  for (StringRef Decl : {"@g = internal global i32 0",
                        "@g = external thread_local global i32"}) {
    LLVMContext C;
    auto M = parse(C, (Decl + R"(
      define i32 @entry(i32 %cell) {
        %v = load i32, ptr @g
        ret i32 %v
      })").str());
    Error E = validateFinalVersionModule(*M, "entry");
    EXPECT_TRUE(bool(E));
    consumeError(std::move(E));
  }
}

TEST(EJitVersionMerge, FunctionLocalLookupDoesNotRequireUnnamedAddr) {
  for (StringRef Use : {"%r = load i32, ptr %p",
                        "%r = load volatile i32, ptr %p",
                        "%r = ptrtoint ptr %p to i32",
                        "store ptr %p, ptr %out\n %r = add i32 %idx, 1"}) {
    LLVMContext C;
    auto M = parse(C, (R"(
      @Foo.xxx = private constant [3 x i32] [i32 7, i32 3, i32 9]
      define i32 @entry(i32 %idx, ptr %out) {
        %p = getelementptr i32, ptr getelementptr (i8, ptr @Foo.xxx, i64 4), i32 %idx
      )" + Use + "\n ret i32 %r\n}").str());
    auto E = validateFinalVersionModule(*M, "entry");
    if (Use == "%r = load i32, ptr %p")
      EXPECT_FALSE(bool(E)) << toString(std::move(E));
    else {
      ASSERT_TRUE(bool(E)) << Use.str();
      EXPECT_NE(toString(std::move(E)).find("Foo.xxx"), std::string::npos);
    }
  }
}

TEST(EJitVersionMerge, ImportRejectsOversizedOrMissingCohortBeforeEmission) {
  SmallVector<EJitFinalModuleArm, 2> Empty;
  auto Result = assembleFinalVersionModules("entry", 0, {0}, Empty);
  EXPECT_FALSE(bool(Result));
  consumeError(Result.takeError());
  LLVMContext C;
  auto M = parse(C, R"(
    @large = internal unnamed_addr constant [524289 x i8] zeroinitializer
    define i32 @entry(i32 %cell) { ret i32 7 }
  )");
  Error E = validateFinalVersionModule(*M, "entry");
  ASSERT_TRUE(bool(E));
  EXPECT_NE(toString(std::move(E)).find("budget"), std::string::npos);
}

#endif

#if defined(EJIT_SWITCH_CASE) && defined(EJIT_SRE_SHARED_TASKPOOL) &&          \
    defined(EJIT_SRE_TASKPOOL_NO_RECLAIM) && defined(EJIT_SRE_CODE_POOL)
namespace {
void exerciseCapacity(Config Cfg, unsigned Count, unsigned ExpectedMerges,
                      bool ExplicitFallback = false, bool Overflow = false) {
  EJitFuncRegistry::instance().reset();
  EJitLifecycleRegistry::instance().reset();
  LLVMContext C;
  SMDiagnostic D;
  auto M = parseAssemblyString(R"(
    define i32 @entry(i8 %cell, i8 %trp, i32 %x) !ejit.metadata !0 {
      %c = zext i8 %cell to i32
      %t = zext i8 %trp to i32
      %a = mul i32 %c, 100
      %b = mul i32 %t, 10
      %s = add i32 %a, %b
      %r = add i32 %s, %x
      ret i32 %r
    }
    !0 = !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
  )",
                               D, C);
  ASSERT_TRUE(M);
  auto *Entry = M->getFunction("entry");
  std::vector<std::string> Bitcode(Count);
  std::vector<uint32_t> Indices;
  EJitModuleLoader Loader;
  EJitRuntimeState State;
  Cfg.enablePgo = Cfg.enableFinalVersionMerge = true;
  EJitCompileDriver Driver(Cfg, State, Loader);
  for (unsigned I = 0; I != Count; ++I) {
    std::string Name = "capacity_" + std::to_string(I);
    Entry->setName(Name);
    raw_string_ostream OS(Bitcode[I]);
    WriteBitcodeToFile(*M, OS);
    ASSERT_TRUE(Loader.registerBitcode(
        Name, reinterpret_cast<const uint8_t *>(Bitcode[I].data()),
        Bitcode[I].size()));
    Indices.push_back(EJitFuncRegistry::instance().resolveAssign(Name));
    if (!ExplicitFallback)
      Driver.registerSymbol(std::string(FINAL_MERGE_AOT_PREFIX) + Name,
                            reinterpret_cast<void *>(&runtimeMergeAot));
  }
  const auto Cell = EJitLifecycleRegistry::instance().resolveAssign("cell");
  const auto Trp = EJitLifecycleRegistry::instance().resolveAssign("trp");
  auto *Pool = Driver.sharedTaskPool();
  Pool->setWorkerHooks(nullptr, nullptr, nullptr);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  Pool->setPgoEnabled(true, 2);
  Pool->setInstanceEnabled(Cell, 0, true);
  Pool->setInstanceEnabled(Cell, 3, true);
  Pool->setInstanceEnabled(Trp, 2, true);
  if (Overflow)
    for (unsigned V = 0; V <= 8; ++V) {
      Pool->setInstanceEnabled(Cell, V, true);
      Pool->setInstanceEnabled(Trp, V, true);
    }
  using Fn = decltype(&runtimeMergeAot);
  for (unsigned I = 0; I != Count; ++I) {
    SCOPED_TRACE(I);
    auto Lookup = [&](unsigned V, unsigned W = 2) {
      EJitDimPair Ds[] = {{Cell, V}, {Trp, W}};
      auto R = Pool->compileOrGet(
          Indices[I], Ds, 2,
          ExplicitFallback ? reinterpret_cast<void *>(&runtimeMergeAot)
                           : nullptr);
      if (R.hasReadToken)
        Pool->releaseRead(R.bucketIndex);
      return reinterpret_cast<Fn>(R.fnPtr);
    };
    auto Train = [&](unsigned V, unsigned W) {
      for (unsigned Poll = 0; Poll != 12; ++Poll) {
        if (auto F = Lookup(V, W))
          ASSERT_EQ(F(V, W, 9), runtimeMergeAot(V, W, 9));
        Pool->workerPollOnce();
        ASSERT_TRUE(Pool->flushCodeBatch());
      }
    };
    if (Overflow) {
      for (unsigned V = 1; V <= 8; ++V)
        for (unsigned W = 1; W <= 8; ++W)
          Train(V, W);
      auto Full = Lookup(8, 8);
      ASSERT_NE(Full, nullptr);
      Train(0, 0); // numerically smaller than all 64 admitted cache keys
      auto Independent = Lookup(0, 0);
      ASSERT_NE(Independent, nullptr);
      ASSERT_NE(Independent, Full);
      ASSERT_EQ(Independent(0, 0, 7), 7u);
      for (unsigned V = 1; V <= 8; ++V)
        for (unsigned W = 1; W <= 8; ++W)
          ASSERT_EQ(Full, Lookup(V, W));
      ASSERT_EQ(Driver.finalMergeUsage().emissionAttempts, 63u);
      break;
    }
    Train(0, 2);
    Train(3, 2);
    auto A = Lookup(0), B = Lookup(3);
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);
    ASSERT_EQ(A == B, I < ExpectedMerges);
    ASSERT_EQ(A(0, 2, 8), 28u);
    ASSERT_EQ(B(3, 2, 8), 328u);
    if (A == B)
      ASSERT_EQ(B(9, 7, 8), 978u); // untrained tuple reaches AOT
  }
  unsigned ResidentT2 = 0;
  Pool->forEachCompiled(
      [](const EJitSharedCacheSlot &S, void *Ctx) {
        if (S.tier.loadRelaxed() == kEJitTierPgoUse)
          ++*static_cast<unsigned *>(Ctx);
      },
      &ResidentT2);
  ::testing::Test::RecordProperty("resident_t2_slots", ResidentT2);
  auto Usage = Driver.finalMergeUsage();
  ::testing::Test::RecordProperty("peak_retained_members",
                                  std::to_string(Usage.peakMembers));
  ::testing::Test::RecordProperty("peak_retained_bitcode_bytes",
                                  std::to_string(Usage.peakFullIRBytes));
  ::testing::Test::RecordProperty("peak_retained_recipe_bytes",
                                  std::to_string(Usage.peakRecipeBytes));
  ::testing::Test::RecordProperty("peak_retained_total_bytes",
                                  std::to_string(Usage.peakArtifactBytes));
  ::testing::Test::RecordProperty("rebuilt_members",
                                  std::to_string(Usage.rebuiltMembers));
  EXPECT_EQ(Usage.replayFailures, 0u);
  EXPECT_LE(Usage.peakArtifactBytes, Cfg.finalMergeMaxArtifactBytes);
  if (!Cfg.finalMergeIRCacheBytes && ExpectedMerges) {
    EXPECT_GT(Usage.rebuiltMembers, 0u);
    EXPECT_EQ(Usage.peakFullIRBytes, 0u);
  }
  ::testing::Test::RecordProperty("emission_attempts",
                                  std::to_string(Usage.emissionAttempts));
  ::testing::Test::RecordProperty("published_dispatchers",
                                  std::to_string(Usage.publishedDispatchers));
  EXPECT_EQ(Usage.publishedDispatchers, ExpectedMerges);
  if (Overflow)
    EXPECT_EQ(ResidentT2, 65u);
  else if (Count <= 2 ||
           (kEJitSharedCacheSlots >= 128 && kEJitSharedCacheBuckets >= 32))
    EXPECT_EQ(ResidentT2, Count * 2);
  Pool->ownerShutdown();
  EJitFuncRegistry::instance().reset();
  EJitLifecycleRegistry::instance().reset();
}
} // namespace

TEST(EJitFinalCapacity, ThousandDistinctEntriesPublishTwoArmDispatchers) {
  exerciseCapacity(Config{}, 1025, 1025);
}

TEST(EJitFinalCapacity, ColdRecipesRebuildAfterT1CountersAreReleased) {
  Config Cfg;
  Cfg.finalMergeIRCacheBytes = 0;
  exerciseCapacity(Cfg, 3, 3);
}

TEST(EJitFinalCapacity, ReclaimOptionalIRToAdmitSecondRecipe) {
  Config Cfg;
  // Enough for one cached body plus its recipe, not two bodies. Mandatory
  // replay inputs take precedence over opportunistically cached full IR.
  Cfg.finalMergeMaxArtifactBytes = 4000;
  Cfg.finalMergeIRCacheBytes = 4000;
  exerciseCapacity(Cfg, 1, 1);
}

TEST(EJitFinalCapacity, LateLowTupleDoesNotDisplaceActiveSixtyFour) {
  exerciseCapacity(Config{}, 1, 63, false, true);
}

TEST(EJitFinalCapacity, MemberAndByteQuotasKeepIndependentBodies) {
  Config Cfg;
  Cfg.finalMergeMaxMembers = 1;
  exerciseCapacity(Cfg, 1, 0);
  Cfg.finalMergeMaxMembers = 2;
  Cfg.finalMergeMaxArtifactBytes = 1;
  exerciseCapacity(Cfg, 1, 0);
}

TEST(EJitFinalCapacity, PendingFallbackQuotaAppliesToExplicitT1Pointer) {
  Config Cfg;
  Cfg.finalMergeMaxFallbacks = 1;
  exerciseCapacity(Cfg, 1, 1, true);
  Cfg.finalMergeMaxFallbacks = 0;
  exerciseCapacity(Cfg, 1, 0, true);
}

TEST(EJitFinalCapacity, LifetimeAttemptLimitKeepsLaterBodiesIndependent) {
  Config Cfg;
  Cfg.finalMergeMaxEmissionAttempts = 1;
  exerciseCapacity(Cfg, 2, 1);
}
#endif
