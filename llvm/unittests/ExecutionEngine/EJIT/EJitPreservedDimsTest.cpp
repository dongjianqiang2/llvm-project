//===-- EJitPreservedDimsTest.cpp - Load-only specialization tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitPreservedScalar.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Error.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace {

static const char *Metadata = R"(
!0 = !{!1, !2}
!1 = !{!"ejit_period_arr", !"cell", i64 6}
!2 = !{!"ejit_may_const_field", i64 0}
!3 = !{!4, !5, !6}
!4 = !{!"ejit_entry"}
!5 = !{!"ejit_period_arr_ind", !"cell", i32 0}
!6 = !{!"ejit_period_arr_ind", !"trp", i32 1}
!7 = !{}
)";

static const char *DynamicBody = R"(
  %gp = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 0
  %gain = load i32, ptr %gp, !ejit.may_const !7
  %lp = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 1, i64 %trp
  %live = load i32, ptr %lp
  %product = mul i32 %x, %gain
  %sum = add i32 %product, %live
  store i32 %sum, ptr %lp
  %ci = trunc i64 %cell to i32
  %result = add i32 %sum, %ci
  ret i32 %result
)";

static SpecializationContext context(uint8_t Cell = 2) {
  SpecializationContext C;
  C.fnName = "f";
  C.dimensions.push_back({"cell", Cell});
  C.dimensions.push_back({"trp", 1});
  return C;
}

static std::unique_ptr<Module> parseIR(LLVMContext &C, StringRef IR) {
  SMDiagnostic Error;
  auto M = parseAssemblyString(IR, Error, C);
  if (!M)
    Error.print("EJitPreservedDimsTest", errs());
  return M;
}

static std::unique_ptr<Module> parse(LLVMContext &C, StringRef Body,
                                     StringRef Extra = "") {
  return parseIR(C, std::string(R"(
    target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
    %Cfg = type { i32, [2 x i32] }
    @cfg = external global [6 x %Cfg], !ejit.metadata !0
  )") + Extra.str() + R"(
    define i32 @f(i64 %cell, i64 %trp, i32 %x) !ejit.metadata !3 {
  )" + Body.str() + "\n}\n" + Metadata);
}

static unsigned markedLoads(const Module &M) {
  unsigned Count = 0;
  for (const Function &F : M)
    for (const Instruction &I : instructions(F))
      if (const auto *LI = dyn_cast<LoadInst>(&I))
        Count += LI->hasMetadata(MD_EJIT_MAY_CONST);
  return Count;
}

static unsigned loads(const Function &F) {
  unsigned Count = 0;
  for (const Instruction &I : instructions(F))
    Count += isa<LoadInst>(I);
  return Count;
}

static void replace(Module &M, PeriodArrayRegistry &R,
                    const SpecializationContext &C) {
  EJitStructFieldPass P(R, C.boundPointers, C.fnName);
  P.setPreservedDimensions(C);
  P.initFromModule(M);
  FunctionAnalysisManager AM;
  for (Function &F : M)
    if (!F.isDeclaration())
      P.run(F, AM);
  EXPECT_FALSE(verifyModule(M, &errs()));
}

class PreservedDimsTest : public testing::Test {
protected:
  LLVMContext C;
  PeriodArrayRegistry R;
  // Target bytes, not host-endian integers.
  uint8_t Data[6 * 12] = {};
  void SetUp() override {
    for (unsigned I = 0; I != 6; ++I)
      Data[I * 12] = 7;
    R.registerArray("cell", "cfg", Data, 6);
  }
};

TEST_F(PreservedDimsTest, LoadOnlyKeepsDynamicAddressesAndCallArguments) {
  std::string Body = DynamicBody;
  Body.insert(Body.find("  ret i32 %result"),
              "  call void @observe(i64 %cell, i64 %trp, ptr %lp)\n");
  auto M = parse(C, Body, "declare void @observe(i64, i64, ptr)\n");
  ASSERT_TRUE(M);
  Function *F = M->getFunction("f");
  StoreInst *Store = nullptr;
  CallBase *Call = nullptr;
  for (Instruction &I : instructions(F)) {
    if (auto *S = dyn_cast<StoreInst>(&I))
      Store = S;
    if (auto *CB = dyn_cast<CallBase>(&I))
      Call = CB;
  }
  ASSERT_NE(Store, nullptr);
  ASSERT_NE(Call, nullptr);
  Value *Address = Store->getPointerOperand();
  replace(*M, R, context());
  EXPECT_EQ(markedLoads(*M), 0u);
  EXPECT_EQ(loads(*F), 1u);
  EXPECT_EQ(Store->getPointerOperand(), Address);
  EXPECT_EQ(Call->getArgOperand(0), F->getArg(0));
  EXPECT_EQ(Call->getArgOperand(1), F->getArg(1));
  EXPECT_EQ(Call->getArgOperand(2), Address);
}

TEST_F(PreservedDimsTest, SixCellsTwentyEntriesAndSingleFieldDifference) {
  Data[5 * 12] = 9;
  for (unsigned Entry = 0; Entry != 20; ++Entry)
    for (uint8_t Cell = 0; Cell != 6; ++Cell) {
      auto M = parse(C, R"(
        %p = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 0
        %v = load i32, ptr %p, !ejit.may_const !7
        ret i32 %v
      )");
      ASSERT_TRUE(M);
      std::string Name = "entry" + std::to_string(Entry);
      M->getFunction("f")->setName(Name);
      auto Spec = context(Cell);
      Spec.fnName = Name;
      replace(*M, R, Spec);
      auto *Ret = cast<ReturnInst>(M->getFunction(Name)->back().getTerminator());
      auto *V = dyn_cast<ConstantInt>(Ret->getReturnValue());
      ASSERT_NE(V, nullptr);
      EXPECT_EQ(V->getZExtValue(), Cell == 5 ? 9u : 7u);
    }
}

TEST_F(PreservedDimsTest, MissingDimensionAndOtherCellAreNotFolded) {
  auto M = parse(C, DynamicBody);
  ASSERT_TRUE(M);
  auto Spec = context();
  Spec.dimensions.clear();
  replace(*M, R, Spec);
  EXPECT_EQ(markedLoads(*M), 1u);
  auto Other = parse(C, R"(
    %next = add i64 %cell, 1
    %p = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %next, i32 0
    %v = load i32, ptr %p, !ejit.may_const !7
    ret i32 %v
  )");
  ASSERT_TRUE(Other);
  replace(*Other, R, context());
  EXPECT_EQ(markedLoads(*Other), 1u);
}

TEST_F(PreservedDimsTest, AtomicVolatileUnknownAndPoisonRemainLoads) {
  const char *Bodies[] = {
      R"(%p = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 0
         %v = load volatile i32, ptr %p, !ejit.may_const !7
         ret i32 %v)",
      R"(%p = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 0
         %v = load atomic i32, ptr %p monotonic, align 4, !ejit.may_const !7
         ret i32 %v)",
      R"(%b = icmp eq i32 %x, 0
         %i = select i1 %b, i64 %cell, i64 5
         %p = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %i, i32 0
         %v = load i32, ptr %p, !ejit.may_const !7
         ret i32 %v)",
      R"(%i = add nuw i64 %cell, -1
         %p = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %i, i32 0
         %v = load i32, ptr %p, !ejit.may_const !7
         ret i32 %v)"};
  for (const char *Body : Bodies) {
    auto M = parse(C, Body);
    ASSERT_TRUE(M);
    replace(*M, R, context());
    EXPECT_EQ(markedLoads(*M), 1u);
  }
}

TEST_F(PreservedDimsTest, PointerBaseAndUnboundedPointeeStayDynamic) {
  auto M = parse(C, R"(
    %base = load ptr, ptr @pointer
    %p = getelementptr i32, ptr %base, i64 %cell
    %v = load i32, ptr %p, !ejit.may_const !7
    ret i32 %v
  )", "@pointer = external global ptr, !ejit.metadata !0\n");
  ASSERT_TRUE(M);
  void *Pointer = Data;
  R.registerArray("cell", "pointer", &Pointer, 6);
  replace(*M, R, context());
  EXPECT_EQ(loads(*M->getFunction("f")), 2u);
}

static const char *Helper = R"(
define internal i32 @helper(i64 %not_named_cell) noinline {
  %p = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %not_named_cell, i32 0
  %v = load i32, ptr %p, !ejit.may_const !7
  ret i32 %v
}
)";

TEST_F(PreservedDimsTest, HelperInferenceUsesAllDirectCallEdges) {
  const char *Body = "%v = call i32 @helper(i64 %cell)\nret i32 %v\n";
  auto M = parse(C, Body, Helper);
  ASSERT_TRUE(M);
  replace(*M, R, context());
  EXPECT_EQ(markedLoads(*M), 0u);
  auto Conflict = parse(C, R"(
    %next = add i64 %cell, 1
    %a = call i32 @helper(i64 %cell)
    %b = call i32 @helper(i64 %next)
    %r = add i32 %a, %b
    ret i32 %r
  )", Helper);
  ASSERT_TRUE(Conflict);
  replace(*Conflict, R, context());
  EXPECT_EQ(markedLoads(*Conflict), 1u);
  auto Escaped = parse(C, Body, std::string(Helper) +
                                     "@escape = global ptr @helper\n");
  ASSERT_TRUE(Escaped);
  replace(*Escaped, R, context());
  EXPECT_EQ(markedLoads(*Escaped), 1u);
  auto Outside = parse(C, Body, std::string(Helper) + R"(
    define i32 @outside(i64 %unknown) {
      %v = call i32 @helper(i64 %unknown)
      ret i32 %v
    }
  )");
  ASSERT_TRUE(Outside);
  replace(*Outside, R, context());
  EXPECT_EQ(markedLoads(*Outside), 1u);
}

TEST_F(PreservedDimsTest, ReinitializationDiscardsDeletedIRMaps) {
  auto M = parse(C, DynamicBody);
  ASSERT_TRUE(M);
  EJitStructFieldPass P(R);
  P.setPreservedDimensions(context());
  P.initFromModule(*M);
  M.reset();
  auto Next = parse(C, DynamicBody);
  ASSERT_TRUE(Next);
  P.setPreservedDimensions(context(3));
  P.initFromModule(*Next);
  FunctionAnalysisManager AM;
  P.run(*Next->getFunction("f"), AM);
  EXPECT_FALSE(verifyModule(*Next, &errs()));
  EXPECT_EQ(markedLoads(*Next), 0u);
}

TEST_F(PreservedDimsTest, LastRoundAfterUnrollingRetainsContext) {
  auto M = parseIR(C, std::string(R"(
    target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
    @cfg = external global [6 x [2 x i32]], !ejit.metadata !0
    define i32 @f(i64 %cell, i64 %trp, i32 %x) !ejit.metadata !3 {
    entry:
      br label %loop
    loop:
      %i = phi i64 [0, %entry], [%next, %loop]
      %sum = phi i32 [0, %entry], [%add, %loop]
      %p = getelementptr [6 x [2 x i32]], ptr @cfg, i64 0, i64 %cell, i64 %i
      %v = load i32, ptr %p, !ejit.may_const !7
      %add = add i32 %sum, %v
      %next = add nuw i64 %i, 1
      %again = icmp ult i64 %next, 2
      br i1 %again, label %loop, label %exit
    exit:
      ret i32 %add
    }
  )") + Metadata);
  ASSERT_TRUE(M);
  uint8_t Values[6 * 8] = {};
  Values[2 * 8] = 7;
  Values[2 * 8 + 4] = 9;
  PeriodArrayRegistry Local;
  Local.registerArray("cell", "cfg", Values, 6);
  auto Spec = context();
  replace(*M, Local, Spec);
  EXPECT_EQ(markedLoads(*M), 1u);
  Spec.optLevel = ejit::OptimizationLevel::L3;
  EJitOptimizer Optimizer(Local, true);
  Optimizer.runPipeline(*M, Spec);
  EXPECT_FALSE(verifyModule(*M, &errs()));
  EXPECT_EQ(markedLoads(*M), 0u);
  auto *Ret = dyn_cast<ReturnInst>(M->getFunction("f")->back().getTerminator());
  ASSERT_NE(Ret, nullptr);
  auto *V = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(V, nullptr);
  EXPECT_EQ(V->getZExtValue(), 16u);
  Optimizer.clearAnalyses();
}

TEST_F(PreservedDimsTest, ExplicitLegacyPolicyStillSpecializesArguments) {
  auto M = parse(C, "%v = trunc i64 %cell to i32\nret i32 %v\n");
  ASSERT_TRUE(M);
  EJitOptimizer Optimizer(R, false);
  Optimizer.runPipeline(*M, context());
  auto *Ret = cast<ReturnInst>(M->getFunction("f")->back().getTerminator());
  auto *V = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(V, nullptr);
  EXPECT_EQ(V->getZExtValue(), 2u);
  Optimizer.clearAnalyses();
}

TEST_F(PreservedDimsTest, BorrowedBoundsAndLifecycleAreChecked) {
  const std::string IR = std::string(R"(
    target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
    define i32 @f(i64 %cell, i64 %trp, i32 %x, ptr %object)
        !ejit.metadata !8 {
      %v = load i32, ptr %object
      %p = getelementptr i8, ptr %object, i64 4
      %live = load i32, ptr %p
      %sum = add i32 %live, %v
      store i32 %sum, ptr %p
      ret i32 %sum
    }
    !8 = !{!4, !5, !6, !9}
    !9 = !{!"ejit_bound_ptr", !"cell", i32 3, i64 8, !10}
    !10 = !{i64 0, i64 4}
  )") + Metadata;
  uint8_t Object[8] = {7, 0, 0, 0, 3, 0, 0, 0};
  for (unsigned Case = 0; Case != 5; ++Case) {
    auto M = parseIR(C, IR);
    ASSERT_TRUE(M);
    auto Spec = context();
    Spec.boundPointers.push_back({Object, Case == 1 ? 4u : 8u, 3,
                                  Case == 3 ? 3u : 2u});
    if (Case == 2)
      Spec.dimensions.clear();
    if (Case == 4)
      Spec.boundPointers.push_back(Spec.boundPointers.front());
    replace(*M, R, Spec);
    EXPECT_EQ(loads(*M->getFunction("f")), Case == 0 ? 1u : 2u);
    unsigned Stores = 0;
    for (Instruction &I : instructions(M->getFunction("f")))
      Stores += isa<StoreInst>(I);
    EXPECT_EQ(Stores, 1u);
  }
}

TEST_F(PreservedDimsTest, BigEndianFloatBitsAndNarrowIntegers) {
  struct TestCase { const char *Type; uint64_t Bits; unsigned Bytes; };
  const TestCase Cases[] = {{"double", UINT64_C(0x8000000000000000), 8},
                            {"double", UINT64_C(0x7ff8000000001234), 8},
                            {"float", UINT64_C(0x7fc00123), 4},
                            {"i7", UINT64_C(0x65), 1}};
  for (const TestCase &Case : Cases) {
    uint8_t Bytes[6 * 8] = {};
    for (unsigned I = 0; I != Case.Bytes; ++I)
      Bytes[2 * 8 + I] = Case.Bits >> ((Case.Bytes - I - 1) * 8);
    PeriodArrayRegistry Local;
    Local.registerArray("cell", "cfg", Bytes, 6);
    const std::string IR = std::string(R"(
      target datalayout = "E-m:e-i64:64-i128:128-n32:64-S128"
      @cfg = external global [6 x i64], !ejit.metadata !0
      define )") + Case.Type + R"( @f(i64 %cell, i64 %trp, i32 %x)
          !ejit.metadata !3 {
        %p = getelementptr [6 x i64], ptr @cfg, i64 0, i64 %cell
        %v = load )" + Case.Type + R"(, ptr %p, !ejit.may_const !7
        ret )" + Case.Type + " %v\n}\n" + Metadata;
    auto M = parseIR(C, IR);
    ASSERT_TRUE(M);
    replace(*M, Local, context());
    auto *Ret = cast<ReturnInst>(M->getFunction("f")->back().getTerminator());
    if (Case.Type[0] == 'i') {
      auto *V = dyn_cast<ConstantInt>(Ret->getReturnValue());
      ASSERT_NE(V, nullptr);
      EXPECT_EQ(V->getZExtValue(), Case.Bits);
    } else {
      auto *V = dyn_cast<ConstantFP>(Ret->getReturnValue());
      ASSERT_NE(V, nullptr);
      EXPECT_EQ(V->getValueAPF().bitcastToAPInt().getZExtValue(), Case.Bits);
    }
  }
}

TEST_F(PreservedDimsTest, AmbiguousShapeAddressSpaceAndOverflowStayDynamic) {
  const char *Bodies[] = {
      R"(%p = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 6, i32 0
         %v = load i32, ptr %p, !ejit.may_const !7
         ret i32 %v)",
      R"(%p = getelementptr [6 x %Cfg], ptr @cfg, i64 9223372036854775807
         %v = load i32, ptr %p, !ejit.may_const !7
         ret i32 %v)",
      R"(%p = addrspacecast ptr @cfg to ptr addrspace(1)
         %v = load i32, ptr addrspace(1) %p, !ejit.may_const !7
         ret i32 %v)",
      R"(%outer = getelementptr inbounds [6 x %Cfg], ptr @cfg, i64 100
         %p = getelementptr [6 x %Cfg], ptr %outer, i64 -100
         %v = load i32, ptr %p, !ejit.may_const !7
         ret i32 %v)"};
  for (const char *Body : Bodies) {
    auto M = parse(C, Body);
    ASSERT_TRUE(M);
    replace(*M, R, context(0));
    EXPECT_EQ(markedLoads(*M), 1u);
  }
  auto M = parse(C, DynamicBody);
  ASSERT_TRUE(M);
  auto Spec = context();
  Spec.dimensions.push_back({"cell", 3});
  replace(*M, R, Spec);
  EXPECT_EQ(markedLoads(*M), 1u);
  PeriodArrayRegistry Wrong;
  Wrong.registerArray("cell", "cfg", Data, 5);
  auto N = parse(C, DynamicBody);
  ASSERT_TRUE(N);
  replace(*N, Wrong, context());
  EXPECT_EQ(markedLoads(*N), 1u);
}

TEST_F(PreservedDimsTest, InferenceBudgetExhaustionRetainsLoads) {
  std::string Extra;
  for (unsigned I = 0; I != 256; ++I)
    Extra += "declare void @unused" + std::to_string(I) + "()\n";
  auto M = parse(C, DynamicBody, Extra);
  ASSERT_TRUE(M);
  replace(*M, R, context());
  EXPECT_EQ(markedLoads(*M), 1u);
  EXPECT_FALSE(M->getFunction("f")->getArg(0)->use_empty());
}

TEST_F(PreservedDimsTest, FreeDimWitnessIsLoadOnlyNotACallProof) {
  auto M = parseIR(C, std::string(R"(
    target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
    @cfg = external global [6 x [2 x i32]], !ejit.metadata !0
    define i32 @f(i64 %cell, i64 %slot, i32 %x) !ejit.metadata !8 {
      %p = getelementptr [6 x [2 x i32]], ptr @cfg, i64 0, i64 %cell, i64 %slot
      %v = load i32, ptr %p, !ejit.may_const !7
      store i32 %x, ptr %p
      call void @observe(i64 %slot)
      %c = call i32 @helper(i64 %slot)
      %r = add i32 %c, %v
      ret i32 %r
    }
    declare void @observe(i64)
    define internal i32 @helper(i64 %unknown) noinline {
      %p = getelementptr [6 x [2 x i32]], ptr @cfg, i64 0, i64 2, i64 %unknown
      %v = load i32, ptr %p, !ejit.may_const !7
      ret i32 %v
    }
    !8 = !{!4, !5, !9}
    !9 = !{!"ejit_free_dim", !"", i32 1}
  )") + Metadata);
  ASSERT_TRUE(M);
  uint8_t Values[6 * 8] = {};
  Values[2 * 8] = 7;
  Values[2 * 8 + 4] = 7;
  PeriodArrayRegistry Local;
  Local.registerArray("cell", "cfg", Values, 6);
  auto Spec = context();
  Spec.dimensions.pop_back();
  replace(*M, Local, Spec);
  EXPECT_EQ(markedLoads(*M), 1u);
  EXPECT_EQ(loads(*M->getFunction("f")), 0u);
  EXPECT_EQ(loads(*M->getFunction("helper")), 1u);
  EXPECT_FALSE(M->getFunction("f")->getArg(1)->use_empty());
}

#ifndef EJIT_FREESTANDING
// Real LLVM optimization, CodeGen, JITLink, counters and generated execution.
// This is a pipeline test, NOT a group/session/publication or SRE-board test.
TEST(EJitPreservedDimsNative, RealGenUseAndDynamicStores) {
  ASSERT_FALSE(InitializeNativeTarget());
  ASSERT_FALSE(InitializeNativeTargetAsmPrinter());
  auto JOrErr = orc::LLJITBuilder().create();
  ASSERT_TRUE(static_cast<bool>(JOrErr));
  auto J = std::move(*JOrErr);
  if (!J->getTargetTriple().isOSBinFormatELF())
    GTEST_SKIP() << "The raw-LLJIT profile test currently requires an ELF host";
  struct ConfigRow { uint32_t Gain; uint32_t Live[2]; } Rows[6] = {};
  for (auto &Row : Rows)
    Row.Gain = 7;
  PeriodArrayRegistry R;
  R.registerArray("cell", "cfg", Rows, 6);
  orc::SymbolMap Symbols;
  Symbols[J->mangleAndIntern("cfg")] = orc::ExecutorSymbolDef(
      orc::ExecutorAddr::fromPtr(Rows), JITSymbolFlags::Exported);
  cantFail(J->getMainJITDylib().define(orc::absoluteSymbols(std::move(Symbols))));

  auto TC = std::make_unique<LLVMContext>();
  auto T1 = parse(*TC, DynamicBody);
  ASSERT_TRUE(T1);
  T1->setDataLayout(J->getDataLayout());
  T1->setTargetTriple(J->getTargetTriple());
  auto Spec = context();
  Spec.tier = CompileTier::Instrumented;
  EJitOptimizer Optimizer(R, true);
  std::vector<std::string> Names;
  bool SawT1 = false;
  bool SawT2 = false;
  std::optional<uint64_t> T2Entries;
  unsigned T2MarkedLoads = 0;
  // Match EJitOrcEngine: instrumentation happens after the original module's
  // symbols are claimed; explicitly claim the newly generated counters.
  J->getIRTransformLayer().setTransform(
      [&](orc::ThreadSafeModule TSM, orc::MaterializationResponsibility &MR)
          -> Expected<orc::ThreadSafeModule> {
        orc::SymbolFlagsMap Flags;
        bool Invalid = false;
        TSM.withModuleDo([&](Module &M) {
          Optimizer.clearAnalyses();
          Optimizer.runPipeline(M, Spec);
          Invalid = verifyModule(M, &errs());
          if (Spec.tier == CompileTier::Instrumented) {
            SawT1 = true;
            Names.assign(Optimizer.getLastCounterNames().begin(),
                         Optimizer.getLastCounterNames().end());
            for (const std::string &Name : Names) {
              Flags[J->mangleAndIntern("__profc_" + Name)] =
                  JITSymbolFlags::Exported;
              Flags[J->mangleAndIntern("__profd_" + Name)] =
                  JITSymbolFlags::Exported;
            }
          } else {
            SawT2 = true;
            if (auto *F = M.getFunction("f"))
              if (auto Entries = F->getEntryCount())
                T2Entries = Entries->getCount();
            T2MarkedLoads = markedLoads(M);
          }
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
          if (!Optimizer.getLastMayConstLoadSites().empty())
            Flags[J->mangleAndIntern("__ejit_mayconst_hits")] =
                JITSymbolFlags::Exported;
#endif
          Optimizer.clearAnalyses();
        });
        if (Invalid)
          return createStringError(inconvertibleErrorCode(),
                                   "Invalid preserved-dimension pipeline IR");
        if (!Flags.empty())
          if (auto Err = MR.defineMaterializing(std::move(Flags)))
            return std::move(Err);
        return std::move(TSM);
      });
  cantFail(J->addIRModule(orc::ThreadSafeModule(std::move(T1), std::move(TC))));
  using Fn = uint32_t (*)(uint64_t, uint64_t, uint32_t);
  Fn RunT1 = cantFail(J->lookup("f")).toPtr<Fn>();
  ASSERT_TRUE(SawT1);
  ASSERT_FALSE(Names.empty());
  for (unsigned I = 0; I != 64; ++I) {
    const uint32_t Before = Rows[2].Live[1];
    EXPECT_EQ(RunT1(2, 1, 3), Before + 21 + 2);
  }
  SmallVector<PgoCounterRef, 4> Counters;
  for (const std::string &Name : Names)
    Counters.push_back({Name.c_str(),
        cantFail(J->lookup("__profc_" + Name)).getValue(),
        cantFail(J->lookup("__profd_" + Name)).getValue()});
  Spec.profileData = synthesizeProfileBuffer(Counters);
  ASSERT_FALSE(Spec.profileData.empty());
  Spec.tier = CompileTier::PGOUse;
  TC = std::make_unique<LLVMContext>();
  auto T2 = parse(*TC, DynamicBody);
  ASSERT_TRUE(T2);
  T2->setDataLayout(J->getDataLayout());
  T2->setTargetTriple(J->getTargetTriple());
  auto &FinalJD = cantFail(J->createJITDylib("final"));
  FinalJD.addToLinkOrder(J->getMainJITDylib());
  cantFail(J->addIRModule(FinalJD,
      orc::ThreadSafeModule(std::move(T2), std::move(TC))));
  Fn RunT2 = cantFail(J->lookup(FinalJD, "f")).toPtr<Fn>();
  ASSERT_TRUE(SawT2);
  ASSERT_TRUE(T2Entries.has_value());
  EXPECT_EQ(*T2Entries, 64u);
  EXPECT_EQ(T2MarkedLoads, 0u);
  for (unsigned Cell = 0; Cell != 6; ++Cell)
    for (unsigned TRP = 0; TRP != 2; ++TRP) {
      const uint32_t Before = Rows[Cell].Live[TRP];
      EXPECT_EQ(RunT2(Cell, TRP, 5), Before + 35 + Cell);
      EXPECT_EQ(Rows[Cell].Live[TRP], Before + 35);
    }
}
#endif

} // namespace
