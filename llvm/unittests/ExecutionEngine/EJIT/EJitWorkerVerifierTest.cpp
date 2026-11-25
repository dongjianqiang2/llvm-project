//===-- EJitWorkerVerifierTest.cpp - Worker-only substitution checks ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitWorkerVerifier.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

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
      %v = load i32, ptr %p, align 4, !ejit.may_const !7
      ret i32 %v
    }
    attributes #0 = { nounwind willreturn memory(none) nocallback nosync "ejit.local_free_dim"="v1" }
    !0 = !{!1, !2}
    !1 = !{!"ejit_period_arr", !"cell", i64 2}
    !2 = !{!"ejit_may_const_field", i64 0}
    !3 = distinct !{!4, !5, !6}
    !4 = !{!"ejit_entry"}
    !5 = !{!"ejit_period_arr_ind", !"cell", i32 0}
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

TEST(EJitWorkerVerifier, CapturesEveryResidueWithoutChangingIR) {
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
      EJitWorkerVerifyCapture Capture;
      Capture.period = "cell";
      Ctx.workerVerify = &Capture;
      EJitOptimizer Observed(Registry);
      Observed.runPipeline(*With, Ctx);
      EXPECT_EQ(printModule(*With), printModule(*Without));
      EXPECT_EQ(Capture.uncovered, 0u);
      ASSERT_EQ(Capture.watches.size(), 1u);
      const auto &W = Capture.watches.front();
      EXPECT_EQ(W.expected, 11u);
      ASSERT_EQ(W.addresses.size(), Mod);
      for (unsigned R = 0; R < Mod; ++R)
        EXPECT_EQ(W.addresses[R], reinterpret_cast<uintptr_t>(&Data[1][R]));
    }
}

TEST(EJitWorkerVerifier, ReportsTailResidueOnlyAfterPublication) {
  auto State = std::make_unique<EJitSharedTaskPoolState>();
  EJitSharedTaskPool Pool;
  Pool.bind(State.get());
  Pool.setMode(EJitCompileMode::Async);
  EJitWorkerVerifier Verifier;
  uint32_t Data[5] = {7, 7, 7, 7, 7};
  uintptr_t Root = reinterpret_cast<uintptr_t>(Data);
  struct Context {
    EJitWorkerVerifier &verifier;
    uint32_t (&data)[5];
    uintptr_t &root;
  } Ctx{Verifier, Data, Root};
  Pool.setCompiler(
      [](void *Ptr, const EJitCompileRequest &Req, void **Out) {
        auto &C = *static_cast<Context *>(Ptr);
        EJitWorkerVerifyCapture Capture;
        Capture.period = "cell";
        auto &W = Capture.watches.emplace_back();
        W.function = "probe";
        W.object = "cfg";
        W.width = 32;
        W.expected = 7;
        uint32_t Endian = 1;
        W.littleEndian = *reinterpret_cast<uint8_t *>(&Endian) != 0;
        for (auto &Value : C.data)
          W.addresses.push_back(reinterpret_cast<uintptr_t>(&Value));
        EJitWorkerVerifyWatch Leaf;
        Leaf.function = "probe";
        Leaf.object = "pointer_cfg";
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
    Verifier.idle(Pool, true);
    return testing::internal::GetCapturedStdout();
  };
  EXPECT_NE(Dump().find("checks=0"), std::string::npos);
  ASSERT_TRUE(Pool.pollOne());
  EXPECT_NE(Dump().find("checked_active=6/6"), std::string::npos);
  __atomic_store_n(&Data[4], 9u, __ATOMIC_RELEASE);
  auto Mismatch = Dump();
  EXPECT_NE(Mismatch.find("residue=4/5 kind=value"), std::string::npos);
  EXPECT_NE(Mismatch.find("expected=0x7 actual=0x9"), std::string::npos);
  EXPECT_EQ(Dump().find("MISMATCH"), std::string::npos);
  __atomic_store_n(&Root, uintptr_t(1), __ATOMIC_RELEASE);
  auto ChangedRoot = Dump();
  EXPECT_NE(ChangedRoot.find("object=pointer_cfg"), std::string::npos);
  EXPECT_NE(ChangedRoot.find("kind=pointer-root"), std::string::npos);
  EXPECT_NE(ChangedRoot.find("actual=0x1"), std::string::npos);
  EXPECT_EQ(ChangedRoot.find("blocked_root=0"), std::string::npos);
  EXPECT_TRUE(Pool.setInstanceEnabled(0, 1, false));
  EXPECT_NE(Dump().find("versions=0 staged=0 checked_active=0/0"),
            std::string::npos);
  ejit_set_log_level(Level);
  Pool.ownerShutdown();
}
