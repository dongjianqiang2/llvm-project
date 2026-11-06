//===-- EJitVerifyPipelineTest.cpp - verifier pipeline coverage -----------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitVerify.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"
#include <sstream>
#include <chrono>
#include <thread>

using namespace llvm;
using namespace llvm::ejit;

#ifdef EJIT_VERIFY_SUBSTITUTION
namespace {
struct VerifyFixture {
  struct Record { uint64_t Field, Other; };
  std::vector<Record> Data, Alternate;
  void *Pointer;
  unsigned Slots, Cell;
  VerifyFixture(unsigned Slots = 5, unsigned Cell = 2)
      : Data(16 * Slots, {7, 0}), Alternate(16 * Slots, {11, 0}),
        Pointer(Data.data()), Slots(Slots), Cell(Cell) {}

  std::unique_ptr<Module> module(LLVMContext &Ctx, bool Bytes = false,
                                bool Absolute = false, bool BigEndian = false,
                                bool FreeDim = true, bool Local = false) {
    std::ostringstream S;
    S << "target datalayout = \"" << (BigEndian ? 'E' : 'e')
      << "-p:64:64-i64:64\"\n%Record = type {i64, i64}\n"
         "@cfg = external global ptr, !ejit.metadata !0\n"
         "declare i16 @__ejit_local_free_dim_v1_i16(i16) #0\n"
         "define i64 @probe(i8 %cell, i16 %slot, i1 %take) !ejit.metadata !3 {\n"
         "  br i1 %take, label %read, label %skip\nread:\n";
    if (Local)
      S << "  %local = call i16 @__ejit_local_free_dim_v1_i16(i16 %slot)\n";
    S << "  %phase = urem i16 " << (Local ? "%local" : "%slot")
      << ", " << Slots << "\n"
         "  %idx = zext i16 %phase to i64\n"
         "  %cell64 = zext i8 %cell to i64\n"
         "  %outer = mul i64 %cell64, " << Slots << "\n"
         "  %fused = add i64 %outer, %idx\n";
    if (!Absolute)
      S << "  %root = load ptr, ptr @cfg\n";
    std::string Root = Absolute
        ? "inttoptr (i64 " + std::to_string(reinterpret_cast<uintptr_t>(Pointer)) + " to ptr)"
        : "%root";
    if (Bytes)
      S << "  %off = mul i64 %idx, 16\n"
           "  %fixed = mul i64 %outer, 16\n"
           "  %sum = add i64 %fixed, %off\n"
           "  %p = getelementptr i8, ptr " << Root << ", i64 %sum\n";
    else
      S << "  %p = getelementptr %Record, ptr " << Root << ", i64 %fused, i32 0\n";
    S << "  %v = load i64, ptr %p, align 8, !ejit.may_const !7\n"
         "  ret i64 %v\nskip:\n  ret i64 99\n}\n"
         "attributes #0 = { nounwind readnone willreturn \"ejit.local_free_dim\"=\"v1\" }\n"
         "!0 = !{!1, !2}\n"
         "!1 = !{!\"ejit_period_arr\", !\"cell\", i32 0}\n"
         "!2 = !{!\"ejit_pointer_layout\", i64 16, !8}\n"
         "!8 = !{i64 0, i64 8}\n"
         "!3 = !{!4, !5" << (FreeDim && !Local ? ", !6" : "") << "}\n"
         "!4 = !{!\"ejit_entry\"}\n"
         "!5 = !{!\"ejit_period_arr_ind\", !\"cell\", i32 0}\n"
         "!6 = !{!\"ejit_free_dim\", !\"\", i32 1}\n!7 = !{}\n";
    SMDiagnostic Err;
    auto M = parseAssemblyString(S.str(), Err, Ctx);
    if (!M)
      ADD_FAILURE() << Err.getMessage().str();
    return M;
  }
  void registerRoot(PeriodArrayRegistry &Registry) {
    Registry.registerArray("cell", "cfg", &Pointer, 0);
  }
  SpecializationContext context(OptimizationLevel Level) {
    SpecializationContext C;
    C.fnName = "probe";
    C.cacheKey = 1;
    C.dimensions.push_back({"cell", static_cast<uint8_t>(Cell)});
    C.optLevel = Level;
    return C;
  }
};

unsigned checksIn(Module &M) {
  unsigned N = 0;
  for (Instruction &I : instructions(M.getFunction("probe")))
    if (auto *Call = dyn_cast<CallBase>(&I))
      if (auto *Callee = Call->getCalledFunction())
        N += Callee->getName() == "__ejit_verify_check";
  return N;
}

TEST(EJitVerifyPipeline, FusedWitnessKeepsOneLiveCheckAcrossPipelineRounds) {
  for (unsigned Slots : {5U, 10U})
    for (unsigned Cell : {0U, 2U, 15U})
      for (bool Bytes : {false, true})
        for (bool Absolute : {false, true})
          for (bool BigEndian : {false, true})
            for (auto Level : {OptimizationLevel::L2, OptimizationLevel::L3}) {
              // A host-native pointer slot is not a big-endian pointer image.
              // Cross-endian checks use absolute roots; native runs cover live
              // pointer slots. Actual BE pointer codegen is checked separately.
              if (BigEndian && !Absolute)
                continue;
              SCOPED_TRACE(::testing::Message() << Slots << '/' << Cell << '/'
                  << Bytes << '/' << Absolute << '/' << BigEndian << '/' << unsigned(Level));
              VerifyFixture F(Slots, Cell);
              if (BigEndian)
                for (auto &Record : F.Data) {
                  Record.Field = 0;
                  reinterpret_cast<uint8_t *>(&Record.Field)[7] = 7;
                }
              LLVMContext Ctx;
              auto M = F.module(Ctx, Bytes, Absolute, BigEndian);
              ASSERT_NE(M, nullptr);
              PeriodArrayRegistry Registry;
              F.registerRoot(Registry);
              auto C = F.context(Level);
              ejitVerifyResetStats();
              EJitOptimizer Opt(Registry, nullptr, true);
              Opt.runPipeline(*M, C);
              ASSERT_FALSE(verifyModule(*M, &errs()));
              EXPECT_EQ(checksIn(*M), 1U);
              for (Instruction &I : instructions(M->getFunction("probe")))
                if (auto *Call = dyn_cast<CallBase>(&I))
                  if (auto *Callee = Call->getCalledFunction())
                    if (Callee->getName() == "__ejit_verify_check") {
                      auto *Baked = dyn_cast<ConstantInt>(Call->getArgOperand(3));
                      ASSERT_NE(Baked, nullptr);
                      EXPECT_EQ(Baked->getZExtValue(), 7U);
                    }
              EXPECT_FALSE(M->getFunction("probe")->getArg(1)->use_empty());
              VerifyStats Stats{};
              ejitVerifyGetStats(&Stats);
              EXPECT_EQ(Stats.sites, 1U);
              EXPECT_EQ(Stats.checks, 0U); // emission is not runtime coverage
              EXPECT_EQ(ejitVerifySiteCount(), 0U);
            }
}

TEST(EJitVerifyPipeline, OrdinaryModeStillFoldsAndUnannotatedIndexIsNotChecked) {
  for (bool Verify : {false, true}) {
    VerifyFixture F;
    LLVMContext Ctx;
    auto M = F.module(Ctx, false, false, false, !Verify);
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    F.registerRoot(Registry);
    auto C = F.context(OptimizationLevel::L2);
    ejitVerifyResetStats();
    EJitOptimizer Opt(Registry, nullptr, Verify);
    Opt.runPipeline(*M, C);
    ASSERT_FALSE(verifyModule(*M, &errs()));
    EXPECT_EQ(checksIn(*M), 0U);
    EXPECT_EQ(M->getFunction("probe")->getArg(1)->use_empty(), !Verify);
    VerifyStats Stats{};
    ejitVerifyGetStats(&Stats);
    EXPECT_EQ(Stats.sites, 0U);
    EXPECT_EQ(Stats.checks, 0U);
  }
}

static void runNativeVerifier(bool Pgo, bool Local) {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  for (auto Level : {OptimizationLevel::L2, OptimizationLevel::L3}) {
    SCOPED_TRACE(unsigned(Level));
    VerifyFixture F;
    LLVMContext Ctx;
    auto M = F.module(Ctx, false, false, false, true, Local);
    ASSERT_NE(M, nullptr);
    auto JTMB = orc::JITTargetMachineBuilder::detectHost();
    ASSERT_TRUE(bool(JTMB));
    auto Layout = JTMB->getDefaultDataLayoutForTarget();
    ASSERT_TRUE(bool(Layout));
    M->setDataLayout(*Layout);
    std::string Bitcode;
    raw_string_ostream OS(Bitcode);
    WriteBitcodeToFile(*M, OS);
    OS.flush();
    EJitRuntimeState State;
    auto &Registry = State.getRegistry();
    F.registerRoot(Registry);
    Config Cfg;
    Cfg.optLevel = Level;
    Cfg.verifySubstitution = true;
    auto MaybeEngine = EJitOrcEngine::Create(Cfg, Registry, State);
    ASSERT_TRUE(bool(MaybeEngine));
    auto Engine = std::move(*MaybeEngine);
    auto C = F.context(Level);
    C.tier = Pgo ? CompileTier::Instrumented : CompileTier::Baseline;
    Engine->setActiveContext(&C);
    ejitVerifyResetStats();
    if (auto Err = Engine->loadBitcodeModule(Bitcode, 1, "probe"))
      FAIL() << toString(std::move(Err));
    auto Address = Engine->lookup(1, "probe");
    ASSERT_TRUE(bool(Address));
#ifdef EJIT_SRE_CODE_POOL
    if (auto Err = Engine->flushPendingCode())
      FAIL() << toString(std::move(Err));
#endif
    using Probe = uint64_t (*)(uint8_t, uint16_t, bool);
    auto Fn = reinterpret_cast<Probe>(*Address);
    const unsigned Samples = Pgo ? 64 : 5;
    for (unsigned I = 0; I < Samples; ++I)
      EXPECT_EQ(Fn(F.Cell, I, true), 7U);
    if (Pgo) {
      auto Profc = Engine->lookup(1, "__profc_probe");
      auto Profd = Engine->lookup(1, "__profd_probe");
      ASSERT_TRUE(bool(Profc));
      ASSERT_TRUE(bool(Profd));
      PgoCounterRef Ref{"probe", reinterpret_cast<uintptr_t>(*Profc),
                       reinterpret_cast<uintptr_t>(*Profd)};
      C.profileData = synthesizeProfileBuffer({Ref});
      ASSERT_FALSE(C.profileData.empty());
      C.tier = CompileTier::PGOUse;
      if (auto Err = Engine->loadBitcodeModule(Bitcode, 1, "probe"))
        FAIL() << toString(std::move(Err));
      auto T2 = Engine->lookup(1, "probe");
      ASSERT_TRUE(bool(T2));
#ifdef EJIT_SRE_CODE_POOL
      if (auto Err = Engine->flushPendingCode())
        FAIL() << toString(std::move(Err));
#endif
      Fn = reinterpret_cast<Probe>(*T2);
    }
    VerifyStats Stats{};
    ejitVerifyGetStats(&Stats);
    ASSERT_EQ(Stats.checks, Samples);
    ASSERT_EQ(Stats.mismatches, 0U);
    // Change only a non-witness slot. A check reading slot zero would miss it.
    F.Data[F.Cell * F.Slots + 4].Field = 9;
    EXPECT_EQ(Fn(F.Cell, 4, true), 9U);
    // Keep the runtime root live too; comparing through a frozen root is wrong.
    F.Pointer = F.Alternate.data();
    EXPECT_EQ(Fn(F.Cell, 2, true), 11U);
    EXPECT_EQ(Fn(F.Cell, 4, false), 99U);
    ejitVerifyGetStats(&Stats);
    EXPECT_EQ(Stats.checks, Samples + 2);
    EXPECT_EQ(Stats.mismatches, 2U);
    EXPECT_GT(ejitVerifySiteCount(), 0U);
    // Warm code after reset still checks, even though no new sites are emitted.
    ejitVerifyResetStats();
    EXPECT_EQ(Fn(F.Cell, 2, true), 11U);
    ejitVerifyGetStats(&Stats);
    EXPECT_EQ(Stats.sites, 0U);
    EXPECT_EQ(Stats.checks, 1U);
    EXPECT_EQ(Stats.mismatches, 1U);
    Engine->setActiveContext(nullptr);
  }
}

TEST(EJitVerifyPipeline, ExecutesFusedWitnessAndDetectsSlotAndRootChanges) {
  runNativeVerifier(false, false);
}
TEST(EJitVerifyPipeline, ExecutesLocalFreeDimWitness) {
  runNativeVerifier(false, true);
}
TEST(EJitVerifyPipeline, ExecutesRealTier1ProfileThenTier2Checks) {
  runNativeVerifier(true, false);
}

struct RuntimeGuard {
  static void reset() {
    ejit_shutdown();
    EJitLifecycleRegistry::instance().reset();
    EJitFuncRegistry::instance().reset();
    EJitRegistrationStore::instance().consume();
    EJitRegistrationStore::instance().consumeError();
  }
  RuntimeGuard() { reset(); }
  ~RuntimeGuard() { reset(); }
};

TEST(EJitVerifyPipeline, InitCannotSilentlyKeepAnIncompatibleMode) {
  RuntimeGuard Guard;
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK);
  EXPECT_EQ(ejit_init_verify(nullptr), EJIT_ERR_INVALID_PARAM);
  RuntimeGuard::reset();
  ASSERT_EQ(ejit_init_verify(nullptr), EJIT_OK);
  EXPECT_EQ(ejit_init_verify(nullptr), EJIT_OK);
  EXPECT_EQ(ejit_init_verify_pgo(nullptr), EJIT_ERR_INVALID_PARAM);
  EXPECT_EQ(ejit_init(nullptr), EJIT_ERR_INVALID_PARAM);
  RuntimeGuard::reset();
  ASSERT_EQ(ejit_init_verify_pgo(nullptr), EJIT_OK);
  EXPECT_EQ(ejit_init_verify_pgo(nullptr), EJIT_OK);
  EXPECT_EQ(ejit_init_verify(nullptr), EJIT_ERR_INVALID_PARAM);
}

TEST(EJitVerifyPipeline, CApiOnlinePgoPublishesVerifierT2After64RealCalls) {
#ifndef EJIT_SRE_SHARED_TASKPOOL
  GTEST_SKIP() << "requires the shared taskpool runtime";
#else
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  VerifyFixture F;
  LLVMContext Ctx;
  auto M = F.module(Ctx);
  ASSERT_NE(M, nullptr);
  auto JTMB = orc::JITTargetMachineBuilder::detectHost();
  ASSERT_TRUE(bool(JTMB));
  auto Layout = JTMB->getDefaultDataLayoutForTarget();
  ASSERT_TRUE(bool(Layout));
  M->setDataLayout(*Layout);
  std::string Bitcode;
  raw_string_ostream OS(Bitcode);
  WriteBitcodeToFile(*M, OS);
  OS.flush();
  RuntimeGuard Guard;
  uint32_t DimID = kEJitInvalidDimType, Func = 0;
  ejit_register_lifecycle("cell", &DimID);
  ejit_register_funcindex("probe", &Func);
  auto &Store = EJitRegistrationStore::instance();
  Store.registerBitcode("probe", reinterpret_cast<const uint8_t *>(Bitcode.data()),
                         Bitcode.size());
  Store.registerPeriodArray("cell", "cfg", &F.Pointer, 0);
  ASSERT_EQ(ejit_init_verify_pgo(nullptr), EJIT_OK);
  ejit_verify_reset_stats();
  ASSERT_EQ(ejit_activate("cell", F.Cell), EJIT_OK);
  const ejit_dim_pair_t Dim{DimID, static_cast<uint8_t>(F.Cell)};
  using Probe = uint64_t (*)(uint8_t, uint16_t, bool);
  void *T1 = nullptr, *T2 = nullptr;
  unsigned Samples = 0;
  const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!T2 && std::chrono::steady_clock::now() < Deadline) {
    void *Fn = nullptr;
    uint32_t Bucket = 0;
    auto Status = ejit_taskpool_compile_or_get(Func, &Dim, 1, &Fn, &Bucket);
    if (Status == EJIT_OK && Fn) {
      if (!T1)
        T1 = Fn;
      const uint64_t Actual = reinterpret_cast<Probe>(Fn)(F.Cell, Samples % 5, true);
      ejit_taskpool_release_read(Bucket);
      EXPECT_EQ(Actual, 7U);
      if (Fn == T1)
        ++Samples;
      else
        T2 = Fn;
    } else {
      ASSERT_EQ(Status, EJIT_PENDING);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_NE(T2, nullptr);
  EXPECT_EQ(Samples, 64U);
  ejit_verify_stats_t Stats{};
  ejit_verify_get_stats(&Stats);
  EXPECT_EQ(Stats.checks, 65U);
  EXPECT_EQ(Stats.mismatches, 0U);
  F.Data[F.Cell * F.Slots + 4].Field = 9;
  void *Fn = nullptr;
  uint32_t Bucket = 0;
  ASSERT_EQ(ejit_taskpool_compile_or_get(Func, &Dim, 1, &Fn, &Bucket), EJIT_OK);
  ASSERT_EQ(Fn, T2);
  const uint64_t Actual = reinterpret_cast<Probe>(Fn)(F.Cell, 4, true);
  ejit_taskpool_release_read(Bucket);
  EXPECT_EQ(Actual, 9U);
  ejit_verify_get_stats(&Stats);
  EXPECT_EQ(Stats.checks, 66U);
  EXPECT_EQ(Stats.mismatches, 1U);
  ASSERT_EQ(ejit_deactivate("cell", F.Cell), EJIT_OK);
#endif
}
} // namespace
#endif
