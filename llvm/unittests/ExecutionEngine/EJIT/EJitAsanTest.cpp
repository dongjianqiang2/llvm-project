//===-- EJitAsanTest.cpp - Worker-only substitution checks ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitAsan.h"
#include "EJitAsanState.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"
#include <array>
#include <cstring>
#include <set>
#include <sstream>

using namespace llvm;
using namespace llvm::ejit;

namespace {
std::unique_ptr<Module> makeProbe(LLVMContext &C, bool Local, unsigned Mod) {
  std::string IR = R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
    @cfg = external global [2 x [10 x i32]], !ejit.metadata !0
    declare i16 @__ejit_local_free_dim_v1_i16(i16) #0
    define i32 @probe(i32 %cell, i16 %slot) !ejit.metadata !3 {
  )";
  if (Local)
    IR += "%local = call i16 @__ejit_local_free_dim_v1_i16(i16 %slot)\n";
  IR += std::string("%r = urem i16 ") + (Local ? "%local, " : "%slot, ") +
        std::to_string(Mod) + "\n";
  IR += R"(
      %ci = zext i32 %cell to i64
      %ri = zext i16 %r to i64
      %p = getelementptr inbounds [2 x [10 x i32]], ptr @cfg,
                                  i64 0, i64 %ci, i64 %ri
      %v = load i32, ptr %p, align 4, !ejit.period_const !7
      ret i32 %v
    }
    attributes #0 = { nounwind willreturn memory(none) nocallback nosync "ejit.local_free_dim"="v1" }
    !0 = !{!1, !2}
    !1 = !{!"ejit_group_indexed", !"cell", i64 2}
    !2 = !{!"ejit_period_const_field", i64 0}
    !3 = distinct !{!4, !5, !6}
    !4 = !{!"ejit_entry"}
    !5 = !{!"ejit_dim", !"cell", i32 0}
    !6 = !{!"ejit_free_dim", !"", i32 1}
    !7 = !{}
  )";
  SMDiagnostic Err;
  return parseAssemblyString(IR, Err, C);
}

std::string printModule(const Module &M) {
  std::string S;
  raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return S;
}

void codeMarker() {}
} // namespace

TEST(EJitAsan, CapturesEveryResidueWithoutChangingIR) {
  for (bool Local : {false, true})
    for (unsigned Mod : {5u, 10u}) {
      SCOPED_TRACE(::testing::Message() << Local << ':' << Mod);
      uint32_t Data[2][10];
      for (unsigned I = 0; I < 10; ++I) {
        Data[0][I] = 7;
        Data[1][I] = I == Mod - 1 ? 13 : 11;
      }
      PeriodArrayRegistry Registry;
      Registry.registerArray("cell", "cfg", Data, 2);
      LLVMContext WithContext, WithoutContext;
      auto With = makeProbe(WithContext, Local, Mod);
      auto Without = makeProbe(WithoutContext, Local, Mod);
      ASSERT_TRUE(With);
      ASSERT_TRUE(Without);
      SpecializationContext Ctx;
      Ctx.fnName = "probe";
      Ctx.dimensions.push_back({"cell", 1});
      EJitOptimizer Reference(Registry);
      Reference.runPipeline(*Without, Ctx);
      EJitAsanCapture Capture;
      Ctx.asanCapture = &Capture;
      EJitOptimizer Observed(Registry);
      Observed.runPipeline(*With, Ctx);
      EXPECT_EQ(printModule(*With), printModule(*Without));
      EXPECT_EQ(Capture.uncovered, 0u);
      ASSERT_EQ(Capture.watches.size(), 1u);
      const auto &W = Capture.watches.front();
      EXPECT_EQ(W.period, "cell");
      EXPECT_EQ(W.object, "cfg");
      EXPECT_EQ(W.objectBase, reinterpret_cast<uintptr_t>(Data));
      EXPECT_EQ(W.freeArg, 1);
      EXPECT_EQ(W.expected, 11u);
      ASSERT_EQ(W.addresses.size(), Mod);
      for (unsigned R = 0; R < Mod; ++R)
        EXPECT_EQ(W.addresses[R], reinterpret_cast<uintptr_t>(&Data[1][R]));
    }
}

TEST(EJitAsan, CapturesPhiWitnessFoldUnderWitnessBinding) {
  // A period_const load whose byte-offset GEP chain passes through a merge
  // PHI only folds via the phi-witness path: every feasible PHI binding
  // must fold to the same constant, and the fold exports the binding it
  // proved. Capture must keep up — the residue-domain walk consults
  // Assumed.phiInputs instead of rejecting every such fold as uncovered.
  // The fixture mirrors the proven EJitPhiWitness shapes: a static-root
  // object, a local-free-dim slot urem, and two constant PHI edges
  // (records 0 and 1) whose members repeat, so both bindings agree.
  constexpr unsigned RecordSize = 1240, Slots = 5;
  constexpr unsigned FieldOffset = 12, FieldAlign = 4;
  const uint64_t Expected = 1234;
  alignas(64) std::array<uint8_t, 19 * RecordSize * Slots> Data{};
  for (unsigned I = 0; I < 19; ++I)
    for (unsigned S = 0; S < Slots; ++S)
      for (unsigned B = 0; B < 4; ++B)
        Data[I * RecordSize * Slots + S * RecordSize + FieldOffset + B] =
            Expected >> (8 * B);

  auto Build = [&](LLVMContext &C, std::unique_ptr<Module> &Out) {
    std::ostringstream IR;
    IR << "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64-S128\"\n"
          "%R = type { [12 x i8], i32, [5 x i8], i8, i8, [1217 x i8] }\n"
          "@root = external global [18 x [5 x %R]], !ejit.metadata !0\n"
          "declare i16 @__ejit_local_free_dim_v1_i16(i16) #0\n"
          "declare void @consume(i32, i16, ptr)\n"
          "define void @probe(i8 %cell, ptr %ctx, ptr %flags) !ejit.metadata "
          "!5 { ret void }\n"
          "attributes #0 = { nounwind willreturn memory(none) nocallback "
          "nosync \"ejit.local_free_dim\"=\"v1\" }\n"
          "!0 = !{!1";
    for (unsigned I = 0; I < 18 * Slots; ++I)
      IR << ", !" << 100 + I;
    IR << "}\n"
          "!1 = !{!\"ejit_group\", !\"static\"}\n"
          "!5 = distinct !{!6, !7}\n"
          "!6 = !{!\"ejit_entry\"}\n"
          "!7 = !{!\"ejit_dim\", !\"cell\", i32 0}\n";
    for (unsigned I = 0; I < 18 * Slots; ++I)
      IR << "!" << 100 + I << " = !{!\"ejit_period_const_field\", i64 "
         << I * RecordSize + FieldOffset << "}\n";
    SMDiagnostic Err;
    Out = parseAssemblyString(IR.str(), Err, C);
    ASSERT_TRUE(Out);
    Function *F = Out->getFunction("probe");
    F->getEntryBlock().eraseFromParent();
    IRBuilder<> B(C);
    auto Block = [&](const char *Name) {
      return BasicBlock::Create(C, Name, F);
    };
    BasicBlock *Entry = Block("entry"), *Default = Block("default"),
               *Dynamic = Block("dynamic"), *Valid = Block("valid"),
               *Merge = Block("merge"), *Exit = Block("exit");
    B.SetInsertPoint(Entry);
    Value *SlotP = B.CreateGEP(B.getInt8Ty(), F->getArg(1), B.getInt64(20));
    Value *RawSlot = B.CreateAlignedLoad(B.getInt16Ty(), SlotP, Align(4));
    Value *Slot = B.CreateCall(
        Out->getFunction("__ejit_local_free_dim_v1_i16"), {RawSlot});
    Slot = B.CreateZExt(B.CreateURem(Slot, B.getInt16(Slots)),
                        B.getInt64Ty());
    Value *Flag = B.CreateAlignedLoad(B.getInt8Ty(), F->getArg(2), Align(1));
    Value *Cond = B.CreateICmpNE(Flag, B.getInt8(0));
    B.CreateCondBr(Cond, Dynamic, Default);
    B.SetInsertPoint(Default);
    B.CreateBr(Merge);
    B.SetInsertPoint(Dynamic);
    Value *InputP = B.CreateGEP(B.getInt8Ty(), F->getArg(2), B.getInt64(17));
    Value *Raw = B.CreateAlignedLoad(B.getInt8Ty(), InputP, Align(1));
    B.CreateCondBr(B.CreateICmpULT(Raw, B.getInt8(18)), Valid, Exit);
    B.SetInsertPoint(Valid);
    B.CreateBr(Merge);
    B.SetInsertPoint(Merge);
    auto *Index = B.CreatePHI(B.getInt64Ty(), 2, "choice");
    // Record 1 on the default edge, record 0 on the guarded edge: constant
    // incoming values, so every feasible binding is enumerable, and the two
    // records repeat the same field value so the fold proves them equal.
    Index->addIncoming(B.getInt64(RecordSize * Slots), Default);
    Index->addIncoming(B.getInt64(0), Valid);
    Value *Ptr =
        B.CreateInBoundsGEP(B.getInt8Ty(), Out->getGlobalVariable("root"),
                            Index);
    Ptr = B.CreateInBoundsGEP(StructType::getTypeByName(C, "R"), Ptr, Slot);
    Ptr = B.CreateInBoundsGEP(B.getInt8Ty(), Ptr, B.getInt64(FieldOffset));
    auto *Load = B.CreateAlignedLoad(B.getInt32Ty(), Ptr, Align(FieldAlign));
    Load->setMetadata("ejit.period_const", MDNode::get(C, {}));
    B.CreateCall(Out->getFunction("consume"), {Load, RawSlot, Ptr});
    B.CreateBr(Exit);
    B.SetInsertPoint(Exit);
    B.CreateRetVoid();
    ASSERT_FALSE(verifyModule(*Out, &errs()));
  };

  LLVMContext WithContext, WithoutContext;
  std::unique_ptr<Module> With, Without;
  Build(WithContext, With);
  Build(WithoutContext, Without);
  PeriodArrayRegistry Registry;
  Registry.registerStaticVar("root", Data.data());
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 0});
  Ctx.optLevel = OptimizationLevel::L2;
  Ctx.tier = CompileTier::Baseline;
  EJitOptimizer Reference(Registry);
  Reference.runPipeline(*Without, Ctx);
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;
  EJitOptimizer Observed(Registry);
  Observed.runPipeline(*With, Ctx);
  EXPECT_EQ(printModule(*With), printModule(*Without));
  EXPECT_EQ(Capture.uncovered, 0u);
  ASSERT_EQ(Capture.watches.size(), 1u);
  const auto &W = Capture.watches.front();
  EXPECT_EQ(W.function, "probe");
  EXPECT_EQ(W.period, "static");
  EXPECT_EQ(W.object, "root");
  EXPECT_EQ(W.objectBase, reinterpret_cast<uintptr_t>(Data.data()));
  // The slot dimension is driven by a local free-dim marker wrapping a
  // load, not a function argument: the row reports src=const.
  EXPECT_EQ(W.freeArg, 0xFF);
  EXPECT_EQ(W.expected, Expected);
  // The residue enumeration evaluates under the exported witness binding
  // (record 0), so the probed addresses are record 0's slots.
  ASSERT_EQ(W.addresses.size(), Slots);
  for (unsigned R = 0; R < Slots; ++R)
    EXPECT_EQ(W.addresses[R],
              reinterpret_cast<uintptr_t>(Data.data() + R * RecordSize +
                                         FieldOffset));
}

TEST(EJitAsan, ReportsTailResidueOnlyAfterPublication) {
  auto State = std::make_unique<EJitSharedTaskPoolState>();
  EJitSharedTaskPool Pool;
  Pool.bind(State.get());
  Pool.setMode(EJitCompileMode::Async);
  EJitAsan Verifier;
  uint32_t Data[5] = {7, 7, 7, 7, 7};
  uintptr_t Root = reinterpret_cast<uintptr_t>(Data);
  struct Context {
    EJitAsan &verifier;
    uint32_t (&data)[5];
    uintptr_t &root;
  } Ctx{Verifier, Data, Root};
  Pool.setCompiler(
      [](void *Ptr, const EJitCompileRequest &Req, void **Out) {
        auto &C = *static_cast<Context *>(Ptr);
        EJitAsanCapture Capture;
        auto &W = Capture.watches.emplace_back();
        W.function = "probe";
        W.object = "cfg";
        W.period = "cell";
        W.objectBase = reinterpret_cast<uintptr_t>(C.data);
        W.freeArg = 0;
        W.width = 32;
        W.expected = 7;
        uint32_t Endian = 1;
        W.littleEndian = *reinterpret_cast<uint8_t *>(&Endian) != 0;
        for (auto &Value : C.data)
          W.addresses.push_back(reinterpret_cast<uintptr_t>(&Value));
        EJitAsanWatch Leaf;
        Leaf.function = "probe";
        Leaf.object = "pointer_cfg";
        Leaf.period = "cell";
        Leaf.objectBase = C.root;
        Leaf.width = 32;
        Leaf.expected = 7;
        Leaf.littleEndian = W.littleEndian;
        Leaf.rootSlot = reinterpret_cast<uintptr_t>(&C.root);
        Leaf.expectedRoot = C.root;
        Leaf.addresses.push_back(C.root);
        Capture.watches.push_back(std::move(Leaf));
        *Out = reinterpret_cast<void *>(&codeMarker);
        C.verifier.onCompiled(Req, reinterpret_cast<uintptr_t>(*Out),
                             std::move(Capture));
        return true;
      },
      &Ctx);
  Pool.setPublishCallback(
      [](void *Ptr, const EJitCompileRequest &Req, bool Published) {
        static_cast<Context *>(Ptr)->verifier.onPublished(Req, Published);
      },
      &Ctx);
  ASSERT_EQ(Pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  ASSERT_TRUE(Pool.setInstanceEnabled(0, 1, true));
  const EJitDimPair Dim{0, 1};
  ASSERT_EQ(Pool.compileOrGet(44, &Dim, 1,
                             reinterpret_cast<void *>(&codeMarker)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  auto Level = ejit_get_log_level();
  ejit_set_log_level(EJIT_LOG_INFO);
  auto Dump = [&] {
    testing::internal::CaptureStdout();
    EXPECT_TRUE(Verifier.dumpNow(Pool));
    return testing::internal::GetCapturedStdout();
  };
  EXPECT_NE(Dump().find("checks=0"), std::string::npos);
  ASSERT_TRUE(Pool.pollOne());
  auto Live = Dump();
  EXPECT_NE(Live.find("checked_active=6/6"), std::string::npos);
  EXPECT_NE(Live.find("watches=2/81920"), std::string::npos);
  __atomic_store_n(&Data[4], 9u, __ATOMIC_RELEASE);
  auto Mismatch = Dump();
  EXPECT_NE(Mismatch.find("residue=4/5 kind=value"), std::string::npos);
  EXPECT_NE(Mismatch.find("expected=0x7 first=0x9"), std::string::npos);
  EXPECT_NE(Mismatch.find("object=cfg@0x"), std::string::npos);
  EXPECT_NE(Mismatch.find("offset=0x10"), std::string::npos);
  EXPECT_NE(Mismatch.find("src=arg0 "), std::string::npos);
  // The rate is the location's lifetime ratio (samples accumulate from
  // before the first mismatch too), so its exact value depends on scan
  // rotation; assert presence and a non-zero ratio instead of an exact one.
  EXPECT_NE(Mismatch.find("count="), std::string::npos);
  EXPECT_NE(Mismatch.find("rate="), std::string::npos);
  EXPECT_EQ(Mismatch.find("rate=0.0%"), std::string::npos);
  // Sticky records: the recorded location re-reports on every dump; it is
  // not unregistered while still mismatching.
  EXPECT_NE(Dump().find("residue=4/5"), std::string::npos);
  __atomic_store_n(&Root, uintptr_t(1), __ATOMIC_RELEASE);
  auto ChangedRoot = Dump();
  EXPECT_NE(ChangedRoot.find("object=pointer_cfg@0x"), std::string::npos);
  EXPECT_NE(ChangedRoot.find("kind=pointer-root"), std::string::npos);
  EXPECT_NE(ChangedRoot.find("first=0x1"), std::string::npos);
  EXPECT_EQ(ChangedRoot.find("blocked_root=0"), std::string::npos);
  // Recovery does not unregister a recorded location either: the rows stay
  // listed with their frozen counts while samples keep growing, so the
  // printed rate drops below 100%.
  __atomic_store_n(&Data[4], 7u, __ATOMIC_RELEASE);
  __atomic_store_n(&Root, reinterpret_cast<uintptr_t>(Data),
                   __ATOMIC_RELEASE);
  auto Recovered = Dump();
  EXPECT_NE(Recovered.find("residue=4/5"), std::string::npos);
  EXPECT_NE(Recovered.find("residue=0/1"), std::string::npos);
  EXPECT_EQ(Recovered.find("rate=100.0%"), std::string::npos);
  EXPECT_TRUE(Pool.setInstanceEnabled(0, 1, false));
  EXPECT_NE(Dump().find("versions=0 staged=0 watches=0/81920"),
            std::string::npos);
  ejit_set_log_level(Level);
  Pool.ownerShutdown();
}

//===----------------------------------------------------------------------===//
// Entry instrumentation: shared blob, rotation gate, mismatch hook, stub
// emission, and end-to-end execution. The blob is process-global state, so
// every test below resets it first for determinism.
//===----------------------------------------------------------------------===//

namespace {
// One constant-offset bound fold through a bound_ptr argument: the shape the
// entry stub covers (modulus == 1). The metadata mirrors what Clang emits
// for ejit_bound_ptr (see the EJitPointerPeriod loop-unroll fixture): a
// single field node {i64 offset, i64 size}.
constexpr uint32_t kBoundFieldOffset = 4;
constexpr uint32_t kBoundExpected = 0x11223344u;

std::unique_ptr<Module> makeBoundFoldModule(LLVMContext &C) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    declare void @observe(i32)
    define i32 @probe(i8 %cell, ptr %cfg) !ejit.metadata !0 {
    entry:
      %p = getelementptr inbounds i8, ptr %cfg, i64 4
      %v = load i32, ptr %p, !ejit.period_const !7
      ret i32 %v
    }
    !0 = !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_dim", !"cell", i32 0}
    !3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 8, !4}
    !4 = !{i64 4, i64 4}
    !7 = !{}
  )",
                               Err, C);
  if (!M)
    ADD_FAILURE() << "bad test IR: " << Err.getMessage().str();
  return M;
}

void resetAsanBlob() {
  // The shared blob is loader-zero-filled in production; reset it the same
  // way here so tests are order-independent. The state is fixed-width
  // scalars and atomics only, so the byte-zeroing is the loader's own.
  std::memset(static_cast<void *>(&gEJitAsanState), 0,
              sizeof(gEJitAsanState));
}

// LLJIT (the end-to-end test) needs the native target registered once per
// process, mirroring EJitVpScalarSpecTest.
static const bool kTargetsInitialized = [] {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
  return true;
}();

unsigned countCallsTo(const Module &M, StringRef Callee) {
  unsigned N = 0;
  for (const Function &F : M)
    for (const Instruction &I : instructions(F))
      if (auto *Call = dyn_cast<CallInst>(&I))
        if (Call->getCalledFunction() &&
            Call->getCalledFunction()->getName() == Callee)
          ++N;
  return N;
}
} // namespace

TEST(EJitAsan, GateRotatesPerCoreWindow) {
  resetAsanBlob();
  const uint32_t SlotA = ejitAsanClaimSlot("fn_a");
  const uint32_t SlotB = ejitAsanClaimSlot("fn_b");
  ASSERT_EQ(SlotA, 0u);
  ASSERT_EQ(SlotB, 1u);

  EJitCoreId::setCurrentForTest(0);
  // Fresh core arms slot 0's window instead of skipping it.
  for (unsigned I = 0; I < EJIT_ASAN_ROTATION_WINDOW; ++I)
    EXPECT_EQ(__ejit_asan_gate(SlotA), 1);
  // Window exhausted: the next call advances to slot 1, so slot A is dormant
  // and slot B is served.
  EXPECT_EQ(__ejit_asan_gate(SlotA), 0);
  EXPECT_EQ(__ejit_asan_gate(SlotB), 1);
  EXPECT_EQ(gEJitAsanState.cells[0].checks,
            EJIT_ASAN_ROTATION_WINDOW + 1u);

  // Rotation state is per core: core 1 is unaffected by core 0's rotation.
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(__ejit_asan_gate(SlotA), 1);
  EXPECT_EQ(__ejit_asan_gate(SlotB), 0);

  // Cores beyond the cell capacity are safely degraded: never active.
  EJitCoreId::setCurrentForTest(EJIT_ASAN_MAX_CORES);
  EXPECT_EQ(__ejit_asan_gate(SlotA), 0);
  EJitCoreId::resetForTest();
}

TEST(EJitAsan, MismatchHookFreezesFirstRecord) {
  resetAsanBlob();
  const uint32_t Slot = ejitAsanClaimSlot("probe");
  const uint32_t Site = ejitAsanPublishSite(Slot, "probe", "cell",
                                             /*tier=*/0, /*argNo=*/1,
                                             /*width=*/32, /*align=*/4,
                                             kBoundFieldOffset, kBoundExpected);
  ASSERT_NE(Site, kEJitAsanNoSite);

  uint32_t Buffer = 0x55667788u;
  __ejit_asan_mismatch(Site, &Buffer, 0x55667788u);
  __ejit_asan_mismatch(Site, &Buffer, 0x99aabbccu);

  const EJitAsanSticky &R = gEJitAsanState.sticky[Site];
  EXPECT_EQ(R.recorded.loadRelaxed(), 2u);
  // The FIRST mismatch froze the seen value and the actual argument pointer
  // (the family member that diverged); the second only grew the count.
  EXPECT_EQ(R.firstValue, 0x55667788u);
  EXPECT_EQ(R.firstArg, reinterpret_cast<uint64_t>(&Buffer));
  EXPECT_EQ(R.count.loadRelaxed(), 2u);
  EXPECT_EQ(gEJitAsanState.slots[Slot].activeSites.loadRelaxed(), 1u);

  // An unpublishable site id is ignored (stale stub in a recycled slot).
  __ejit_asan_mismatch(Site + 1, &Buffer, 1u);
  EXPECT_EQ(gEJitAsanState.sticky[Site + 1].recorded.loadRelaxed(), 0u);
}

TEST(EJitAsan, EmitsEntryStubForConstantOffsetBoundFold) {
  for (bool Enabled : {false, true}) {
    SCOPED_TRACE(Enabled);
    resetAsanBlob();
    auto C = std::make_unique<LLVMContext>();
    auto M = makeBoundFoldModule(*C);
    ASSERT_TRUE(M);
    uint64_t Object[1] = {uint64_t(kBoundExpected) << 32};
    PeriodArrayRegistry Registry;
    SpecializationContext Ctx;
    Ctx.fnName = "probe";
    Ctx.dimensions.push_back({"cell", 1});
    Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Object),
                                  /*size=*/8, /*argIndex=*/1, /*instance=*/1});
    Ctx.asanEntryChecks = Enabled;
    EJitAsanCapture Capture;
    Ctx.asanCapture = &Capture;

    EJitOptimizer(Registry).runPipeline(*M, Ctx);
    ASSERT_FALSE(verifyModule(*M, &errs()));

    // The fold happened in both runs: the entry returns the baked constant
    // and the capture recorded one constant-offset bound fold.
    ReturnInst *Ret = nullptr;
    for (Instruction &I : instructions(M->getFunction("probe")))
      if (auto *R = dyn_cast<ReturnInst>(&I))
        Ret = R;
    auto *Baked = dyn_cast_or_null<ConstantInt>(Ret->getReturnValue());
    ASSERT_NE(Baked, nullptr);
    EXPECT_EQ(Baked->getZExtValue(), kBoundExpected);
    ASSERT_EQ(Capture.entryFolds.size(), 1u);
    const auto &EF = Capture.entryFolds.front();
    EXPECT_EQ(EF.argNo, 1u);
    EXPECT_EQ(EF.byteOffset, kBoundFieldOffset);
    EXPECT_EQ(EF.expected, kBoundExpected);
    EXPECT_EQ(EF.width, 32u);

    if (!Enabled) {
      // Mode off: the fold and its capture are unchanged, but no stub is
      // emitted and no site is published.
      EXPECT_EQ(countCallsTo(*M, "__ejit_asan_gate"), 0u);
      EXPECT_EQ(countCallsTo(*M, "__ejit_asan_mismatch"), 0u);
      EXPECT_EQ(EF.siteId, kEJitAsanNoSite);
      EXPECT_EQ(gEJitAsanState.siteCounter.loadRelaxed(), 0u);
      continue;
    }

    // Mode on: gate call once in the entry head, one check load compared
    // against the baked constant, and the mismatch hook reachable.
    EXPECT_EQ(countCallsTo(*M, "__ejit_asan_gate"), 1u);
    EXPECT_EQ(countCallsTo(*M, "__ejit_asan_mismatch"), 1u);
    ASSERT_NE(EF.siteId, kEJitAsanNoSite);
    ASSERT_LT(EF.siteId, EJIT_ASAN_MAX_SITES);
    const EJitAsanSite &S = gEJitAsanState.sites[EF.siteId];
    EXPECT_EQ(S.published.loadRelaxed(), 1u);
    EXPECT_STREQ(S.function, "probe");
    EXPECT_STREQ(S.period, "cell");
    EXPECT_EQ(S.argNo, 1u);
    EXPECT_EQ(S.byteOffset, kBoundFieldOffset);
    EXPECT_EQ(S.expected, kBoundExpected);
    EXPECT_EQ(S.width, 32u);
    EXPECT_EQ(S.slot, 0u);
    EXPECT_EQ(gEJitAsanState.slots[0].activeSites.loadRelaxed(), 1u);
    bool SawCheck = false;
    for (Instruction &I : instructions(M->getFunction("probe")))
      if (auto *Cmp = dyn_cast<ICmpInst>(&I))
        if (Cmp->getPredicate() == ICmpInst::ICMP_EQ)
          if (auto *K = dyn_cast<ConstantInt>(Cmp->getOperand(1)))
            if (K->getZExtValue() == kBoundExpected)
              SawCheck = true;
    EXPECT_TRUE(SawCheck);
  }
}

TEST(EJitAsan, UnrolledLoopBoundFoldsGetEntryStubs) {
  // Same bound-fold machinery as EJitPointerPeriod's loop-unroll fixture.
  // The loop carries llvm.loop.unroll.full: by the last StructFieldPass run
  // the PHI induction variable is gone and each unrolled copy has a
  // CONSTANT offset — exactly the shape the entry stub covers. All four
  // folds must be captured (each with its own field offset and expected
  // value) behind ONE gate call at the function entry.
  resetAsanBlob();
  LLVMContext C;
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    declare void @observe(i8, i16)
    define void @probe(i8 %cell, i16 %slot, ptr %cfg) !ejit.metadata !0 {
    entry:
      br label %loop
    loop:
      %i = phi i64 [0, %entry], [%next, %loop]
      %p = getelementptr inbounds i8, ptr %cfg, i64 %i
      %v = load i8, ptr %p, !ejit.period_const !7
      call void @observe(i8 %v, i16 %slot)
      %next = add nuw nsw i64 %i, 1
      %more = icmp ult i64 %next, 4
      br i1 %more, label %loop, label %exit, !llvm.loop !8
    exit:
      ret void
    }
    !0 = !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_dim", !"cell", i32 0}
    !3 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 4, !4, !5, !6, !10}
    !4 = !{i64 0, i64 1}
    !5 = !{i64 1, i64 1}
    !6 = !{i64 2, i64 1}
    !7 = !{}
    !8 = distinct !{!8, !9}
    !9 = !{!"llvm.loop.unroll.full"}
    !10 = !{i64 3, i64 1}
  )",
                               Err, C);
  ASSERT_TRUE(M);
  const uint8_t Data[] = {7, 9, 11, 13};
  PeriodArrayRegistry Registry;
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  Ctx.boundPointers.push_back({Data, sizeof(Data), /*argIndex=*/2,
                                /*instance=*/1});
  Ctx.asanEntryChecks = true;
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  ASSERT_EQ(Capture.entryFolds.size(), 4u);
  // One gate call for the whole entry; one check (and one hook call) per
  // unrolled fold.
  EXPECT_EQ(countCallsTo(*M, "__ejit_asan_gate"), 1u);
  EXPECT_EQ(countCallsTo(*M, "__ejit_asan_mismatch"), 4u);
  std::set<uint64_t> Offsets, Expecteds;
  for (const auto &EF : Capture.entryFolds) {
    EXPECT_EQ(EF.argNo, 2u);
    EXPECT_EQ(EF.width, 8u);
    EXPECT_NE(EF.siteId, kEJitAsanNoSite);
    Offsets.insert(EF.byteOffset);
    Expecteds.insert(EF.expected);
  }
  EXPECT_EQ(Offsets, (std::set<uint64_t>{0, 1, 2, 3}));
  EXPECT_EQ(Expecteds, (std::set<uint64_t>{7, 9, 11, 13}));
  EXPECT_EQ(gEJitAsanState.slots[0].activeSites.loadRelaxed(), 4u);
}

TEST(EJitAsan, EntryStubEndToEndRecordsDivergentBuffer) {
  resetAsanBlob();
  EJitCoreId::setCurrentForTest(0);
  auto C = std::make_unique<LLVMContext>();
  auto M = makeBoundFoldModule(*C);
  ASSERT_TRUE(M);
  // The snapshot the compile baked from (field at +4 = expected value).
  uint64_t Good[1] = {uint64_t(kBoundExpected) << 32};
  // A divergent family member: same field offset, different value. The
  // field is the u32 at offset 4, so 0x77abcdef occupies bytes 4..7.
  uint64_t Bad[1] = {0x77abcdef00000000ULL};

  PeriodArrayRegistry Registry;
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Good),
                               /*size=*/8, /*argIndex=*/1, /*instance=*/1});
  Ctx.asanEntryChecks = true;
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;
  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  ASSERT_EQ(Capture.entryFolds.size(), 1u);
  const uint32_t Site = Capture.entryFolds.front().siteId;
  ASSERT_NE(Site, kEJitAsanNoSite);

  // Execute the specialized function for real: resolve the two hooks as
  // absolute symbols in the LLJIT dylib (the same link shape the EJIT
  // engine produces on the board).
  auto J = cantFail(orc::LLJITBuilder().create());
  orc::SymbolMap Syms;
  Syms[J->mangleAndIntern("__ejit_asan_gate")] = orc::ExecutorSymbolDef(
      orc::ExecutorAddr::fromPtr(&__ejit_asan_gate), JITSymbolFlags::Exported);
  Syms[J->mangleAndIntern("__ejit_asan_mismatch")] =
      orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(&__ejit_asan_mismatch),
                             JITSymbolFlags::Exported);
  cantFail(J->getMainJITDylib().define(orc::absoluteSymbols(std::move(Syms))));
  cantFail(J->addIRModule(
      orc::ThreadSafeModule(std::move(M), std::move(C))));
  auto Sym = cantFail(J->lookup("probe"));
  auto *Probe =
      jitTargetAddressToFunction<int32_t (*)(uint8_t, void *)>(Sym.getValue());

  // First call: fresh core arms this function's slot, the check runs against
  // the divergent buffer, and the hook records it.
  EXPECT_EQ(Probe(1, Bad), static_cast<int32_t>(kBoundExpected));
  const EJitAsanSticky &R = gEJitAsanState.sticky[Site];
  EXPECT_EQ(R.recorded.loadRelaxed(), 2u);
  EXPECT_EQ(R.firstValue, 0x77abcdefu);
  // LE host: the stub's typed load of the field at +4 of the divergent
  // buffer sees the u32 formed by bytes 4..7.
  EXPECT_EQ(R.firstArg, reinterpret_cast<uint64_t>(Bad));
  EXPECT_EQ(R.count.loadRelaxed(), 1u);
  // Report-only: the call returned the BAKED constant, not the buffer value.
  // A consistent family member on the next call records nothing new.
  EXPECT_EQ(Probe(1, Good), static_cast<int32_t>(kBoundExpected));
  EXPECT_EQ(R.count.loadRelaxed(), 1u);
  EXPECT_EQ(R.firstArg, reinterpret_cast<uint64_t>(Bad));
  EJitCoreId::resetForTest();
}

TEST(EJitAsan, DumpReportsEntryRowsAndStats) {
  resetAsanBlob();
  const uint32_t Slot = ejitAsanClaimSlot("probe");
  const uint32_t Site = ejitAsanPublishSite(Slot, "probe", "cell",
                                             /*tier=*/0, /*argNo=*/1,
                                             /*width=*/32, /*align=*/4,
                                             kBoundFieldOffset, kBoundExpected);
  ASSERT_NE(Site, kEJitAsanNoSite);
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(__ejit_asan_gate(Slot), 1); // Serve one check on this core.
  EJitCoreId::resetForTest();
  uint32_t Buffer = 0xdeadbeefu;
  __ejit_asan_mismatch(Site, &Buffer, 0xdeadbeefu);

  // The dump is driven directly (no pool needed with no probe watches).
  EJitSharedTaskPool Pool;
  EJitAsan Verifier;
  auto Level = ejit_get_log_level();
  ejit_set_log_level(EJIT_LOG_INFO);
  testing::internal::CaptureStdout();
  EXPECT_TRUE(Verifier.dumpNow(Pool));
  const std::string Out = testing::internal::GetCapturedStdout();
  ejit_set_log_level(Level);

  EXPECT_NE(Out.find("kind=entry"), std::string::npos);
  EXPECT_NE(Out.find("object=bound@0x"), std::string::npos);
  EXPECT_NE(Out.find("src=bound1"), std::string::npos);
  EXPECT_NE(Out.find("fn=probe"), std::string::npos);
  EXPECT_NE(Out.find("expected=0x11223344"), std::string::npos);
  EXPECT_NE(Out.find("first=0xdeadbeef"), std::string::npos);
  EXPECT_NE(Out.find("entry_checks=1"), std::string::npos);
  EXPECT_NE(Out.find("entry_sites=1"), std::string::npos);
}

TEST(EJitAsan, SkipsEntryFoldsOwnedByOtherEntries) {
  // The capture pass runs over every defined function of the module, and a
  // fold reached through a second ejit_entry's bound FORMAL belongs to
  // THAT function's argument list, not to the specialized entry's. The
  // emitter must not index the specialized function's arguments with it:
  // pin the regression where this shape crashed the worker compile
  // (review P1: probe, bound arg 1, calls helper — a second entry whose
  // bound formal is index 2; helper's fold used to be emitted into probe).
  resetAsanBlob();
  LLVMContext C;
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    declare void @observe(i32)
    define i32 @probe(i8 %cell, ptr %cfg) !ejit.metadata !0 {
    entry:
      %r = call i32 @helper(i8 %cell, ptr %cfg, ptr %cfg)
      ret i32 %r
    }
    define i32 @helper(i8 %cell, ptr %cfg, ptr %h) !ejit.metadata !5 {
    entry:
      %p = getelementptr inbounds i8, ptr %h, i64 4
      %v = load i32, ptr %p, !ejit.period_const !7
      ret i32 %v
    }
    !0 = !{!1, !2, !8}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_dim", !"cell", i32 0}
    !5 = !{!6, !2, !10}
    !6 = !{!"ejit_entry"}
    !7 = !{}
    !8 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 8, !9}
    !9 = !{i64 4, i64 4}
    !10 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 8, !9}
  )",
                               Err, C);
  ASSERT_TRUE(M);
  uint64_t Object[1] = {uint64_t(kBoundExpected) << 32};
  PeriodArrayRegistry Registry;
  SpecializationContext Ctx;
  Ctx.fnName = "probe"; // The specialized entry: helper is NOT it.
  Ctx.dimensions.push_back({"cell", 1});
  Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Object),
                                /*size=*/8, /*argIndex=*/1, /*instance=*/1});
  Ctx.asanEntryChecks = true;
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  // Helper's fold IS captured (with its own function name and argument
  // index), and NOTHING is emitted into probe: no site, no stub. (Before
  // the fix this crashed inside the emitter with F->getArg(2) on probe's
  // two-argument list.)
  ASSERT_EQ(Capture.entryFolds.size(), 1u);
  const auto &EF = Capture.entryFolds.front();
  EXPECT_EQ(EF.function, "helper");
  EXPECT_EQ(EF.argNo, 2u);
  EXPECT_EQ(EF.siteId, kEJitAsanNoSite);
  EXPECT_EQ(countCallsTo(*M, "__ejit_asan_gate"), 0u);
  EXPECT_EQ(countCallsTo(*M, "__ejit_asan_mismatch"), 0u);
  EXPECT_EQ(gEJitAsanState.siteCounter.loadRelaxed(), 0u);
}

TEST(EJitAsan, RotationStatusPrintsPerCoreState) {
  resetAsanBlob();
  const uint32_t Slot = ejitAsanClaimSlot("probe");
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(__ejit_asan_gate(Slot), 1);
  EJitCoreId::resetForTest();

  auto Level = ejit_get_log_level();
  ejit_set_log_level(EJIT_LOG_INFO);
  testing::internal::CaptureStdout();
  EXPECT_EQ(ejit_asan_rotation_status(), EJIT_OK);
  const std::string Out = testing::internal::GetCapturedStdout();
  ejit_set_log_level(Level);

  EXPECT_NE(Out.find("[EJIT-ASAN] rotation slots=1/"), std::string::npos);
  EXPECT_NE(Out.find("rotation core=0"), std::string::npos);
  EXPECT_NE(Out.find("slot=0(probe)"), std::string::npos);
  EXPECT_NE(Out.find("checks=1"), std::string::npos);
}
