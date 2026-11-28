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
  ejitAsanSetSlotBound(SlotA, 2);

  EJitCoreId::setCurrentForTest(0);
  // Fresh core arms slot 0: each activated call checks ONE bound argument,
  // so the window spans bound x W calls.
  const unsigned W = EJIT_ASAN_ROTATION_WINDOW;
  for (unsigned I = 0; I < W; ++I)
    EXPECT_EQ(__ejit_asan_gate(SlotA), 0);
  for (unsigned I = 0; I < W; ++I)
    EXPECT_EQ(__ejit_asan_gate(SlotA), 1);
  // Window exhausted: the next call advances to slot 1, so slot A is dormant
  // and slot B (no registered stubs: one sub-window) is served.
  EXPECT_EQ(__ejit_asan_gate(SlotA), -1);
  EXPECT_EQ(__ejit_asan_gate(SlotB), 0);
  EXPECT_EQ(gEJitAsanState.cells[0].checks, 2 * W + 1u);

  // Rotation state is per core: core 1 is unaffected by core 0's rotation.
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(__ejit_asan_gate(SlotA), 0);
  EXPECT_EQ(__ejit_asan_gate(SlotB), -1);

  // Cores beyond the cell capacity are safely degraded: never active.
  EJitCoreId::setCurrentForTest(EJIT_ASAN_MAX_CORES);
  EXPECT_EQ(__ejit_asan_gate(SlotA), -1);
  EJitCoreId::resetForTest();
}

TEST(EJitAsan, GateBoundsSubWindowsByArmedSnapshot) {
  resetAsanBlob();
  const uint32_t Slot = ejitAsanClaimSlot("fn");
  EJitCoreId::setCurrentForTest(0);
  const unsigned W = EJIT_ASAN_ROTATION_WINDOW;
  // No registered stubs: degrade to a single sub-window.
  EXPECT_EQ(__ejit_asan_gate(Slot), 0);
  // A registration mid-window does not re-arm: the snapshot keeps serving
  // sub 0 for the rest of the window.
  ejitAsanSetSlotBound(Slot, 2);
  for (unsigned I = 1; I < W; ++I)
    EXPECT_EQ(__ejit_asan_gate(Slot), 0);
  // The new count applies from the next arming: W calls of sub 0, then W
  // calls of sub 1.
  for (unsigned I = 0; I < W; ++I)
    EXPECT_EQ(__ejit_asan_gate(Slot), 0);
  for (unsigned I = 0; I < W; ++I)
    EXPECT_EQ(__ejit_asan_gate(Slot), 1);
  // Monotonic maximum: a smaller count never shrinks the window.
  ejitAsanSetSlotBound(Slot, 1);
  EXPECT_EQ(__ejit_asan_gate(Slot), 0);
  EXPECT_EQ(gEJitAsanState.cells[0].bound, 2u);
  EJitCoreId::resetForTest();
}

TEST(EJitAsan, MismatchHookFreezesFirstRecord) {
  resetAsanBlob();
  const uint32_t Slot = ejitAsanClaimSlot("probe");
  const uint32_t Site = ejitAsanPublishSite(Slot, "probe", "cell",
                                            kEJitAsanKindFolded,
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

TEST(EJitAsan, MismatchHookDriftProtocolObservesAndFreezes) {
  resetAsanBlob();
  const uint32_t Slot = ejitAsanClaimSlot("probe");
  const uint32_t Site = ejitAsanPublishSite(Slot, "probe", "cell",
                                            kEJitAsanKindDrift,
                                            /*tier=*/0, /*argNo=*/1,
                                            /*width=*/32, /*align=*/4,
                                            kBoundFieldOffset, kBoundExpected);
  ASSERT_NE(Site, kEJitAsanNoSite);
  const EJitAsanSticky &R = gEJitAsanState.sticky[Site];
  uint32_t Buffer = 0;

  // A same-value observation counts a sample only; the publish-seeded
  // baseline prevents a pseudo-change on the first observation.
  __ejit_asan_mismatch(Site, &Buffer, kBoundExpected);
  EXPECT_EQ(R.samples.loadRelaxed(), 1u);
  EXPECT_EQ(R.driftChanges.loadRelaxed(), 0u);
  EXPECT_EQ(R.lastSeen.loadRelaxed(), kBoundExpected);
  EXPECT_EQ(R.recorded.loadRelaxed(), 0u);

  // A changed value transfers lastSeen, counts one change, and freezes
  // the first change with the diverging call's pointer.
  __ejit_asan_mismatch(Site, &Buffer, 0x12345678u);
  EXPECT_EQ(R.samples.loadRelaxed(), 2u);
  EXPECT_EQ(R.driftChanges.loadRelaxed(), 1u);
  EXPECT_EQ(R.lastSeen.loadRelaxed(), 0x12345678u);
  EXPECT_EQ(R.recorded.loadRelaxed(), 2u);
  EXPECT_EQ(R.firstValue, 0x12345678u);
  EXPECT_EQ(R.firstArg, reinterpret_cast<uint64_t>(&Buffer));

  // Later changes keep counting; the frozen record stays.
  __ejit_asan_mismatch(Site, &Buffer, 0x9u);
  EXPECT_EQ(R.driftChanges.loadRelaxed(), 2u);
  EXPECT_EQ(R.lastSeen.loadRelaxed(), 0x9u);
  EXPECT_EQ(R.firstValue, 0x12345678u);

  // Re-observing the current value samples without counting a change.
  __ejit_asan_mismatch(Site, &Buffer, 0x9u);
  EXPECT_EQ(R.samples.loadRelaxed(), 4u);
  EXPECT_EQ(R.driftChanges.loadRelaxed(), 2u);
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
    EXPECT_EQ(gEJitAsanState.slots[0].boundCount.loadRelaxed(), 1u);
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
  // All four folds share one bound argument: one sub-window.
  EXPECT_EQ(gEJitAsanState.slots[0].boundCount.loadRelaxed(), 1u);
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
                                            kEJitAsanKindFolded,
                                            /*tier=*/0, /*argNo=*/1,
                                            /*width=*/32, /*align=*/4,
                                            kBoundFieldOffset, kBoundExpected);
  ASSERT_NE(Site, kEJitAsanNoSite);
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(__ejit_asan_gate(Slot), 0); // Serve one check on this core.
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
  EXPECT_NE(Out.find("entry_folded_sites=1"), std::string::npos);
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

TEST(EJitAsan, DriftWalkCapturesUnfoldedBoundLoadAsDriftSite) {
  // An atomic period_const load through a bound_ptr argument never folds
  // (atomics are excluded from substitution), so it survives to the final
  // module — the unfolded group the drift walk exists to watch. The walk
  // records a folded=false entry with the value the compile saw, and the
  // IR is untouched.
  resetAsanBlob();
  auto C = std::make_unique<LLVMContext>();
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    declare void @observe(i32)
    define i32 @probe(i8 %cell, ptr %cfg) !ejit.metadata !0 {
    entry:
      %p = getelementptr inbounds i8, ptr %cfg, i64 4
      %v = load atomic i32, ptr %p acquire, align 4, !ejit.period_const !7
      ret i32 %v
    }
    !0 = !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_dim", !"cell", i32 0}
    !3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 8, !4}
    !4 = !{i64 4, i64 4}
    !7 = !{}
  )",
                               Err, *C);
  ASSERT_TRUE(M);
  uint64_t Object[1] = {0xCAFEBABE00000000ULL};
  PeriodArrayRegistry Registry;
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Object),
                                /*size=*/8, /*argIndex=*/1, /*instance=*/1});
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  bool SawAtomic = false;
  for (Instruction &I : instructions(M->getFunction("probe")))
    if (auto *LI = dyn_cast<LoadInst>(&I))
      SawAtomic |= LI->isAtomic();
  EXPECT_TRUE(SawAtomic);
  EXPECT_EQ(gEJitAsanState.siteCounter.loadRelaxed(), 0u);
  EXPECT_EQ(Capture.driftUncovered, 0u);
  ASSERT_EQ(Capture.entryFolds.size(), 1u);
  const auto &EF = Capture.entryFolds.front();
  EXPECT_FALSE(EF.folded);
  EXPECT_EQ(EF.function, "probe");
  EXPECT_EQ(EF.period, "cell");
  EXPECT_EQ(EF.argNo, 1u);
  EXPECT_EQ(EF.byteOffset, 4u);
  EXPECT_EQ(EF.width, 32u);
  // Baseline = the observed value at specialization time (LE host: the u32
  // at bytes 4..7 of the buffer).
  EXPECT_EQ(EF.expected, 0xCAFEBABEu);
  EXPECT_EQ(EF.siteId, kEJitAsanNoSite);
}

TEST(EJitAsan, DriftWalkRecognizesAtomicReadsWithoutMarker) {
  // The shape real Clang emits for an atomic read of an annotated bound
  // field: an atomic load with NO marker (atomics bypass the load
  // decoration). The walk recognizes it through the bound metadata's field
  // ranges and records the drift site.
  resetAsanBlob();
  auto C = std::make_unique<LLVMContext>();
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    declare void @observe(i32)
    define i32 @probe(i8 %cell, ptr %cfg) !ejit.metadata !0 {
    entry:
      %p = getelementptr inbounds i8, ptr %cfg, i64 4
      %v = load atomic i32, ptr %p acquire, align 4
      %q = getelementptr inbounds i8, ptr %cfg, i64 8
      %u = load i32, ptr %q, align 4
      %r = add i32 %v, %u
      ret i32 %r
    }
    !0 = !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_dim", !"cell", i32 0}
    !3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 16, !4}
    !4 = !{i64 4, i64 4}
    !7 = !{}
  )",
                               Err, *C);
  ASSERT_TRUE(M);
  uint64_t Object[2] = {0xCAFEBABE00000000ULL, 0x5566778855667788ULL};
  PeriodArrayRegistry Registry;
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Object),
                                /*size=*/16, /*argIndex=*/1, /*instance=*/1});
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  EXPECT_EQ(Capture.driftUncovered, 0u);
  // Only the annotated atomic read at +4 records a site: the plain load at
  // +8 sits in no listed field and stays invisible.
  ASSERT_EQ(Capture.entryFolds.size(), 1u);
  const auto &EF = Capture.entryFolds.front();
  EXPECT_FALSE(EF.folded);
  EXPECT_TRUE(EF.atomic);
  EXPECT_EQ(EF.argNo, 1u);
  EXPECT_EQ(EF.byteOffset, 4u);
  EXPECT_EQ(EF.expected, 0xCAFEBABEu);
  EXPECT_EQ(gEJitAsanState.siteCounter.loadRelaxed(), 0u);
}

TEST(EJitAsan, DriftWalkEnumeratesAnnotatedFields) {
  // The full-scope watch set is metadata driven: every annotated field of
  // every registered object is enumerated whether or not any load of it
  // survived. Here the probe's only load folds, and the field still shows
  // up for the boot-lifetime probe table.
  resetAsanBlob();
  uint32_t Data[2][10];
  for (unsigned I = 0; I < 10; ++I) {
    Data[0][I] = 7;
    Data[1][I] = 11;
  }
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "cfg", Data, 2);
  auto C = std::make_unique<LLVMContext>();
  auto M = makeProbe(*C, /*Local=*/false, /*Mod=*/5);
  ASSERT_TRUE(M);
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  ASSERT_EQ(Capture.annotatedFields.size(), 1u);
  const auto &AF = Capture.annotatedFields.front();
  EXPECT_EQ(AF.object, "cfg");
  EXPECT_EQ(AF.period, "cell");
  EXPECT_EQ(AF.offset, 0u);
  EXPECT_EQ(AF.instanceCount, 2u);
  EXPECT_EQ(AF.width, 32u);
  EXPECT_TRUE(AF.littleEndian);
}

TEST(EJitAsan, DriftWalkDerivesNestedStructFieldWidths) {
  // Nested struct members: the frontend expands inner arrays so the
  // metadata carries absolute element-relative offsets; the width
  // derivation must descend array levels (stride modulo) and struct
  // members (exact member start) to the innermost scalar. A mid-member
  // offset resolves to no scalar and is skipped with a reason.
  resetAsanBlob();
  struct Inner { uint32_t value; };
  struct Outer { Inner in[3]; uint32_t pad; };
  Outer Data[2] = {};
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "nested", Data, 2);
  LLVMContext C;
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
    %struct.Inner = type { i32 }
    %struct.Outer = type { [3 x %struct.Inner], i32 }
    @nested = external global [2 x %struct.Outer], !ejit.metadata !0
    define i32 @probe(i8 %cell) !ejit.metadata !5 {
    entry:
      ret i32 0
    }
    !0 = !{!1, !2, !3, !4, !8}
    !1 = !{!"ejit_group_indexed", !"cell", i64 2}
    !2 = !{!"ejit_period_const_field", i64 0}
    !3 = !{!"ejit_period_const_field", i64 4}
    !4 = !{!"ejit_period_const_field", i64 8}
    !8 = !{!"ejit_period_const_field", i64 6}
    !5 = !{!6, !7}
    !6 = !{!"ejit_entry"}
    !7 = !{!"ejit_dim", !"cell", i32 0}
  )",
                               Err, C);
  ASSERT_TRUE(M);
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  // Offsets 0/4/8 are in[k].value scalars (32-bit); offset 6 lands mid
  // member and is skipped.
  ASSERT_EQ(Capture.annotatedFields.size(), 3u);
  const std::set<uint64_t> Offsets{0, 4, 8};
  std::set<uint64_t> Seen;
  for (const auto &AF : Capture.annotatedFields) {
    EXPECT_EQ(AF.object, "nested");
    EXPECT_EQ(AF.period, "cell");
    EXPECT_EQ(AF.instanceCount, 2u);
    EXPECT_EQ(AF.width, 32u);
    EXPECT_EQ(AF.base, reinterpret_cast<uintptr_t>(Data));
    EXPECT_EQ(AF.stride, 16u);
    Seen.insert(AF.offset);
  }
  EXPECT_EQ(Seen, Offsets);
}

TEST(EJitAsan, DriftWalkClassifiesUncoveredSurvivors) {
  // Survivors the entry cannot observe count into driftUncovered with a
  // reason: a volatile annotated load stays out (an observation load of our
  // own would change the access pattern the program promised); a global
  // load with a plain (non-free-dim) runtime index has no resolvable
  // offset; a residue-driven bound load (modulus 3 — a power-of-two
  // modulus would canonicalize the urem away — whose witness address lies
  // outside the view, so the fold declines) gets no stub site in v1; a
  // marker on an i128 field the GV's own metadata does not list (too
  // wide to materialize, so nothing folds it) is the
  // annotation-consistency smell; and an i64 load over a field the bound
  // metadata declares 4 bytes wide (the fold declines on the width
  // mismatch, the walk on the access not fitting the view) would read
  // past the exemplar both as a baseline and in the stub.
  resetAsanBlob();
  auto C = std::make_unique<LLVMContext>();
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
    @cfg = external global [2 x [10 x i32]], !ejit.metadata !0
    @cfg2 = external global [2 x [10 x i32]], !ejit.metadata !11
    define i32 @probe(i8 %cell, i16 %j, i16 %k, ptr %b) !ejit.metadata !5 {
    entry:
      %p = getelementptr inbounds i8, ptr %b, i64 4
      %v = load volatile i32, ptr %p, !ejit.period_const !7
      %p8 = getelementptr inbounds i8, ptr %b, i64 4
      %v8 = load i64, ptr %p8, !ejit.period_const !7
      %q = getelementptr inbounds [2 x [10 x i32]], ptr @cfg, i64 0, i64 1, i16 %k
      %w = load i32, ptr %q, !ejit.period_const !7
      %idx = urem i16 %j, 3
      %idxz = zext i16 %idx to i64
      %mul = mul i64 %idxz, 4
      %s = getelementptr inbounds i8, ptr %b, i64 %mul
      %s2 = getelementptr inbounds i8, ptr %s, i64 8
      %u = load i32, ptr %s2, !ejit.period_const !7
      %q2 = getelementptr inbounds [2 x [10 x i32]], ptr @cfg2, i64 0, i64 0, i64 0
      %w2 = load i128, ptr %q2, align 4, !ejit.period_const !7
      %x2 = add i128 %w2, 1
      %lo = trunc i128 %x2 to i32
      %lo8 = trunc i64 %v8 to i32
      %r1 = add i32 %v, %w
      %r2 = add i32 %r1, %u
      %r3 = add i32 %r2, %lo
      %r4 = add i32 %r3, %lo8
      ret i32 %r4
    }
    !0 = !{!1, !2}
    !1 = !{!"ejit_group_indexed", !"cell", i64 2}
    !2 = !{!"ejit_period_const_field", i64 0}
    !11 = !{!12}
    !12 = !{!"ejit_group_indexed", !"cell", i64 2}
    !5 = !{!6, !8, !9, !13}
    !6 = !{!"ejit_entry"}
    !8 = !{!"ejit_dim", !"cell", i32 0}
    !9 = !{!"ejit_bound_ptr", !"cell", i32 3, i64 8, !10}
    !10 = !{i64 4, i64 4}
    !13 = !{!"ejit_free_dim", !"", i32 1}
    !7 = !{}
  )",
                               Err, *C);
  ASSERT_TRUE(M);
  uint32_t Data[2][10] = {};
  uint32_t Data2[2][10] = {};
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "cfg", Data, 2);
  Registry.registerArray("cell", "cfg2", Data2, 2);
  uint64_t Object[1] = {0x1122334400000000ULL};
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Object),
                                /*size=*/8, /*argIndex=*/3, /*instance=*/1});
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  auto Level = ejit_get_log_level();
  ejit_set_log_level(EJIT_LOG_VERBOSE);
  testing::internal::CaptureStdout();
  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  const std::string Out = testing::internal::GetCapturedStdout();
  ejit_set_log_level(Level);

  ASSERT_FALSE(verifyModule(*M, &errs()));
  EXPECT_EQ(Capture.entryFolds.size(), 0u);
  EXPECT_EQ(Capture.driftUncovered, 5u);
  ASSERT_EQ(Capture.annotatedFields.size(), 1u);
  EXPECT_EQ(Capture.annotatedFields.front().object, "cfg");
  EXPECT_NE(Out.find("reason=volatile-annotated"), std::string::npos);
  EXPECT_NE(Out.find("reason=non-const-offset"), std::string::npos);
  EXPECT_NE(Out.find("reason=residue-driven-unfolded-bound"), std::string::npos);
  EXPECT_NE(Out.find("reason=offset-not-in-field-metadata"), std::string::npos);
  EXPECT_NE(Out.find("reason=invalid-bound-offset"), std::string::npos);
}

TEST(EJitAsan, EmitsMergedStubForFoldedAndDriftFolds) {
  // One entry, two annotated fields through the same bound argument: one
  // folds (plain load, baked constant), one survives as atomic (a drift
  // observation). The merged stub has ONE gate, ONE switch case for the
  // single bound argument, a comparison for the folded field, and an
  // unconditional observation call for the drift field.
  resetAsanBlob();
  auto C = std::make_unique<LLVMContext>();
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    declare void @observe(i32)
    define i32 @probe(i8 %cell, ptr %cfg) !ejit.metadata !0 {
    entry:
      %p = getelementptr inbounds i8, ptr %cfg, i64 4
      %v = load i32, ptr %p, !ejit.period_const !7
      %q = getelementptr inbounds i8, ptr %cfg, i64 0
      %w = load atomic i32, ptr %q acquire, align 4, !ejit.period_const !7
      %r = add i32 %v, %w
      ret i32 %r
    }
    !0 = !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_dim", !"cell", i32 0}
    !3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 8, !4, !5}
    !4 = !{i64 0, i64 4}
    !5 = !{i64 4, i64 4}
    !7 = !{}
  )",
                               Err, *C);
  ASSERT_TRUE(M);
  // Field at +0 = drift baseline, field at +4 = folded constant.
  uint64_t Object[1] = {0x1122334400000000ULL | 0xCAFEBABE};
  PeriodArrayRegistry Registry;
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Object),
                                /*size=*/8, /*argIndex=*/1, /*instance=*/1});
  Ctx.asanEntryChecks = true;
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  ASSERT_EQ(Capture.entryFolds.size(), 2u);
  const EJitAsanEntryFold *Folded = nullptr, *Drift = nullptr;
  for (const auto &EF : Capture.entryFolds) {
    if (EF.folded)
      Folded = &EF;
    else
      Drift = &EF;
  }
  ASSERT_NE(Folded, nullptr);
  ASSERT_NE(Drift, nullptr);
  EXPECT_EQ(Folded->byteOffset, 4u);
  EXPECT_EQ(Folded->expected, 0x11223344u);
  EXPECT_EQ(Drift->byteOffset, 0u);
  EXPECT_EQ(Drift->expected, 0xCAFEBABEu);
  EXPECT_TRUE(Drift->atomic);

  EXPECT_EQ(countCallsTo(*M, "__ejit_asan_gate"), 1u);
  EXPECT_EQ(countCallsTo(*M, "__ejit_asan_mismatch"), 2u);
  EXPECT_NE(Folded->siteId, kEJitAsanNoSite);
  EXPECT_NE(Drift->siteId, kEJitAsanNoSite);
  EXPECT_EQ(gEJitAsanState.sites[Folded->siteId].kind, kEJitAsanKindFolded);
  EXPECT_EQ(gEJitAsanState.sites[Drift->siteId].kind, kEJitAsanKindDrift);
  EXPECT_EQ(gEJitAsanState.slots[0].boundCount.loadRelaxed(), 1u);
  EXPECT_EQ(gEJitAsanState.slots[0].activeSites.loadRelaxed(), 2u);
  // Publish seeded the drift baseline so the first observation cannot
  // report a pseudo-change.
  EXPECT_EQ(gEJitAsanState.sticky[Drift->siteId].lastSeen.loadRelaxed(),
            0xCAFEBABEu);
  // One switch with one case (a single bound argument), keeping the
  // per-call cost to exactly one argument's checks.
  SwitchInst *Rot = nullptr;
  for (Instruction &I : instructions(M->getFunction("probe")))
    if (auto *Sw = dyn_cast<SwitchInst>(&I))
      Rot = Sw;
  ASSERT_NE(Rot, nullptr);
  EXPECT_EQ(Rot->getNumCases(), 1u);
  // The stub's drift observation load carries the source's atomicity
  // (Acquire); the folded compare load stays plain. Trace each hook's
  // value argument back through its zext to the emitted load.
  unsigned AtomicVals = 0, PlainVals = 0;
  for (Instruction &I : instructions(M->getFunction("probe")))
    if (auto *CI = dyn_cast<CallInst>(&I))
      if (CI->getCalledFunction() &&
          CI->getCalledFunction()->getName() == "__ejit_asan_mismatch") {
        Value *V = CI->getArgOperand(2);
        if (auto *Z = dyn_cast<ZExtInst>(V))
          V = Z->getOperand(0);
        auto *L = dyn_cast<LoadInst>(V);
        ASSERT_NE(L, nullptr);
        if (L->isAtomic()) {
          ++AtomicVals;
          EXPECT_EQ(L->getOrdering(), AtomicOrdering::Acquire);
        } else {
          ++PlainVals;
        }
      }
  EXPECT_EQ(AtomicVals, 1u);
  EXPECT_EQ(PlainVals, 1u);
}

TEST(EJitAsan, EmitsPerArgumentSubWindowsForMultipleBoundPointers) {
  // The sub-rotation's product rule at the IR level: a function with TWO
  // bound_ptr arguments gets one gate and one switch with two cases, and
  // case K's check chain references ONLY the K-th bound argument (rank =
  // ascending argument index) — sub 0 never touches the second pointer.
  resetAsanBlob();
  auto C = std::make_unique<LLVMContext>();
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    declare void @observe(i32)
    define i32 @probe(i8 %cell, ptr %a, ptr %b) !ejit.metadata !0 {
    entry:
      %pa = getelementptr inbounds i8, ptr %a, i64 0
      %va = load i32, ptr %pa, !ejit.period_const !7
      %pb = getelementptr inbounds i8, ptr %b, i64 4
      %vb = load i32, ptr %pb, !ejit.period_const !7
      %r = add i32 %va, %vb
      ret i32 %r
    }
    !0 = !{!1, !2, !3, !4}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_dim", !"cell", i32 0}
    !3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 8, !5}
    !4 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 8, !6}
    !5 = !{i64 0, i64 4}
    !6 = !{i64 4, i64 4}
    !7 = !{}
  )",
                               Err, *C);
  ASSERT_TRUE(M);
  uint32_t Bufs[2][2] = {{0xAABBCCDDu, 0}, {0, 0xEEFF0011u}};
  PeriodArrayRegistry Registry;
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Bufs[0]),
                                /*size=*/8, /*argIndex=*/1, /*instance=*/1});
  Ctx.boundPointers.push_back({reinterpret_cast<const uint8_t *>(Bufs[1]),
                                /*size=*/8, /*argIndex=*/2, /*instance=*/1});
  Ctx.asanEntryChecks = true;
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;

  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  ASSERT_EQ(Capture.entryFolds.size(), 2u);
  EXPECT_EQ(countCallsTo(*M, "__ejit_asan_gate"), 1u);
  EXPECT_EQ(countCallsTo(*M, "__ejit_asan_mismatch"), 2u);
  // Two distinct bound arguments published: two sub-windows.
  EXPECT_EQ(gEJitAsanState.slots[0].boundCount.loadRelaxed(), 2u);

  SwitchInst *Rot = nullptr;
  for (Instruction &I : instructions(M->getFunction("probe")))
    if (auto *Sw = dyn_cast<SwitchInst>(&I))
      Rot = Sw;
  ASSERT_NE(Rot, nullptr);
  ASSERT_EQ(Rot->getNumCases(), 2u);
  Function *F = M->getFunction("probe");
  // Walk each case's check chain (everything reachable before the switch
  // default): every argument it touches must be exactly the K-th bound
  // argument, and exactly one hook call lives in the chain.
  BasicBlock *Cont = Rot->getDefaultDest();
  for (unsigned Rank = 0; Rank < 2; ++Rank) {
    auto Case = Rot->findCaseValue(
        ConstantInt::get(Type::getInt32Ty(M->getContext()), Rank));
    ASSERT_NE(Case, Rot->case_default());
    std::set<const Argument *> Touched;
    std::set<BasicBlock *> Seen;
    SmallVector<BasicBlock *, 8> Work{Case->getCaseSuccessor()};
    unsigned Hooks = 0;
    while (!Work.empty()) {
      BasicBlock *Cur = Work.pop_back_val();
      if (!Seen.insert(Cur).second || Cur == Cont)
        continue;
      for (Instruction &I : *Cur) {
        if (auto *Call = dyn_cast<CallInst>(&I))
          if (Call->getCalledFunction() &&
              Call->getCalledFunction()->getName() == "__ejit_asan_mismatch")
            ++Hooks;
        for (Value *Op : I.operands())
          if (auto *A = dyn_cast<Argument>(Op->stripPointerCasts()))
            if (A->getParent() == F)
              Touched.insert(A);
      }
      Instruction *T = Cur->getTerminator();
      for (unsigned S = 0; S < T->getNumSuccessors(); ++S)
        Work.push_back(T->getSuccessor(S));
    }
    EXPECT_EQ(Hooks, 1u);
    ASSERT_EQ(Touched.size(), 1u);
    EXPECT_EQ((*Touched.begin())->getArgNo(), Rank == 0 ? 1u : 2u);
  }
}

TEST(EJitAsan, DriftStubEndToEndRecordsDivergentBuffer) {
  // The drift half of the stub, end to end: the observation call reports
  // the field value seen through the CURRENT bound argument on every
  // activated call, and the hook judges it against the baseline. The
  // unfolded load still feeds the body, so the call returns the REAL
  // value — unlike a folded mismatch, nothing is masked.
  resetAsanBlob();
  EJitCoreId::setCurrentForTest(0);
  auto C = std::make_unique<LLVMContext>();
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    declare void @observe(i32)
    define i32 @probe(i8 %cell, ptr %cfg) !ejit.metadata !0 {
    entry:
      %p = getelementptr inbounds i8, ptr %cfg, i64 4
      %v = load atomic i32, ptr %p acquire, align 4, !ejit.period_const !7
      ret i32 %v
    }
    !0 = !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_dim", !"cell", i32 0}
    !3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 8, !4}
    !4 = !{i64 4, i64 4}
    !7 = !{}
  )",
                               Err, *C);
  ASSERT_TRUE(M);
  uint64_t Good[1] = {uint64_t(0x11223344) << 32};
  uint64_t Bad[1] = {uint64_t(0x55aa55aa) << 32};

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
  ASSERT_EQ(gEJitAsanState.sites[Site].kind, kEJitAsanKindDrift);

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

  EJitSharedTaskPool Pool;
  EJitAsan Verifier;
  auto Level = ejit_get_log_level();
  auto DumpAt = [&](ejit_log_level_t Lvl) {
    ejit_set_log_level(Lvl);
    testing::internal::CaptureStdout();
    EXPECT_TRUE(Verifier.dumpNow(Pool));
    std::string S = testing::internal::GetCapturedStdout();
    ejit_set_log_level(Level);
    return S;
  };
  // Before any activation the drift site is never-changed: its row prints
  // at VERBOSE only, with the seeded baseline and no diverging member.
  EXPECT_EQ(DumpAt(EJIT_LOG_INFO).find("kind=drift"), std::string::npos);
  const std::string Pristine = DumpAt(EJIT_LOG_VERBOSE);
  EXPECT_NE(Pristine.find("kind=drift"), std::string::npos);
  EXPECT_NE(Pristine.find("changes=0 samples=0"), std::string::npos);

  // Divergent member observed on the first activated call.
  EXPECT_EQ(Probe(1, Bad), 0x55aa55aa);
  const EJitAsanSticky &R = gEJitAsanState.sticky[Site];
  EXPECT_EQ(R.recorded.loadRelaxed(), 2u);
  EXPECT_EQ(R.firstValue, 0x55aa55aau);
  EXPECT_EQ(R.firstArg, reinterpret_cast<uint64_t>(Bad));
  EXPECT_EQ(R.driftChanges.loadRelaxed(), 1u);
  EXPECT_EQ(R.samples.loadRelaxed(), 1u);
  EXPECT_EQ(R.lastSeen.loadRelaxed(), 0x55aa55aau);
  // Same value again: a sample, no new change.
  EXPECT_EQ(Probe(1, Bad), 0x55aa55aa);
  EXPECT_EQ(R.samples.loadRelaxed(), 2u);
  EXPECT_EQ(R.driftChanges.loadRelaxed(), 1u);
  // Back to the baseline member: a second transfer.
  EXPECT_EQ(Probe(1, Good), 0x11223344);
  EXPECT_EQ(R.driftChanges.loadRelaxed(), 2u);
  EXPECT_EQ(R.lastSeen.loadRelaxed(), 0x11223344u);

  const std::string Out = DumpAt(EJIT_LOG_INFO);
  EXPECT_NE(Out.find("kind=drift"), std::string::npos);
  EXPECT_NE(Out.find("object=bound@0x"), std::string::npos);
  EXPECT_NE(Out.find("initial=0x11223344 last=0x11223344"), std::string::npos);
  EXPECT_NE(Out.find("changes=2 samples=3"), std::string::npos);
  EXPECT_NE(Out.find("entry_drift_sites=1"), std::string::npos);
  EXPECT_NE(Out.find("entry_drift_checks=3"), std::string::npos);
  EXPECT_NE(Out.find("entry_folded_sites=0"), std::string::npos);
  // The forensic detail line is VERBOSE-only.
  EXPECT_EQ(Out.find("DRIFT-DETAIL"), std::string::npos);

  const std::string Verbose = DumpAt(EJIT_LOG_VERBOSE);
  EXPECT_NE(Verbose.find("DRIFT-DETAIL"), std::string::npos);
  EXPECT_NE(Verbose.find("first=0x55aa55aa"), std::string::npos);

  EJitCoreId::resetForTest();
}

TEST(EJitAsan, FullScopeProbeWatchesAnnotatedFields) {
  // The full-scope tier watches every registered instance of every
  // annotated field for the boot's lifetime, independent of versions: the
  // capture's folded watch covers the cell-1 instance (back-marked folded,
  // skipped by the drift scan), the cell-0 instance is sampled here and a
  // value transfer reports a DRIFT row.
  resetAsanBlob();
  uint32_t Data[2][10];
  for (unsigned I = 0; I < 10; ++I) {
    Data[0][I] = 7;
    Data[1][I] = 11;
  }
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "cfg", Data, 2);
  auto C = std::make_unique<LLVMContext>();
  auto M = makeProbe(*C, /*Local=*/false, /*Mod=*/5);
  ASSERT_TRUE(M);
  SpecializationContext Ctx;
  Ctx.fnName = "probe";
  Ctx.dimensions.push_back({"cell", 1});
  EJitAsanCapture Capture;
  Ctx.asanCapture = &Capture;
  EJitOptimizer(Registry).runPipeline(*M, Ctx);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  ASSERT_EQ(Capture.annotatedFields.size(), 1u);
  ASSERT_EQ(Capture.annotatedFields.front().base,
            reinterpret_cast<uintptr_t>(Data));

  EJitCompileRequest Req{};
  Req.funcIndex = 44;
  Req.generation = 1;
  Req.numDims = 1;
  Req.dims[0] = {0, 1};
  Req.versions[0] = 1;
  EJitSharedTaskPool Pool;
  EJitAsan Verifier;
  Verifier.onCompiled(Req, reinterpret_cast<uintptr_t>(&codeMarker),
                      std::move(Capture));

  // A recompile of the same object merges nothing new: the table is
  // deduplicated by instance address for the boot's lifetime.
  {
    auto C2 = std::make_unique<LLVMContext>();
    auto M2 = makeProbe(*C2, /*Local=*/false, /*Mod=*/5);
    ASSERT_TRUE(M2);
    SpecializationContext Ctx2;
    Ctx2.fnName = "probe";
    Ctx2.dimensions.push_back({"cell", 1});
    EJitAsanCapture Capture2;
    Ctx2.asanCapture = &Capture2;
    EJitOptimizer(Registry).runPipeline(*M2, Ctx2);
    ASSERT_FALSE(verifyModule(*M2, &errs()));
    ASSERT_EQ(Capture2.annotatedFields.size(), 1u);
    EJitCompileRequest Req2{};
    Req2.funcIndex = 44;
    Req2.generation = 2;
    Req2.numDims = 1;
    Req2.dims[0] = {0, 1};
    Req2.versions[0] = 2;
    Verifier.onCompiled(Req2, reinterpret_cast<uintptr_t>(&codeMarker),
                        std::move(Capture2));
  }

  auto Level = ejit_get_log_level();
  ejit_set_log_level(EJIT_LOG_INFO);
  auto Dump = [&] {
    testing::internal::CaptureStdout();
    EXPECT_TRUE(Verifier.dumpNow(Pool));
    return testing::internal::GetCapturedStdout();
  };
  // A slice runs the full step budget over the site table, so the one
  // un-folded site is sampled StepBudget/2 times per dump (the folded
  // sibling consumes the other half of the steps); the counts below scale
  // with the number of dumps run so far.
  const unsigned PerDump = EJIT_ASAN_STEP_BUDGET / 2;
  auto SamplesAfter = [&](unsigned Dumps) {
    return "fullscope_samples=" + std::to_string(PerDump * Dumps);
  };
  // Baseline scan: both instances merged, the folded one skipped, nothing
  // changed yet.
  auto First = Dump();
  EXPECT_EQ(First.find("src=metadata"), std::string::npos);
  EXPECT_NE(First.find("fullscope_sites=2/4096"), std::string::npos);
  EXPECT_NE(First.find("fullscope_folded=1"), std::string::npos);
  EXPECT_NE(First.find("fullscope_changed=0"), std::string::npos);
  EXPECT_NE(First.find(SamplesAfter(1)), std::string::npos);
  EXPECT_NE(First.find("drift_uncovered=0"), std::string::npos);
  // Never-changed site: the full row prints at VERBOSE only.
  ejit_set_log_level(EJIT_LOG_VERBOSE);
  testing::internal::CaptureStdout();
  EXPECT_TRUE(Verifier.dumpNow(Pool));
  const std::string V0 = testing::internal::GetCapturedStdout();
  ejit_set_log_level(Level);
  EXPECT_NE(V0.find("instance=0/2 src=metadata"), std::string::npos);
  EXPECT_NE(V0.find("changes=0 samples=" + std::to_string(2 * PerDump)),
            std::string::npos);
  // A value transfer on the un-folded instance reports a DRIFT row with
  // the registered-object identity; the folded instance never does.
  __atomic_store_n(&Data[0][0], 9u, __ATOMIC_RELEASE);
  auto Drifted = Dump();
  EXPECT_NE(Drifted.find("instance=0/2 src=metadata"), std::string::npos);
  EXPECT_NE(Drifted.find("kind=drift"), std::string::npos);
  EXPECT_NE(Drifted.find("initial=0x7 last=0x9"), std::string::npos);
  EXPECT_NE(Drifted.find("changes=1 samples=" + std::to_string(3 * PerDump)),
            std::string::npos);
  EXPECT_NE(Drifted.find("fullscope_changed=1"), std::string::npos);
  EXPECT_NE(Drifted.find(SamplesAfter(3)), std::string::npos);
  EXPECT_EQ(Drifted.find("instance=1/2"), std::string::npos);

  ejit_set_log_level(EJIT_LOG_VERBOSE);
  testing::internal::CaptureStdout();
  EXPECT_TRUE(Verifier.dumpNow(Pool));
  const std::string Verbose = testing::internal::GetCapturedStdout();
  ejit_set_log_level(Level);
  EXPECT_NE(Verbose.find("DRIFT-DETAIL"), std::string::npos);
  EXPECT_NE(Verbose.find("first=0x9"), std::string::npos);
}

TEST(EJitAsan, RotationStatusPrintsPerCoreState) {
  resetAsanBlob();
  const uint32_t Slot = ejitAsanClaimSlot("probe");
  ejitAsanSetSlotBound(Slot, 2);
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(__ejit_asan_gate(Slot), 0);
  EXPECT_EQ(__ejit_asan_gate(Slot), 0);
  EJitCoreId::resetForTest();

  auto Level = ejit_get_log_level();
  ejit_set_log_level(EJIT_LOG_INFO);
  testing::internal::CaptureStdout();
  EXPECT_EQ(ejit_asan_rotation_status(), EJIT_OK);
  const std::string Out = testing::internal::GetCapturedStdout();
  ejit_set_log_level(Level);

  EXPECT_NE(Out.find("[EJIT-ASAN] rotation slots=1/"), std::string::npos);
  EXPECT_NE(Out.find("drift_sites=0"), std::string::npos);
  EXPECT_NE(Out.find("rotation core=0"), std::string::npos);
  EXPECT_NE(Out.find("slot=0(probe)"), std::string::npos);
  EXPECT_NE(Out.find("sub=0"), std::string::npos);
  EXPECT_NE(Out.find("bound=2"), std::string::npos);
  EXPECT_NE(Out.find("window="), std::string::npos);
  EXPECT_NE(Out.find("checks=2"), std::string::npos);
  // Per-slot lines are audit depth: absent at INFO.
  EXPECT_EQ(Out.find("rotation slot="), std::string::npos);

  ejit_set_log_level(EJIT_LOG_VERBOSE);
  testing::internal::CaptureStdout();
  EXPECT_EQ(ejit_asan_rotation_status(), EJIT_OK);
  const std::string Verbose = testing::internal::GetCapturedStdout();
  ejit_set_log_level(Level);
  EXPECT_NE(Verbose.find("[EJIT-ASAN] rotation slot=0(probe) bound=2 sites=0"),
            std::string::npos);
}

TEST(EJitAsan, InitAsanForcesEntryMode) {
  // The use gate, observable through the public surface: this variant's
  // runtime registers the pool's ASAN callbacks, so the synchronous dump is
  // served on the owner core. Plain ejit_init leaves the feature compiled-in
  // but inert, and an already-initialized runtime ignores a later
  // ejit_init_asan (the mode is fixed at the first init).
  ASSERT_EQ(ejit_init_asan(nullptr), EJIT_OK);
  EXPECT_EQ(ejit_asan_dump(), EJIT_OK);
  ejit_shutdown();
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK);
  EXPECT_EQ(ejit_asan_dump(), EJIT_ERR_NOT_ACTIVE);
  EXPECT_EQ(ejit_init_asan(nullptr), EJIT_OK);
  EXPECT_EQ(ejit_asan_dump(), EJIT_ERR_NOT_ACTIVE);
  ejit_shutdown();
}
