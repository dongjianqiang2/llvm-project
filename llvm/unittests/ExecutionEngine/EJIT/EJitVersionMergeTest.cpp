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

TEST(EJitVersionMerge, ColdIndependentPgoToSharedFinalAndLifecycle) {
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
    auto M = parse(C, "@data = internal unnamed_addr constant i32 " +
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
                        "@g = internal constant i32 0",
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
