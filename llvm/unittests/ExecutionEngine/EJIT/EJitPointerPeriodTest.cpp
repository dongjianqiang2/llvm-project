//===-- EJitPointerPeriodTest.cpp - Pointer period bounds tests -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <chrono>
#include <limits>
#include <sstream>
#include <thread>

using namespace llvm;
using namespace llvm::ejit;

namespace {
struct AllocationRecord {
  const void *Base;
  uint64_t Size;
  unsigned Queries = 0;
};

static uint64_t lookupAllocationSize(const void *Base, void *Context) {
  auto &Record = *static_cast<AllocationRecord *>(Context);
  ++Record.Queries;
  return Base == Record.Base ? Record.Size : 0;
}

struct PointerFixture {
  unsigned Slots;
  uint64_t Stride;
  std::vector<uint8_t> Data;
  void *Pointer;

  explicit PointerFixture(unsigned Slots)
      : Slots(Slots), Stride(alignTo(8 + Slots * 4, 8)), Data(2 * Stride),
        Pointer(Data.data()) {
    for (unsigned I = 0; I < Slots; ++I)
      Data[Stride + 8 + I * 4 + 1] = 1;
  }

  std::unique_ptr<Module> module(LLVMContext &Ctx, bool Absolute = false,
                                 bool FreeDim = true, bool Layout = true,
                                 bool Lifecycle = true, bool BigEndian = false,
                                 unsigned Field = 1, StringRef Index = {}) {
    std::ostringstream IR;
    IR << "target datalayout = \"" << (BigEndian ? 'E' : 'e')
       << "-p:64:64-i64:64\"\n"
          "%Slot = type { i8, i8, i16 }\n"
          "%Cell = type { i64, ["
       << Slots
       << " x %Slot] }\n"
          "@cfg = external global ptr, !ejit.metadata !0\n"
          "declare void @yes(i16)\n"
          "declare void @no(i16)\n"
          "define i8 @probe(i8 %cell, i16 %slot) !ejit.metadata !3 {\n";
    if (!Absolute)
      IR << "  %root = load ptr, ptr @cfg\n";
    IR << "  %phase = "
       << (Index.empty() ? "urem i16 %slot, " + std::to_string(Slots)
                         : Index.str())
       << "\n  %idx = zext i16 %phase to i64\n"
          "  %selected = getelementptr inbounds nuw %Cell, ptr ";
    if (Absolute)
      IR << "inttoptr (i64 " << reinterpret_cast<uintptr_t>(Data.data())
         << " to ptr)";
    else
      IR << "%root";
    IR << ", i64 1, i32 1, i64 %idx, i32 " << Field
       << "\n"
          "  %v = load i8, ptr %selected, !ejit.may_const !7\n"
          "  %cond = icmp eq i8 %v, 1\n"
          "  br i1 %cond, label %hot, label %cold\n"
          "hot:\n  call void @yes(i16 %slot)\n"
          "  store i8 0, ptr %selected\n  ret i8 %v\n"
          "cold:\n  call void @no(i16 %slot)\n  ret i8 %v\n"
          "}\n!0 = !{!1";
    if (Layout)
      IR << ", !8";
    IR << "}\n!1 = !{!\"ejit_period_arr\", !\"cell\", i32 0}\n"
          "!3 = !{!4";
    if (Lifecycle)
      IR << ", !5";
    if (FreeDim)
      IR << ", !6";
    IR << "}\n!4 = !{!\"ejit_entry\"}\n"
          "!5 = !{!\"ejit_period_arr_ind\", !\"cell\", i32 0}\n"
          "!6 = !{!\"ejit_free_dim\", !\"\", i32 1}\n!7 = !{}\n"
          "!8 = !{!\"ejit_pointer_layout\", i64 "
       << Stride;
    for (unsigned I = 0; I < Slots; ++I)
      IR << ", !" << 10 + I;
    IR << "}\n";
    for (unsigned I = 0; I < Slots; ++I)
      IR << '!' << 10 + I << " = !{i64 " << 8 + I * 4 + 1 << ", i64 1}\n";
    SMDiagnostic Err;
    auto M = parseAssemblyString(IR.str(), Err, Ctx);
    if (!M)
      ADD_FAILURE() << Err.getMessage().str();
    return M;
  }

  void registerRoot(PeriodArrayRegistry &Registry) {
    Registry.registerArray("cell", "cfg", &Pointer, 0);
  }
};

static unsigned markedLoads(Module &M) {
  unsigned Count = 0;
  for (Instruction &I : instructions(M.getFunction("probe")))
    if (auto *LI = dyn_cast<LoadInst>(&I))
      Count += LI->hasMetadata("ejit.may_const");
  return Count;
}

static void optimize(Module &M, PeriodArrayRegistry &Registry,
                      CompileTier Tier = CompileTier::Baseline,
                      ejit::OptimizationLevel Level =
                          ejit::OptimizationLevel::L2) {
  SpecializationContext Context;
  Context.fnName = "probe";
  Context.dimensions.push_back({"cell", 1});
  Context.tier = Tier;
  Context.optLevel = Level;
  EJitOptimizer Optimizer(Registry);
  Optimizer.runPipeline(M, Context);
  EXPECT_FALSE(verifyModule(M, &errs()));
}

static void useLiveSlotStores(Module &M, const PointerFixture &Fixture,
                              unsigned Element = 1) {
  Function *F = M.getFunction("probe");
  // Marked fields are immutable while active; exercise writes through the
  // real slot using a separate, unmarked byte in each element.
  SmallVector<Instruction *, 4> Erase;
  for (Instruction &I : instructions(F))
    if (isa<CallInst>(I) || isa<StoreInst>(I))
      Erase.push_back(&I);
  for (Instruction *I : Erase)
    I->eraseFromParent();
  IRBuilder<> B(F->getEntryBlock().getTerminator());
  auto *Base = ConstantExpr::getIntToPtr(
      B.getInt64(reinterpret_cast<uintptr_t>(Fixture.Pointer)), B.getPtrTy());
  Value *Slot = B.CreateZExt(
      B.CreateURem(F->getArg(1), B.getInt16(Fixture.Slots)), B.getInt64Ty());
  Value *Offset = B.CreateAdd(B.CreateMul(Slot, B.getInt64(4)),
                              B.getInt64(Element * Fixture.Stride + 8));
  Value *Live = B.CreateGEP(B.getInt8Ty(), Base, Offset);
  Value *Count = B.CreateLoad(B.getInt8Ty(), Live);
  B.CreateStore(B.CreateAdd(Count, B.getInt8(1)), Live);
}

// Keep the default fixture on element 1; explicitly select zero for controls.
static void selectElementZero(Module &M, PointerFixture &Fixture,
                              bool LegacyOffsets = false) {
  for (unsigned I = 0; I < Fixture.Slots; ++I)
    Fixture.Data[8 + I * 4 + 1] = 1;
  for (Instruction &I : instructions(M.getFunction("probe")))
    if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
      if (GEP->getName() == "selected")
        GEP->setOperand(1,
                        ConstantInt::get(Type::getInt64Ty(M.getContext()), 0));
  if (LegacyOffsets) {
    auto &Ctx = M.getContext();
    GlobalVariable *GV = M.getGlobalVariable("cfg");
    SmallVector<Metadata *, 8> Tags;
    for (const MDOperand &Op : GV->getMetadata("ejit.metadata")->operands())
      Tags.push_back(Op.get());
    SmallVector<Metadata *, 16> Fields{
        MDString::get(Ctx, "ejit_may_const_field")};
    for (unsigned I = 0; I < Fixture.Slots; ++I)
      Fields.push_back(ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(Ctx), 8 + I * 4 + 1)));
    Tags.push_back(MDNode::get(Ctx, Fields));
    GV->setMetadata("ejit.metadata", MDNode::get(Ctx, Tags));
  }
}
} // namespace

TEST(EJitPointerPeriod, LightweightElementZeroAcrossTiersLevelsAndByteOrders) {
  for (unsigned Slots : {5U, 10U})
    for (bool BigEndian : {false, true})
      for (auto Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                        CompileTier::PGOUse})
        for (auto Level :
             {ejit::OptimizationLevel::L2, ejit::OptimizationLevel::L3}) {
          SCOPED_TRACE(Slots);
          SCOPED_TRACE(BigEndian);
          SCOPED_TRACE(static_cast<unsigned>(Tier));
          SCOPED_TRACE(static_cast<unsigned>(Level));
          PointerFixture Fixture(Slots);
          LLVMContext Ctx;
          auto M = Fixture.module(Ctx, BigEndian, true, true, true, BigEndian);
          ASSERT_NE(M, nullptr);
          selectElementZero(*M, Fixture);
          PeriodArrayRegistry Registry;
          Fixture.registerRoot(Registry);
          optimize(*M, Registry, Tier, Level);
          EXPECT_EQ(markedLoads(*M), 0U);
          EXPECT_FALSE(M->getFunction("probe")->getArg(1)->use_empty());
          bool DynamicStore = false;
          for (Instruction &I : instructions(M->getFunction("probe")))
            if (auto *Store = dyn_cast<StoreInst>(&I))
              DynamicStore |= !isa<Constant>(Store->getPointerOperand());
          EXPECT_TRUE(DynamicStore);
        }
}

TEST(EJitPointerPeriod, LightweightUnknownLookupAndLegacyTypedFields) {
  for (bool Layout : {false, true})
    for (bool Lifecycle : {false, true}) {
      SCOPED_TRACE(Layout);
      SCOPED_TRACE(Lifecycle);
      PointerFixture Fixture(5);
      AllocationRecord Record{Fixture.Pointer, 0};
      LLVMContext Ctx;
      auto M = Fixture.module(Ctx, true, true, Layout, Lifecycle);
      ASSERT_NE(M, nullptr);
      selectElementZero(*M, Fixture, !Layout);
      PeriodArrayRegistry Registry;
      Fixture.registerRoot(Registry);
      Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
      optimize(*M, Registry);
      EXPECT_EQ(markedLoads(*M), 0U);
      EXPECT_GT(Record.Queries, 0U);
    }
}

TEST(EJitPointerPeriod, LightweightNonzeroElementsAcrossTiersAndByteOrders) {
  for (unsigned Element : {1U, 2U, 15U})
    for (unsigned Slots : {5U, 10U})
      for (bool BigEndian : {false, true})
        for (bool Layout : {false, true})
          for (auto Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                            CompileTier::PGOUse})
            for (auto Level :
                 {ejit::OptimizationLevel::L2, ejit::OptimizationLevel::L3}) {
              SCOPED_TRACE(Element);
              SCOPED_TRACE(Slots);
              SCOPED_TRACE(BigEndian);
              SCOPED_TRACE(Layout);
              SCOPED_TRACE(static_cast<unsigned>(Tier));
              SCOPED_TRACE(static_cast<unsigned>(Level));
              PointerFixture Fixture(Slots);
              Fixture.Data.resize((Element + 1) * Fixture.Stride);
              Fixture.Pointer = Fixture.Data.data();
              LLVMContext Ctx;
              auto M =
                  Fixture.module(Ctx, BigEndian, true, Layout, true, BigEndian);
              ASSERT_NE(M, nullptr);
              if (!Layout)
                selectElementZero(*M, Fixture, true);
              auto *Selected = cast<GetElementPtrInst>(
                  M->getFunction("probe")->getValueSymbolTable()->lookup(
                      "selected"));
              Selected->setOperand(
                  1, ConstantInt::get(Type::getInt64Ty(Ctx), Element));
              for (unsigned I = 0; I < Slots; ++I)
                Fixture.Data[Element * Fixture.Stride + 8 + I * 4 + 1] = 7;
              useLiveSlotStores(*M, Fixture, Element);
              PeriodArrayRegistry Registry;
              Fixture.registerRoot(Registry);
              optimize(*M, Registry, Tier, Level);
              EXPECT_EQ(markedLoads(*M), 0U);
              for (Instruction &I : instructions(M->getFunction("probe")))
                if (auto *Ret = dyn_cast<ReturnInst>(&I)) {
                  auto *Value = dyn_cast<ConstantInt>(Ret->getReturnValue());
                  ASSERT_NE(Value, nullptr);
                  EXPECT_EQ(Value->getZExtValue(), 7U);
                }
              bool DynamicStore = false;
              for (Instruction &I : instructions(M->getFunction("probe")))
                if (auto *Store = dyn_cast<StoreInst>(&I))
                  DynamicStore |= !isa<Constant>(Store->getPointerOperand());
              EXPECT_TRUE(DynamicStore);
            }
}

// Rewrite the fixture's selected GEP so the free-dim witness is fused into
// the outer element index: element = Element + (slot % Slots). This is the
// shape `p[period_const * N + free % N]` takes after integer promotion, and
// the fused byte form a flattened pool of stride-sized records produces.
static void fuseWitnessElementIndex(Module &M, const PointerFixture &Fixture,
                                    unsigned Element, bool ByteForm) {
  Function *F = M.getFunction("probe");
  auto *Selected =
      cast<GetElementPtrInst>(F->getValueSymbolTable()->lookup("selected"));
  auto *Phase = cast<Instruction>(F->getValueSymbolTable()->lookup("phase"));
  IRBuilder<NoFolder> B(Selected);
  Value *Witness = B.CreateZExt(Phase, B.getInt64Ty());
  if (!ByteForm) {
    // Typed element step, in the shape integer promotion produces: the add
    // happens in a narrower width and is extended to the index width.
    Value *Narrow = B.CreateAdd(B.CreateZExt(Phase, B.getInt32Ty()),
                                B.getInt32(Element));
    Selected->setOperand(1, B.CreateSExt(Narrow, B.getInt64Ty()));
    Selected->setOperand(3, B.getInt64(0));
    return;
  }
  Value *WitnessBytes = B.CreateMul(Witness, B.getInt64(Fixture.Stride));
  Value *Offset =
      B.CreateAdd(B.getInt64(Element * Fixture.Stride + 9), WitnessBytes);
  auto *Bytes = cast<GetElementPtrInst>(B.CreateGEP(B.getInt8Ty(),
                                                    Selected->getPointerOperand(),
                                                    Offset, "fused"));
  Selected->replaceAllUsesWith(Bytes);
  Selected->eraseFromParent();
}

TEST(EJitPointerPeriod, LightweightFusedConstantPlusWitnessElements) {
  for (unsigned Element : {0U, 1U, 2U, 15U})
    for (unsigned Slots : {5U, 10U})
      for (bool BigEndian : {false, true})
        for (bool ByteForm : {false, true}) {
          SCOPED_TRACE(Element);
          SCOPED_TRACE(Slots);
          SCOPED_TRACE(BigEndian);
          SCOPED_TRACE(ByteForm);
          PointerFixture Fixture(Slots);
          Fixture.Data.resize((Element + 1) * Fixture.Stride);
          Fixture.Pointer = Fixture.Data.data();
          LLVMContext Ctx;
          auto M = Fixture.module(Ctx, BigEndian, true, true, true, BigEndian);
          ASSERT_NE(M, nullptr);
          fuseWitnessElementIndex(*M, Fixture, Element, ByteForm);
          for (unsigned I = 0; I < Slots; ++I)
            Fixture.Data[Element * Fixture.Stride + 8 + I * 4 + 1] = 7;
          useLiveSlotStores(*M, Fixture, Element);
          PeriodArrayRegistry Registry;
          Fixture.registerRoot(Registry);
          optimize(*M, Registry);
          EXPECT_EQ(markedLoads(*M), 0U);
          EXPECT_FALSE(M->getFunction("probe")->getArg(1)->use_empty());
          for (Instruction &I : instructions(M->getFunction("probe")))
            if (auto *Ret = dyn_cast<ReturnInst>(&I)) {
              auto *Value = dyn_cast<ConstantInt>(Ret->getReturnValue());
              ASSERT_NE(Value, nullptr);
              EXPECT_EQ(Value->getZExtValue(), 7U);
            }
          bool DynamicStore = false;
          for (Instruction &I : instructions(M->getFunction("probe")))
            if (auto *Store = dyn_cast<StoreInst>(&I))
              DynamicStore |= !isa<Constant>(Store->getPointerOperand());
          EXPECT_TRUE(DynamicStore);
        }
}

TEST(EJitPointerPeriod, LightweightFusedWitnessAcrossTiersAndLevels) {
  for (auto Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                    CompileTier::PGOUse})
    for (auto Level :
         {ejit::OptimizationLevel::L2, ejit::OptimizationLevel::L3})
      for (bool BigEndian : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Tier));
        SCOPED_TRACE(static_cast<unsigned>(Level));
        SCOPED_TRACE(BigEndian);
        PointerFixture Fixture(5);
        Fixture.Data.resize(3 * Fixture.Stride);
        Fixture.Pointer = Fixture.Data.data();
        LLVMContext Ctx;
        auto M = Fixture.module(Ctx, BigEndian, true, true, true, BigEndian);
        ASSERT_NE(M, nullptr);
        fuseWitnessElementIndex(*M, Fixture, 2, false);
        for (unsigned I = 0; I < Fixture.Slots; ++I)
          Fixture.Data[2 * Fixture.Stride + 8 + I * 4 + 1] = 7;
        useLiveSlotStores(*M, Fixture, 2);
        PeriodArrayRegistry Registry;
        Fixture.registerRoot(Registry);
        optimize(*M, Registry, Tier, Level);
        EXPECT_EQ(markedLoads(*M), 0U);
        for (Instruction &I : instructions(M->getFunction("probe")))
          if (auto *Ret = dyn_cast<ReturnInst>(&I)) {
            auto *Value = dyn_cast<ConstantInt>(Ret->getReturnValue());
            ASSERT_NE(Value, nullptr);
            EXPECT_EQ(Value->getZExtValue(), 7U);
          }
      }
}

TEST(EJitPointerPeriod, LightweightRejectsUnsafeFusedWitnesses) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    SCOPED_TRACE(Case);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true);
    ASSERT_NE(M, nullptr);
    auto *F = M->getFunction("probe");
    auto *Selected =
        cast<GetElementPtrInst>(F->getValueSymbolTable()->lookup("selected"));
    auto *Phase = cast<Instruction>(F->getValueSymbolTable()->lookup("phase"));
    IRBuilder<NoFolder> B(Selected);
    if (Case == 3) {
      // The fused shape folds; an unmarked sibling at the same address must
      // survive on the real slot value.
      Selected->setOperand(1,
                           B.CreateAdd(B.CreateZExt(Phase, B.getInt64Ty()),
                                       B.getInt64(1)));
      Selected->setOperand(3, B.getInt64(0));
      LoadInst *Marked = nullptr;
      for (Instruction &I : instructions(F))
        if (auto *Load = dyn_cast<LoadInst>(&I))
          if (Load->hasMetadata("ejit.may_const"))
            Marked = Load;
      ASSERT_NE(Marked, nullptr);
      IRBuilder<> SB(Marked);
      (void)SB.CreateLoad(B.getInt8Ty(), Selected);
    } else if (Case == 0) {
      // A witness leaf that evaluates nonzero (xor(slot,1)) selects the
      // element by assumption alone.
      Selected->setOperand(1, B.CreateZExt(B.CreateXor(F->getArg(1),
                                                       B.getInt16(1)),
                                           B.getInt64Ty()));
    } else if (Case == 1) {
      // An outer index that is dynamic but not a free dim (the period
      // argument before substitution) proves nothing.
      Selected->setOperand(
          1, B.CreateZExt(F->getArg(0), B.getInt64Ty()));
    } else if (Case == 2) {
      // The witness subtracted rather than added stays dynamic.
      Selected->setOperand(1, B.CreateZExt(B.CreateSub(F->getArg(1),
                                                       B.getInt16(1)),
                                           B.getInt64Ty()));
    } else if (Case == 4) {
      // A constant part whose index*stride product overflows: the fixture
      // GEP is `inbounds nuw`, so the flagged multiply rejects the target
      // computation before any extent check runs.
      Selected->setOperand(
          1, B.CreateAdd(B.CreateZExt(Phase, B.getInt64Ty()),
                         B.getInt64(uint64_t(1) << 61)));
    } else {
      // A select whose witness value is nonzero selects by assumption alone.
      Selected->setOperand(
          1, B.CreateSelect(B.CreateICmpEQ(F->getArg(1), B.getInt16(0)),
                            B.getInt64(2), B.getInt64(0)));
    }
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    EJitStructFieldPass Pass(Registry);
    Pass.initFromModule(*M);
    FunctionAnalysisManager AM;
    Pass.run(*F, AM);
    EXPECT_FALSE(verifyModule(*M, &errs()));
    EXPECT_EQ(markedLoads(*M), Case == 3 ? 0U : 1U);
    if (Case == 3) {
      bool SiblingLoad = false;
      for (Instruction &I : instructions(F))
        if (auto *Load = dyn_cast<LoadInst>(&I))
          if (!Load->hasMetadata("ejit.may_const"))
            SiblingLoad |= !isa<Constant>(Load->getPointerOperand());
      EXPECT_TRUE(SiblingLoad);
      for (Instruction &I : instructions(F))
        if (auto *Ret = dyn_cast<ReturnInst>(&I)) {
          auto *Value = dyn_cast<ConstantInt>(Ret->getReturnValue());
          ASSERT_NE(Value, nullptr);
          EXPECT_EQ(Value->getZExtValue(), 1U);
        }
    }
  }
}

TEST(EJitPointerPeriod, LightweightRejectsUnsafeOrUnauthorizedWitnesses) {
  for (unsigned Case = 0; Case != 10; ++Case) {
    SCOPED_TRACE(Case);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true, Case != 0, Case != 1, true, false,
                            Case == 2 ? 0 : 1,
                            Case == 3 ? "sub nuw i16 %slot, 1" : "");
    ASSERT_NE(M, nullptr);
    if (Case != 4)
      selectElementZero(*M, Fixture);
    else {
      auto *Selected = cast<GetElementPtrInst>(
          M->getFunction("probe")->getValueSymbolTable()->lookup("selected"));
      Selected->setOperand(1,
                           ConstantInt::getSigned(Type::getInt64Ty(Ctx), -1));
    }
    if (Case == 5 || Case == 6 || Case == 7) {
      for (Instruction &I :
           make_early_inc_range(instructions(M->getFunction("probe"))))
        if (auto *Load = dyn_cast<LoadInst>(&I))
          if (Load->hasMetadata("ejit.may_const")) {
            if (Case == 5)
              Load->setVolatile(true);
            else if (Case == 6)
              Load->setAtomic(AtomicOrdering::Acquire);
            else {
              IRBuilder<> B(Load);
              auto *Wide = B.CreateAlignedLoad(
                  B.getInt16Ty(), Load->getPointerOperand(), Align(1));
              Wide->setMetadata("ejit.may_const",
                                Load->getMetadata("ejit.may_const"));
              Load->replaceAllUsesWith(B.CreateTrunc(Wide, B.getInt8Ty()));
              Load->eraseFromParent();
            }
          }
    }
    if (Case == 8) {
      auto *GV = M->getGlobalVariable("cfg");
      auto *MD = GV->getMetadata("ejit.metadata");
      // A malformed present layout must not fall back to legacy offsets.
      selectElementZero(*M, Fixture, true);
      auto *Layout = cast<MDNode>(MD->getOperand(1));
      Layout->replaceOperandWith(1, ConstantAsMetadata::get(ConstantInt::get(
                                        Type::getInt64Ty(Ctx), 0)));
    }
    PeriodArrayRegistry Registry;
    if (Case != 9)
      Fixture.registerRoot(Registry);
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
  }
}

TEST(EJitPointerPeriod, LightweightNonzeroChecksSelectorsAndPrefix) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    SCOPED_TRACE(Case);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true);
    ASSERT_NE(M, nullptr);
    auto *F = M->getFunction("probe");
    auto *Selected =
        cast<GetElementPtrInst>(F->getValueSymbolTable()->lookup("selected"));
    IRBuilder<NoFolder> B(Selected);
    if (Case == 0) {
      Selected->setOperand(1, B.CreateZExt(F->getArg(0), B.getInt64Ty()));
    } else if (Case == 1) {
      // The endpoint lies in element 1, but the inbounds chain first escapes
      // the guaranteed prefix [base, end-of-element-1].
      Value *CellStart =
          B.CreateInBoundsGEP(Selected->getSourceElementType(),
                              Selected->getPointerOperand(), B.getInt64(1));
      Value *Offset =
          B.CreateAdd(Selected->getOperand(3), B.getInt64(2 * Fixture.Stride));
      Value *First = B.CreateInBoundsGEP(B.getInt8Ty(), CellStart, Offset);
      Value *Last = B.CreateInBoundsGEP(B.getInt8Ty(), First,
                                        B.getInt64(9 - 2 * Fixture.Stride));
      Selected->replaceAllUsesWith(Last);
      Selected->eraseFromParent();
    } else if (Case == 2) {
      // The selected field address fits, but the guaranteed complete element
      // would wrap the address space. No pointed-to bytes may be read.
      uintptr_t Base =
          std::numeric_limits<uintptr_t>::max() - 2 * Fixture.Stride + 10;
      Fixture.Pointer = reinterpret_cast<void *>(Base);
      Selected->setOperand(
          0, ConstantExpr::getIntToPtr(B.getInt64(Base), B.getPtrTy()));
    } else if (Case < 5) {
      // slot+1 decomposes into a true constant (1) plus a zero witness, so
      // the fused-index family accepts it (see the witness-fallback spec);
      // a wrapped slot-1 still proves nothing.
      Value *Outer = Case == 3 ? B.CreateAdd(F->getArg(1), B.getInt16(1))
                               : B.CreateSub(F->getArg(1), B.getInt16(1));
      Selected->setOperand(1, B.CreateZExt(Outer, B.getInt64Ty()));
    } else {
      // Preserve a separate constant byte step selecting element 1.
      Value *First =
          B.CreateInBoundsGEP(B.getInt8Ty(), Selected->getPointerOperand(),
                              B.getInt64(Fixture.Stride));
      Value *Offset = B.CreateAdd(
          B.CreateMul(Selected->getOperand(3), B.getInt64(4)), B.getInt64(9));
      Value *Last = B.CreateInBoundsGEP(B.getInt8Ty(), First, Offset);
      Selected->replaceAllUsesWith(Last);
      Selected->eraseFromParent();
    }
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    EJitStructFieldPass Pass(Registry);
    Pass.initFromModule(*M);
    FunctionAnalysisManager AM;
    Pass.run(*F, AM);
    EXPECT_FALSE(verifyModule(*M, &errs()));
    EXPECT_EQ(markedLoads(*M), (Case == 3 || Case == 5) ? 0U : 1U);
    if (Case == 3 || Case == 5)
      for (Instruction &I : instructions(F))
        if (auto *Ret = dyn_cast<ReturnInst>(&I)) {
          auto *Value = dyn_cast<ConstantInt>(Ret->getReturnValue());
          ASSERT_NE(Value, nullptr);
          EXPECT_EQ(Value->getZExtValue(), 1U);
        }
  }
}

TEST(EJitPointerPeriod, LightweightNonzeroDoesNotMatchUnrelatedEarlierRoot) {
  PointerFixture Fixture(5);
  Fixture.Data.resize(3 * Fixture.Stride);
  Fixture.Pointer = Fixture.Data.data() + Fixture.Stride;
  for (unsigned I = 0; I < Fixture.Slots; ++I)
    Fixture.Data[2 * Fixture.Stride + 8 + I * 4 + 1] = 1;
  void *Earlier = Fixture.Data.data();
  LLVMContext Ctx;
  auto M = Fixture.module(Ctx);
  ASSERT_NE(M, nullptr);
  auto *Other =
      new GlobalVariable(*M, PointerType::getUnqual(Ctx), false,
                         GlobalValue::ExternalLinkage, nullptr, "other");
  Other->setMetadata("ejit.metadata",
                     M->getGlobalVariable("cfg")->getMetadata("ejit.metadata"));
  PeriodArrayRegistry Registry;
  Fixture.registerRoot(Registry);
  Registry.registerArray("cell", "other", &Earlier, 0);
  optimize(*M, Registry);
  EXPECT_EQ(markedLoads(*M), 0U);
}

TEST(EJitPointerPeriod, LightweightNeverRescuesStrictRejections) {
  for (bool Explicit : {false, true})
    for (unsigned Case = 0; Case != 4; ++Case) {
      SCOPED_TRACE(Explicit);
      SCOPED_TRACE(Case);
      PointerFixture Fixture(5);
      AllocationRecord Record{Fixture.Pointer,
                              Case == 0 ? 8 : Fixture.Data.size()};
      if (Case == 1)
        --Record.Size;
      LLVMContext Ctx;
      auto M = Fixture.module(Ctx, true, true, true, Case != 2, false,
                              Case == 3 ? 0 : 1);
      ASSERT_NE(M, nullptr);
      selectElementZero(*M, Fixture);
      PeriodArrayRegistry Registry;
      Fixture.registerRoot(Registry);
      if (Explicit)
        ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                         Record.Size));
      else
        Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
      optimize(*M, Registry);
      EXPECT_EQ(markedLoads(*M), 1U);
    }
}

TEST(EJitPointerPeriod,
     LightweightRejectsAmbiguousAliasesAndStrictAliasRescue) {
  for (bool NonzeroElement : {false, true})
    for (bool StrictAlias : {false, true}) {
      SCOPED_TRACE(NonzeroElement);
      SCOPED_TRACE(StrictAlias);
      PointerFixture Fixture(5);
      LLVMContext Ctx;
      auto M = Fixture.module(Ctx, true);
      ASSERT_NE(M, nullptr);
      if (!NonzeroElement)
        selectElementZero(*M, Fixture);
      auto *Alias =
          new GlobalVariable(*M, PointerType::getUnqual(Ctx), false,
                             GlobalValue::ExternalLinkage, nullptr, "alias");
      Alias->setMetadata(
          "ejit.metadata",
          M->getGlobalVariable("cfg")->getMetadata("ejit.metadata"));
      PeriodArrayRegistry Registry;
      Fixture.registerRoot(Registry);
      Registry.registerArray("cell", "alias", &Fixture.Pointer, 0);
      if (StrictAlias)
        ASSERT_TRUE(
            Registry.registerPointerPeriodBounds("alias", Fixture.Pointer, 8));
      optimize(*M, Registry);
      EXPECT_EQ(markedLoads(*M), 1U);
    }
}

TEST(EJitPointerPeriod, LightweightFoldsFlattenedModuloStride128) {
  for (bool BigEndian : {false, true})
    for (unsigned Slots : {5U, 10U})
      for (bool NonzeroElement : {false, true}) {
        SCOPED_TRACE(BigEndian);
        SCOPED_TRACE(Slots);
        SCOPED_TRACE(NonzeroElement);
        std::vector<uint8_t> Data(Slots * 128);
        for (unsigned I = 0; I < Slots; ++I)
          Data[I * 128 + 31] = 7;
        void *Pointer = Data.data();
        std::ostringstream IR;
        IR << "target datalayout = \"" << (BigEndian ? 'E' : 'e')
           << "-p:64:64-i64:64\"\n"
              "@cfg = external global ptr, !ejit.metadata !0\n"
              "define i8 @probe(i8 %cell, i16 %slot) !ejit.metadata !3 {\n"
              "  %phase = urem i16 %slot, "
           << Slots
           << "\n  %idx = zext nneg i16 %phase to i64\n"
              "  %bytes = shl nuw nsw i64 %idx, 7\n"
              "  %p = getelementptr inbounds nuw i8, ptr inttoptr (i64 "
           << reinterpret_cast<uintptr_t>(Data.data()) + 31 +
                  (NonzeroElement ? 128 : 0)
           << " to ptr), i64 %bytes\n"
              "  %v = load i8, ptr %p, !ejit.may_const !7\n"
              "  ret i8 %v\n}\n"
              "!0 = !{!1, !8}\n"
              "!1 = !{!\"ejit_period_arr\", !\"cell\", i32 0}\n"
              "!3 = !{!4, !5, !6}\n"
              "!4 = !{!\"ejit_entry\"}\n"
              "!5 = !{!\"ejit_period_arr_ind\", !\"cell\", i32 0}\n"
              "!6 = !{!\"ejit_free_dim\", !\"\", i32 1}\n"
              "!7 = !{}\n"
              "!8 = !{!\"ejit_pointer_layout\", i64 128, !9}\n"
              "!9 = !{i64 31, i64 1}\n";
        LLVMContext Ctx;
        SMDiagnostic Err;
        auto M = parseAssemblyString(IR.str(), Err, Ctx);
        ASSERT_NE(M, nullptr) << Err.getMessage().str();
        PeriodArrayRegistry Registry;
        Registry.registerArray("cell", "cfg", &Pointer, 0);
        optimize(*M, Registry);
        EXPECT_EQ(markedLoads(*M), NonzeroElement ? 1U : 0U);
        if (!NonzeroElement) {
          auto *Ret =
              cast<ReturnInst>(M->getFunction("probe")->back().getTerminator());
          auto *Value = dyn_cast<ConstantInt>(Ret->getReturnValue());
          ASSERT_NE(Value, nullptr);
          EXPECT_EQ(Value->getZExtValue(), 7U);
        }
      }
}

TEST(EJitPointerPeriod, LightweightChecksIntermediateInboundsBeforeReading) {
  for (bool Escapes : {false, true}) {
    SCOPED_TRACE(Escapes);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true);
    ASSERT_NE(M, nullptr);
    selectElementZero(*M, Fixture);
    auto *Selected = cast<GetElementPtrInst>(
        M->getFunction("probe")->getValueSymbolTable()->lookup("selected"));
    IRBuilder<> B(Selected);
    Value *Index = Selected->getOperand(3);
    // The last numerical address is base+9 in both cases. In the rejected
    // witness, an intermediate inbounds GEP nevertheless goes before base.
    Value *Step = Escapes ? B.CreateSub(Index, B.getInt64(1)) : Index;
    Value *First =
        B.CreateInBoundsGEP(B.getInt8Ty(), Selected->getPointerOperand(), Step);
    Value *Last =
        B.CreateInBoundsGEP(B.getInt8Ty(), First, B.getInt64(Escapes ? 10 : 9));
    Selected->replaceAllUsesWith(Last);
    Selected->eraseFromParent();
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    EJitStructFieldPass Pass(Registry);
    Pass.initFromModule(*M);
    FunctionAnalysisManager AM;
    Pass.run(*M->getFunction("probe"), AM);
    EXPECT_FALSE(verifyModule(*M, &errs()));
    EXPECT_EQ(markedLoads(*M), Escapes ? 1U : 0U);
  }
}

TEST(EJitPointerPeriod, LightweightLegacyOffsetsStillRequireTypedAccessWidth) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true, true, false);
    ASSERT_NE(M, nullptr);
    selectElementZero(*M, Fixture, true);
    if (Case)
      for (Instruction &I :
           make_early_inc_range(instructions(M->getFunction("probe"))))
        if (auto *Load = dyn_cast<LoadInst>(&I))
          if (Load->hasMetadata("ejit.may_const")) {
            IRBuilder<> B(Load);
            Value *Address = Load->getPointerOperand();
            if (Case == 2)
              Address = B.CreateGEP(B.getInt16Ty(), Address, B.getInt64(0));
            auto *Wide = B.CreateAlignedLoad(B.getInt16Ty(), Address, Align(1));
            Wide->setMetadata("ejit.may_const",
                              Load->getMetadata("ejit.may_const"));
            Load->replaceAllUsesWith(B.CreateTrunc(Wide, B.getInt8Ty()));
            Load->eraseFromParent();
          }
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    EJitStructFieldPass Pass(Registry);
    Pass.initFromModule(*M);
    FunctionAnalysisManager AM;
    Pass.run(*M->getFunction("probe"), AM);
    EXPECT_EQ(markedLoads(*M), Case ? 1U : 0U);
  }
}

TEST(EJitPointerPeriod, PreservesBoundPointerContextAfterLoopUnrolling) {
  for (bool BigEndian : {false, true})
    for (unsigned Mode : {0U, 1U, 2U})
      for (auto Level :
           {ejit::OptimizationLevel::L2, ejit::OptimizationLevel::L3})
        for (bool WithSnapshot : {false, true}) {
          SCOPED_TRACE(BigEndian);
          SCOPED_TRACE(Mode);
          SCOPED_TRACE(static_cast<unsigned>(Level));
          SCOPED_TRACE(WithSnapshot);
          LLVMContext Ctx;
          SMDiagnostic Err;
          auto M = parseAssemblyString(R"(
          declare void @observe(i8, i16)
          define void @probe(i8 %cell, i16 %slot, ptr %cfg)
              !ejit.metadata !0 {
          entry:
            br label %loop
          loop:
            %i = phi i64 [0, %entry], [%next, %loop]
            %p = getelementptr inbounds i8, ptr %cfg, i64 %i
            %v = load i8, ptr %p, !ejit.may_const !7
            call void @observe(i8 %v, i16 %slot)
            %next = add nuw nsw i64 %i, 1
            %more = icmp ult i64 %next, 4
            br i1 %more, label %loop, label %exit, !llvm.loop !8
          exit:
            ret void
          }
          !0 = !{!1, !2, !3}
          !1 = !{!"ejit_entry"}
          !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
          !3 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 4,
                  !4, !5, !6, !10}
          !4 = !{i64 0, i64 1}
          !5 = !{i64 1, i64 1}
          !6 = !{i64 2, i64 1}
          !7 = !{}
          !8 = distinct !{!8, !9}
          !9 = !{!"llvm.loop.unroll.full"}
          !10 = !{i64 3, i64 1}
        )",
                                       Err, Ctx);
          ASSERT_NE(M, nullptr) << Err.getMessage().str();
          M->setDataLayout(BigEndian ? "E-p:64:64-i64:64" : "e-p:64:64-i64:64");
          const uint8_t Data[] = {7, 9, 11, 13};
          SpecializationContext Context;
          Context.fnName = "probe";
          Context.dimensions.push_back({"cell", 1});
          if (WithSnapshot)
            Context.boundPointers.push_back({Data, sizeof(Data), 2, 1});
          Context.tier = Mode ? CompileTier::PGOUse : CompileTier::Baseline;
#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
          // The audit-only field exists only in audit builds; without the
          // define this iteration degenerates to the plain PGOUse case.
          Context.profileAuditOnly = Mode == 2;
#endif
          Context.optLevel = Level;
          PeriodArrayRegistry Registry;
          EJitOptimizer Optimizer(Registry);
          Optimizer.runPipeline(*M, Context);
          ASSERT_FALSE(verifyModule(*M, &errs()));
          EXPECT_EQ(markedLoads(*M), WithSnapshot ? 0U : 4U);
          unsigned Calls = 0;
          for (Instruction &I : instructions(M->getFunction("probe")))
            if (auto *Call = dyn_cast<CallInst>(&I))
              if (Call->getCalledFunction() &&
                  Call->getCalledFunction()->getName() == "observe") {
                auto *Value = dyn_cast<ConstantInt>(Call->getArgOperand(0));
                ASSERT_LT(Calls, std::size(Data));
                if (WithSnapshot) {
                  ASSERT_NE(Value, nullptr);
                  EXPECT_EQ(Value->getZExtValue(), Data[Calls]);
                } else {
                  EXPECT_EQ(Value, nullptr);
                }
                ++Calls;
                EXPECT_EQ(Call->getArgOperand(1),
                          M->getFunction("probe")->getArg(1));
              }
          EXPECT_EQ(Calls, 4U);
        }
}

TEST(EJitPointerPeriod, FoldsNestedN5N10AcrossPipelineTiers) {
  for (unsigned Slots : {5U, 10U})
    for (CompileTier Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                             CompileTier::PGOUse}) {
      SCOPED_TRACE(static_cast<unsigned>(Tier));
      PointerFixture Fixture(Slots);
      LLVMContext Ctx;
      auto M = Fixture.module(Ctx);
      ASSERT_NE(M, nullptr);
      PeriodArrayRegistry Registry;
      Fixture.registerRoot(Registry);
      ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                       Fixture.Data.size()));
      optimize(*M, Registry, Tier);
      EXPECT_EQ(markedLoads(*M), 0U);
      bool LiveStore = false, LiveCall = false;
      Function *F = M->getFunction("probe");
      EXPECT_FALSE(F->getArg(1)->use_empty());
      for (Instruction &I : instructions(F)) {
        if (auto *Store = dyn_cast<StoreInst>(&I))
          LiveStore |= !isa<Constant>(Store->getPointerOperand());
        if (auto *Call = dyn_cast<CallInst>(&I))
          if (Function *Callee = Call->getCalledFunction()) {
            EXPECT_NE(Callee->getName(), "no");
            LiveCall |= Callee->getName() == "yes" &&
                        Call->getArgOperand(0) == F->getArg(1);
          }
      }
      EXPECT_TRUE(LiveStore);
      EXPECT_TRUE(LiveCall);
    }
}

TEST(EJitPointerPeriod, AbsoluteI8UsesTargetLayoutInBothByteOrders) {
  for (bool BigEndian : {false, true}) {
    PointerFixture Fixture(10);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true, true, true, true, BigEndian);
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                     Fixture.Data.size()));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 0U);
    for (Instruction &I : instructions(M->getFunction("probe")))
      if (auto *Return = dyn_cast<ReturnInst>(&I)) {
        auto *Value = dyn_cast<ConstantInt>(Return->getReturnValue());
        ASSERT_NE(Value, nullptr);
        EXPECT_EQ(Value->getZExtValue(), 1U);
      }
  }
}

TEST(EJitPointerPeriod, FoldsStaticPointerPeriod) {
  PointerFixture Fixture(5);
  LLVMContext Ctx;
  auto M = Fixture.module(Ctx);
  ASSERT_NE(M, nullptr);
  MDNode *Old = M->getGlobalVariable("cfg")->getMetadata("ejit.metadata");
  auto *Static = MDNode::get(
      Ctx, {MDString::get(Ctx, "ejit_period"), MDString::get(Ctx, "static")});
  M->getGlobalVariable("cfg")->setMetadata(
      "ejit.metadata", MDNode::get(Ctx, {Static, Old->getOperand(1).get()}));
  PeriodArrayRegistry Registry;
  Registry.registerStaticVar("cfg", &Fixture.Pointer);
  ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                   Fixture.Data.size()));
  optimize(*M, Registry);
  EXPECT_EQ(markedLoads(*M), 0U);
}

TEST(EJitPointerPeriod, FoldsFlattenedPeriodAndSlotOffsets) {
  for (unsigned Slots : {5U, 10U}) {
    PointerFixture Fixture(Slots);
    for (unsigned I = 0; I < Slots; ++I)
      Fixture.Data[Slots * 4 + I * 4 + 1] = 1;
    LLVMContext Ctx;
    std::ostringstream IR;
    IR << "target datalayout = \"e-p:64:64-i64:64\"\n"
          "%Slot = type { i8, i8, i16 }\n"
          "@cfg = external global ptr, !ejit.metadata !0\n"
          "define i8 @probe(i8 %trp, i16 %slot) !ejit.metadata !3 {\n"
          "  %root = load ptr, ptr @cfg\n"
          "  %block = getelementptr inbounds nuw i8, ptr %root, i64 "
       << Slots * 4
       << "\n"
          "  %phase = urem i16 %slot, "
       << Slots
       << "\n"
          "  %idx = zext i16 %phase to i64\n"
          "  %field = getelementptr inbounds nuw %Slot, ptr %block, "
          "i64 %idx, i32 1\n"
          "  %value = load i8, ptr %field, !ejit.may_const !7\n"
          "  store i8 0, ptr %field\n  ret i8 %value\n}\n"
          "!0 = !{!1, !8}\n"
          "!1 = !{!\"ejit_period_arr\", !\"cell\", i32 0}\n"
          "!3 = !{!4, !5, !6}\n!4 = !{!\"ejit_entry\"}\n"
          "!5 = !{!\"ejit_period_arr_ind\", !\"cell\", i32 0}\n"
          "!6 = !{!\"ejit_free_dim\", !\"\", i32 1}\n!7 = !{}\n"
          "!8 = !{!\"ejit_pointer_layout\", i64 4, !9}\n"
          "!9 = !{i64 1, i64 1}\n";
    SMDiagnostic Err;
    auto M = parseAssemblyString(IR.str(), Err, Ctx);
    ASSERT_NE(M, nullptr) << Err.getMessage().str();
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                     Slots * 4 * 2));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 0U);
    auto *Return =
        dyn_cast<ReturnInst>(M->getFunction("probe")->back().getTerminator());
    ASSERT_NE(Return, nullptr);
    auto *Value = dyn_cast<ConstantInt>(Return->getReturnValue());
    ASSERT_NE(Value, nullptr);
    EXPECT_EQ(Value->getZExtValue(), 1U);
  }
}

TEST(EJitPointerPeriod, UnknownBoundsRelaxationPreservesOtherRequirements) {
  for (unsigned Missing = 0; Missing < 5; ++Missing) {
    SCOPED_TRACE(Missing);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M =
        Fixture.module(Ctx, false, Missing != 1, Missing != 2, Missing != 3);
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    if (Missing != 4)
      Fixture.registerRoot(Registry);
    if (Missing != 0)
      ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                       Fixture.Data.size()));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), Missing == 0 ? 0U : 1U);
  }
}

TEST(EJitPointerPeriod, RejectsCrossFieldAndAtomicLoads) {
  for (bool Atomic : {false, true}) {
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx);
    ASSERT_NE(M, nullptr);
    SmallVector<LoadInst *, 4> Loads;
    for (Instruction &I : instructions(M->getFunction("probe")))
      if (auto *Load = dyn_cast<LoadInst>(&I))
        if (Load->hasMetadata("ejit.may_const"))
          Loads.push_back(Load);
    for (LoadInst *Load : Loads) {
      if (Atomic) {
        Load->setAtomic(AtomicOrdering::Acquire);
      } else {
        IRBuilder<> B(Load);
        auto *Wide = B.CreateAlignedLoad(B.getInt16Ty(),
                                         Load->getPointerOperand(), Align(1));
        Wide->setMetadata("ejit.may_const",
                          Load->getMetadata("ejit.may_const"));
        Load->replaceAllUsesWith(B.CreateTrunc(Wide, B.getInt8Ty()));
        Load->eraseFromParent();
      }
    }
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                     Fixture.Data.size()));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
  }
}

TEST(EJitPointerPeriod, RejectsAmbiguousEligibleAllocations) {
  for (bool Discover : {false, true}) {
    PointerFixture Fixture(5);
    AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true);
    ASSERT_NE(M, nullptr);
    auto *Alias =
        new GlobalVariable(*M, PointerType::getUnqual(Ctx), false,
                           GlobalValue::ExternalLinkage, nullptr, "alias");
    Alias->setMetadata(
        "ejit.metadata", M->getGlobalVariable("cfg")->getMetadata("ejit.metadata"));
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    Registry.registerArray("cell", "alias", &Fixture.Pointer, 0);
    if (Discover) {
      Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
    } else {
      ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                       Fixture.Data.size()));
      ASSERT_TRUE(Registry.registerPointerPeriodBounds("alias", Fixture.Pointer,
                                                       Fixture.Data.size()));
    }
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
  }
}

TEST(EJitPointerPeriod, RejectsInvalidWitnessAndUnauthorizedField) {
  for (unsigned Rejection = 0; Rejection < 7; ++Rejection) {
    SCOPED_TRACE(Rejection);
    PointerFixture Fixture(5);
    LLVMContext Ctx;
    // A different field is deliberately marked in IR: the compiler-generated
    // layout must reject it even when an earlier transform copied the marker.
    auto M = Fixture.module(Ctx, false, true, true, true, false,
                            Rejection == 0 ? 0 : 1,
                            Rejection == 1 ? "sub nuw i16 %slot, 1" : "");
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    uint64_t Size = Fixture.Data.size();
    if (Rejection == 2)
      Size = Fixture.Stride; // selected cell is outside the allocation
    if (Rejection == 3)
      --Size; // not a whole number of pointee elements
    ASSERT_TRUE(
        Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer, Size));
    std::vector<uint8_t> Other(Fixture.Data.size());
    if (Rejection == 4)
      Fixture.Pointer = Other.data(); // stale bounds must not transfer
    if (Rejection == 5)
      for (Instruction &I : instructions(M->getFunction("probe")))
        if (auto *Load = dyn_cast<LoadInst>(&I))
          Load->setVolatile(true);
    if (Rejection == 6)
      M->getGlobalVariable("cfg")->setMetadata(
          "ejit.metadata",
          MDNode::get(
              Ctx, {MDNode::get(Ctx, {MDString::get(Ctx, "ejit_period_arr"),
                                      MDString::get(Ctx, "cell"),
                                      ConstantAsMetadata::get(ConstantInt::get(
                                          Type::getInt32Ty(Ctx), 0))}),
                    MDNode::get(Ctx, {MDString::get(Ctx, "ejit_pointer_layout"),
                                      ConstantAsMetadata::get(ConstantInt::get(
                                          Type::getInt64Ty(Ctx), 0))})}));
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
  }
}

TEST(EJitPointerPeriod, BoundsRegistrationValidatesAndKeepsFirst) {
  PeriodArrayRegistry Registry;
  uint8_t Data[16] = {};
  EXPECT_FALSE(Registry.registerPointerPeriodBounds("", Data, sizeof(Data)));
  EXPECT_FALSE(Registry.registerPointerPeriodBounds("cfg", nullptr, 16));
  EXPECT_FALSE(Registry.registerPointerPeriodBounds("cfg", Data, 0));
  EXPECT_FALSE(Registry.registerPointerPeriodBounds(
      "cfg", Data, std::numeric_limits<uint64_t>::max()));
  EXPECT_TRUE(Registry.registerPointerPeriodBounds("cfg", Data, sizeof(Data)));
  EXPECT_TRUE(Registry.registerPointerPeriodBounds("cfg", Data, sizeof(Data)));
  EXPECT_FALSE(Registry.registerPointerPeriodBounds("cfg", Data + 1, 8));
  ASSERT_NE(Registry.getPointerPeriodBounds("cfg"), nullptr);
  EXPECT_EQ(Registry.getPointerPeriodBounds("cfg")->base, Data);
  EXPECT_EQ(Registry.getPointerPeriodBounds("cfg")->size, sizeof(Data));
}

TEST(EJitPointerPeriod, CApiStagesBoundsAndRejectsConflicts) {
  auto &Store = EJitRegistrationStore::instance();
  Store.consume();
  uint8_t Data[16] = {};
  EXPECT_EQ(ejit_register_pointer_period_bounds(nullptr, Data, 16),
            EJIT_ERR_INVALID_PARAM);
  EXPECT_EQ(ejit_register_pointer_period_bounds("cfg", Data, 16), EJIT_OK);
  EXPECT_EQ(ejit_register_pointer_period_bounds("cfg", Data, 16), EJIT_OK);
  EXPECT_EQ(ejit_register_pointer_period_bounds("cfg", Data, 8),
            EJIT_ERR_INVALID_PARAM);
  auto Stored = Store.consume();
  // Bounds alone must not suppress automatic static registry discovery.
  EXPECT_TRUE(Stored.empty());
  ASSERT_EQ(Stored.pointerPeriodBounds.size(), 1U);
  EXPECT_EQ(Stored.pointerPeriodBounds[0].base, Data);
  EXPECT_EQ(Stored.pointerPeriodBounds[0].size, 16U);
  EXPECT_TRUE(Store.consume().pointerPeriodBounds.empty());
}

TEST(EJitPointerPeriod, InitTransfersBoundsIncludingForcedStaticRegistry) {
  for (bool ForceStatic : {false, true}) {
    auto &Store = EJitRegistrationStore::instance();
    Store.consume();
    uint8_t Data[16] = {};
    Store.registerStaticVar("bounds_marker", Data);
    ASSERT_TRUE(Store.registerPointerPeriodBounds("cfg", Data, sizeof(Data)));
    Config Options;
    Options.forceStaticRegistry = ForceStatic;
    EJit Engine(Options);
    auto *Bounds = Engine.getRegistry().getPointerPeriodBounds("cfg");
    ASSERT_NE(Bounds, nullptr);
    EXPECT_EQ(Bounds->base, Data);
    EXPECT_EQ(Bounds->size, sizeof(Data));
  }
}

TEST(EJitPointerPeriod, DiscoversN5N10WithoutPerGlobalBounds) {
  for (unsigned Slots : {5U, 10U})
    for (bool BigEndian : {false, true})
      for (CompileTier Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                               CompileTier::PGOUse}) {
        SCOPED_TRACE(Slots);
        SCOPED_TRACE(BigEndian);
        SCOPED_TRACE(static_cast<unsigned>(Tier));
        PointerFixture Fixture(Slots);
        AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
        LLVMContext Ctx;
        // The host pointer slot uses host byte order. For cross-endian IR,
        // start after pointer materialization, as the existing BE test does.
        auto M = Fixture.module(Ctx, BigEndian, true, true, true, BigEndian);
        ASSERT_NE(M, nullptr);
        PeriodArrayRegistry Registry;
        Fixture.registerRoot(Registry);
        Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
        optimize(*M, Registry, Tier);
        EXPECT_EQ(markedLoads(*M), 0U);
        EXPECT_GT(Record.Queries, 0U);
        EXPECT_EQ(Registry.getPointerPeriodBounds("cfg"), nullptr);
        bool DynamicStore = false;
        for (Instruction &I : instructions(M->getFunction("probe"))) {
          if (auto *Store = dyn_cast<StoreInst>(&I))
            DynamicStore |= !isa<Constant>(Store->getPointerOperand());
          if (auto *Call = dyn_cast<CallInst>(&I))
            if (auto *Callee = Call->getCalledFunction())
              EXPECT_NE(Callee->getName(), "no");
        }
        EXPECT_TRUE(DynamicStore);
        EXPECT_FALSE(M->getFunction("probe")->getArg(1)->use_empty());
      }
}

TEST(EJitPointerPeriod, DiscoveredBoundsDoNotBypassSafetyChecks) {
  for (unsigned Rejection = 0; Rejection < 7; ++Rejection) {
    SCOPED_TRACE(Rejection);
    PointerFixture Fixture(10);
    AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
    if (Rejection == 0)
      Record.Size = 0; // allocator does not know this address
    if (Rejection == 1)
      Record.Size = Fixture.Stride; // the witness is outside the allocation
    if (Rejection == 2)
      Record.Size = std::numeric_limits<uint64_t>::max(); // wraps address space
    if (Rejection == 3)
      --Record.Size; // partial pointee element
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, false, true, Rejection != 4, Rejection != 5,
                            false, Rejection == 6 ? 0 : 1);
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), Rejection == 0 ? 0U : 1U);
  }
}

TEST(EJitPointerPeriod, ExplicitBoundsWinOverAllocationLookup) {
  PointerFixture Fixture(5);
  AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
  PeriodArrayRegistry Registry;
  Fixture.registerRoot(Registry);
  Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
  // Explicitly bounded too narrowly. Discovery must not widen this bound.
  ASSERT_TRUE(Registry.registerPointerPeriodBounds("cfg", Fixture.Pointer,
                                                   Fixture.Stride));
  LLVMContext Ctx;
  auto M = Fixture.module(Ctx);
  ASSERT_NE(M, nullptr);
  optimize(*M, Registry);
  EXPECT_EQ(markedLoads(*M), 1U);
  EXPECT_EQ(Record.Queries, 0U);
  // Stale explicit bounds must not transfer to a newly discovered allocation.
  uint8_t Other[16] = {};
  auto Bounds = Registry.resolvePointerPeriodBounds("cfg", Other);
  ASSERT_TRUE(Bounds.has_value());
  EXPECT_EQ(Bounds->base, Fixture.Pointer);
  EXPECT_EQ(Record.Queries, 0U);
}

TEST(EJitPointerPeriod, AllocationLookupOnlyAcceptsExactKnownBase) {
  uint8_t Data[16] = {};
  AllocationRecord Record{Data, sizeof(Data)};
  PeriodArrayRegistry Registry;
  Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
  EXPECT_FALSE(Registry.resolvePointerPeriodBounds("cfg", nullptr));
  EXPECT_EQ(Record.Queries, 0U);
  EXPECT_FALSE(Registry.resolvePointerPeriodBounds("cfg", Data + 1));
  auto Bounds = Registry.resolvePointerPeriodBounds("cfg", Data);
  ASSERT_TRUE(Bounds.has_value());
  EXPECT_EQ(Bounds->base, Data);
  EXPECT_EQ(Bounds->size, sizeof(Data));
}

TEST(EJitPointerPeriod, CApiStagesAllocationLookupAndRejectsConflicts) {
  auto &Store = EJitRegistrationStore::instance();
  Store.consume();
  uint8_t Data[16] = {};
  AllocationRecord Record{Data, sizeof(Data)};
  EXPECT_EQ(ejit_set_allocation_size_lookup(nullptr, nullptr),
            EJIT_ERR_INVALID_PARAM);
  EXPECT_EQ(ejit_set_allocation_size_lookup(lookupAllocationSize, &Record),
            EJIT_OK);
  EXPECT_EQ(ejit_set_allocation_size_lookup(lookupAllocationSize, &Record),
            EJIT_OK);
  EXPECT_EQ(ejit_set_allocation_size_lookup(lookupAllocationSize, Data),
            EJIT_ERR_INVALID_PARAM);
  auto Stored = Store.consume();
  EXPECT_TRUE(Stored.empty()); // supplemental lookup preserves table discovery
  EXPECT_EQ(Stored.allocationSizeLookup.callback, lookupAllocationSize);
  EXPECT_EQ(Stored.allocationSizeLookup.context, &Record);
  EXPECT_EQ(Store.consume().allocationSizeLookup.callback, nullptr);
}

TEST(EJitPointerPeriod, InitTransfersAllocationLookupWithForcedStaticRegistry) {
  for (bool ForceStatic : {false, true}) {
    auto &Store = EJitRegistrationStore::instance();
    Store.consume();
    uint8_t Data[16] = {};
    AllocationRecord Record{Data, sizeof(Data)};
    Store.registerStaticVar("lookup_marker", Data);
    ASSERT_TRUE(Store.setAllocationSizeLookup({lookupAllocationSize, &Record}));
    Config Options;
    Options.forceStaticRegistry = ForceStatic;
    EJit Engine(Options);
    auto Bounds = Engine.getRegistry().resolvePointerPeriodBounds("cfg", Data);
    ASSERT_TRUE(Bounds.has_value());
    EXPECT_EQ(Bounds->base, Data);
    EXPECT_EQ(Bounds->size, sizeof(Data));
  }
}

TEST(EJitPointerPeriod, AllocationLookupKeepsLegacyConstantIndexFolds) {
  // Without layout metadata or the lifecycle contract, a constant-index load
  // folds through the legacy path. Installing a lookup must not change that.
  for (bool Layout : {false, true})
    for (bool Lifecycle : {false, true})
      for (bool Discover : {false, true}) {
        SCOPED_TRACE(Layout);
        SCOPED_TRACE(Lifecycle);
        SCOPED_TRACE(Discover);
        PointerFixture Fixture(5);
        AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
        LLVMContext Ctx;
        auto M = Fixture.module(Ctx, false, false, Layout, Lifecycle, false, 1,
                                "add i16 0, 3");
        ASSERT_NE(M, nullptr);
        PeriodArrayRegistry Registry;
        Fixture.registerRoot(Registry);
        if (Discover)
          Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
        optimize(*M, Registry);
        EXPECT_EQ(markedLoads(*M), 0U);
        for (Instruction &I : instructions(M->getFunction("probe")))
          if (auto *Call = dyn_cast<CallInst>(&I))
            if (auto *Callee = Call->getCalledFunction())
              EXPECT_NE(Callee->getName(), "no");
      }
}

TEST(EJitPointerPeriod, AllocationLookupDoesNotMakeExistingFoldAmbiguous) {
  // A discovered allocation aliasing a registered object must not make the
  // object's unique match ambiguous.
  for (bool Discover : {false, true}) {
    SCOPED_TRACE(Discover);
    PointerFixture Fixture(5);
    AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true);
    ASSERT_NE(M, nullptr);
    auto *CellType = StructType::getTypeByName(Ctx, "Cell");
    ASSERT_NE(CellType, nullptr);
    auto *Storage = new GlobalVariable(
        *M, ArrayType::get(CellType, 2), false, GlobalValue::ExternalLinkage,
        nullptr, "storage");
    auto *I32 = Type::getInt32Ty(Ctx);
    SmallVector<Metadata *, 8> StorageMD{MDNode::get(
        Ctx, {MDString::get(Ctx, "ejit_period_arr"), MDString::get(Ctx, "cell"),
              ConstantAsMetadata::get(ConstantInt::get(I32, 2))})};
    for (unsigned I = 0; I < Fixture.Slots; ++I)
      StorageMD.push_back(MDNode::get(
          Ctx, {MDString::get(Ctx, "ejit_may_const_field"),
                ConstantAsMetadata::get(ConstantInt::get(I32, 8 + I * 4 + 1))}));
    Storage->setMetadata("ejit.metadata", MDNode::get(Ctx, StorageMD));
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    Registry.registerArray("cell", "storage", Fixture.Pointer, 2);
    if (Discover)
      Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 0U);
  }
}

TEST(EJitPointerPeriod, DiscoversAbsoluteConstantElementsAcrossTiersAndLevels) {
  for (unsigned Slots : {5U, 10U})
    for (bool BigEndian : {false, true})
      for (CompileTier Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                               CompileTier::PGOUse})
        for (auto Level :
             {ejit::OptimizationLevel::L2, ejit::OptimizationLevel::L3}) {
          SCOPED_TRACE(Slots);
          SCOPED_TRACE(BigEndian);
          SCOPED_TRACE(static_cast<unsigned>(Tier));
          SCOPED_TRACE(static_cast<unsigned>(Level));
          PointerFixture Fixture(Slots);
          AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
          LLVMContext Ctx;
          auto M = Fixture.module(Ctx, true, false, true, true, BigEndian, 1,
                                  "add i16 0, 3");
          ASSERT_NE(M, nullptr);
          PeriodArrayRegistry Registry;
          Fixture.registerRoot(Registry);
          Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
          optimize(*M, Registry, Tier, Level);
          EXPECT_GT(Record.Queries, 0U);
          EXPECT_EQ(markedLoads(*M), 0U);
          for (Instruction &I : instructions(M->getFunction("probe")))
            if (auto *Call = dyn_cast<CallInst>(&I))
              if (auto *Callee = Call->getCalledFunction())
                EXPECT_NE(Callee->getName(), "no");
        }
}

TEST(EJitPointerPeriod, DiscoversFlattenedConstantElementAddress) {
  for (bool BigEndian : {false, true}) {
    PointerFixture Fixture(10);
    AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true, false, true, true, BigEndian, 1,
                            "add i16 0, 3");
    ASSERT_NE(M, nullptr);
    for (Instruction &I : instructions(M->getFunction("probe")))
      if (auto *Load = dyn_cast<LoadInst>(&I))
        if (Load->hasMetadata("ejit.may_const")) {
          IRBuilder<> B(Load);
          auto *Base = ConstantExpr::getIntToPtr(
              B.getInt64(reinterpret_cast<uintptr_t>(Fixture.Pointer)),
              B.getPtrTy());
          Load->setOperand(
              0, B.CreateGEP(B.getInt8Ty(), Base,
                             B.getInt64(Fixture.Stride + 8 + 3 * 4 + 1)));
        }
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
    optimize(*M, Registry);
    EXPECT_GT(Record.Queries, 0U);
    EXPECT_EQ(markedLoads(*M), 0U);
  }
}

TEST(EJitPointerPeriod, ConstantDiscoveryRequiresStrictAuthorization) {
  for (unsigned Rejection = 0; Rejection < 10; ++Rejection) {
    SCOPED_TRACE(Rejection);
    PointerFixture Fixture(5);
    AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
    if (Rejection == 0)
      Record.Size = 0;
    if (Rejection == 1)
      Record.Size = Fixture.Stride;
    if (Rejection == 2)
      --Record.Size;
    if (Rejection == 3)
      Record.Size = std::numeric_limits<uint64_t>::max();
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true, false, Rejection != 4, Rejection != 5,
                            false, Rejection == 6 ? 0 : 1, "add i16 0, 3");
    ASSERT_NE(M, nullptr);
    SmallVector<LoadInst *, 4> Loads;
    for (Instruction &I : instructions(M->getFunction("probe")))
      if (auto *Load = dyn_cast<LoadInst>(&I))
        if (Load->hasMetadata("ejit.may_const"))
          Loads.push_back(Load);
    for (LoadInst *Load : Loads) {
      if (Rejection == 7)
        Load->setVolatile(true);
      if (Rejection == 8)
        Load->setAtomic(AtomicOrdering::Acquire);
      if (Rejection == 9) {
        IRBuilder<> B(Load);
        auto *Wide = B.CreateAlignedLoad(B.getInt16Ty(),
                                         Load->getPointerOperand(), Align(1));
        Wide->setMetadata("ejit.may_const",
                          Load->getMetadata("ejit.may_const"));
        Load->replaceAllUsesWith(B.CreateTrunc(Wide, B.getInt8Ty()));
        Load->eraseFromParent();
      }
    }
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
  }
}

TEST(EJitPointerPeriod, ConstantDiscoveryRejectsAmbiguousAllocations) {
  PointerFixture Fixture(5);
  AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
  LLVMContext Ctx;
  auto M =
      Fixture.module(Ctx, true, false, true, true, false, 1, "add i16 0, 3");
  ASSERT_NE(M, nullptr);
  auto *Alias =
      new GlobalVariable(*M, PointerType::getUnqual(Ctx), false,
                         GlobalValue::ExternalLinkage, nullptr, "alias");
  Alias->setMetadata("ejit.metadata",
                     M->getGlobalVariable("cfg")->getMetadata("ejit.metadata"));
  PeriodArrayRegistry Registry;
  Fixture.registerRoot(Registry);
  Registry.registerArray("cell", "alias", &Fixture.Pointer, 0);
  Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
  optimize(*M, Registry);
  EXPECT_EQ(markedLoads(*M), 1U);
}

TEST(EJitPointerPeriod, ConstantDiscoveryCannotOverrideExplicitBounds) {
  for (bool Stale : {false, true}) {
    SCOPED_TRACE(Stale);
    PointerFixture Fixture(5);
    AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
    std::vector<uint8_t> Other(Fixture.Data.size());
    LLVMContext Ctx;
    auto M =
        Fixture.module(Ctx, true, false, true, true, false, 1, "add i16 0, 3");
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    ASSERT_TRUE(Registry.registerPointerPeriodBounds(
        "cfg", Stale ? Other.data() : Fixture.Pointer,
        Stale ? Fixture.Data.size() : Fixture.Stride));
    Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
    optimize(*M, Registry);
    EXPECT_EQ(markedLoads(*M), 1U);
    EXPECT_EQ(Record.Queries, 0U);
  }
}

TEST(EJitPointerPeriod, AbsoluteConstantElementWithoutBoundsStaysDynamic) {
  PointerFixture Fixture(5);
  LLVMContext Ctx;
  auto M =
      Fixture.module(Ctx, true, false, true, true, false, 1, "add i16 0, 3");
  ASSERT_NE(M, nullptr);
  PeriodArrayRegistry Registry;
  Fixture.registerRoot(Registry);
  optimize(*M, Registry);
  EXPECT_EQ(markedLoads(*M), 1U);
}

TEST(EJitPointerPeriod, ReportsStrictCandidateRejectionReasons) {
#ifndef EJIT_DIAG_ENABLE
  GTEST_SKIP() << "requires EJIT diagnostics";
#else
  const char *Reasons[] = {"unknown-bounds", "stale-pointer-base",
                           "missing-entry-contract", "access-out-of-bounds",
                           "layout-or-field-unauthorized"};
  for (unsigned Case = 0; Case < std::size(Reasons); ++Case) {
    SCOPED_TRACE(Reasons[Case]);
    PointerFixture Fixture(5);
    std::vector<uint8_t> Other(Fixture.Data.size());
    LLVMContext Ctx;
    auto M = Fixture.module(Ctx, true, false, Case != 4, Case != 2, false, 1,
                            "add i16 0, 3");
    ASSERT_NE(M, nullptr);
    PeriodArrayRegistry Registry;
    Fixture.registerRoot(Registry);
    if (Case)
      ASSERT_TRUE(Registry.registerPointerPeriodBounds(
          "cfg", Case == 1 ? Other.data() : Fixture.Pointer,
          Case == 3 ? Fixture.Stride : Fixture.Data.size()));
    const auto OldLevel = ejit_get_log_level();
    ejit_set_log_level(EJIT_LOG_VERBOSE);
    testing::internal::CaptureStdout();
    optimize(*M, Registry);
    std::string Output = testing::internal::GetCapturedStdout();
    ejit_set_log_level(OldLevel);
    EXPECT_EQ(markedLoads(*M), 1U);
    EXPECT_NE(Output.find(std::string("reason=") + Reasons[Case]),
              std::string::npos)
        << Output;
    EXPECT_NE(Output.find("candidate rejected fn=probe gv=cfg period=cell"),
              std::string::npos)
        << Output;
  }
#endif
}

TEST(EJitPointerPeriod,
     ConstantDiscoveryExecutesRealTier1AndTier2WithLiveSlots) {
  for (auto Level :
       {ejit::OptimizationLevel::L2, ejit::OptimizationLevel::L3}) {
    SCOPED_TRACE(static_cast<unsigned>(Level));
    PointerFixture Fixture(10);
    AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
    LLVMContext Ctx;
    auto M =
        Fixture.module(Ctx, true, false, true, true, false, 1, "add i16 0, 3");
    ASSERT_NE(M, nullptr);
    useLiveSlotStores(*M, Fixture);
    ASSERT_FALSE(verifyModule(*M, &errs()));
    EJitRuntimeState State;
    auto &Registry = State.getRegistry();
    Fixture.registerRoot(Registry);
    Registry.setAllocationSizeLookup({lookupAllocationSize, &Record});
    Config Cfg;
    Cfg.optLevel = Level;
    auto EngineOrErr = EJitOrcEngine::Create(Cfg, Registry, State);
    ASSERT_TRUE(static_cast<bool>(EngineOrErr));
    auto Engine = std::move(*EngineOrErr);
    auto JTMB = orc::JITTargetMachineBuilder::detectHost();
    ASSERT_TRUE(static_cast<bool>(JTMB));
    auto NativeLayout = JTMB->getDefaultDataLayoutForTarget();
    ASSERT_TRUE(static_cast<bool>(NativeLayout));
    M->setDataLayout(*NativeLayout);
    std::string Bitcode;
    raw_string_ostream OS(Bitcode);
    WriteBitcodeToFile(*M, OS);
    OS.flush();
    SpecializationContext Context;
    Context.fnName = "probe";
    Context.cacheKey = 1;
    Context.dimensions.push_back({"cell", 1});
    Context.optLevel = Level;
    Context.tier = CompileTier::Instrumented;
    Engine->setActiveContext(&Context);
    if (auto Err = Engine->loadBitcodeModule(Bitcode, 1, "probe"))
      FAIL() << toString(std::move(Err));
    auto Tier1 = Engine->lookup(1, "probe");
    ASSERT_TRUE(static_cast<bool>(Tier1));
#ifdef EJIT_SRE_CODE_POOL
    if (auto Err = Engine->flushPendingCode())
      FAIL() << toString(std::move(Err));
#endif
    auto Profc = Engine->lookup(1, "__profc_probe");
    auto Profd = Engine->lookup(1, "__profd_probe");
    ASSERT_TRUE(static_cast<bool>(Profc));
    ASSERT_TRUE(static_cast<bool>(Profd));
    using Probe = uint8_t (*)(uint8_t, uint16_t);
    for (uint16_t SlotNo = 0; SlotNo < 64; ++SlotNo)
      EXPECT_EQ(reinterpret_cast<Probe>(*Tier1)(1, SlotNo), 1U);

    PgoCounterRef Ref{"probe", reinterpret_cast<uintptr_t>(*Profc),
                      reinterpret_cast<uintptr_t>(*Profd)};
    Context.profileData = synthesizeProfileBuffer({Ref});
    ASSERT_FALSE(Context.profileData.empty());
    Context.tier = CompileTier::PGOUse;
    if (auto Err = Engine->loadBitcodeModule(Bitcode, 1, "probe"))
      FAIL() << toString(std::move(Err));
    auto Tier2 = Engine->lookup(1, "probe");
    ASSERT_TRUE(static_cast<bool>(Tier2));
#ifdef EJIT_SRE_CODE_POOL
    if (auto Err = Engine->flushPendingCode())
      FAIL() << toString(std::move(Err));
#endif
    for (uint16_t SlotNo = 64; SlotNo < 96; ++SlotNo)
      EXPECT_EQ(reinterpret_cast<Probe>(*Tier2)(1, SlotNo), 1U);
    for (unsigned I = 0; I < Fixture.Slots; ++I) {
      EXPECT_EQ(Fixture.Data[Fixture.Stride + 8 + I * 4],
                96 / Fixture.Slots + (I < 96 % Fixture.Slots));
      EXPECT_EQ(Fixture.Data[Fixture.Stride + 8 + I * 4 + 1], 1U);
      EXPECT_EQ(Fixture.Data[8 + I * 4], 0U);
    }

    auto Optimized = CloneModule(*M);
    EJitOptimizer Optimizer(Registry);
    Optimizer.runPipeline(*Optimized, Context);
    EXPECT_FALSE(verifyModule(*Optimized, &errs()));
    EXPECT_EQ(markedLoads(*Optimized), 0U);
    auto Entries = Optimized->getFunction("probe")->getEntryCount();
    ASSERT_TRUE(Entries.has_value());
    EXPECT_EQ(Entries->getCount(), 64U);
    Engine->setActiveContext(nullptr);
  }
}

static void runOnlinePgoLifecycle(bool Lightweight,
                                  unsigned WitnessElement = 0) {
#ifndef EJIT_SRE_SHARED_TASKPOOL
  GTEST_SKIP() << "requires the shared taskpool runtime";
#else
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  PointerFixture Fixture(10);
  AllocationRecord Record{Fixture.Pointer, Fixture.Data.size()};
  LLVMContext Ctx;
  auto M = Fixture.module(Ctx, !Lightweight, Lightweight, true, true, false, 1,
                          Lightweight ? "" : "add i16 0, 3");
  ASSERT_NE(M, nullptr);
  const unsigned Element = Lightweight ? WitnessElement : 1;
  if (Lightweight && Element == 0)
    selectElementZero(*M, Fixture);
  useLiveSlotStores(*M, Fixture, Element);
  auto JTMB = orc::JITTargetMachineBuilder::detectHost();
  ASSERT_TRUE(static_cast<bool>(JTMB));
  auto Layout = JTMB->getDefaultDataLayoutForTarget();
  ASSERT_TRUE(static_cast<bool>(Layout));
  M->setDataLayout(*Layout);
  std::string Bitcode;
  raw_string_ostream OS(Bitcode);
  WriteBitcodeToFile(*M, OS);
  OS.flush();

  auto Reset = [] {
    ejit_shutdown();
    EJitLifecycleRegistry::instance().reset();
    EJitFuncRegistry::instance().reset();
    EJitRegistrationStore::instance().consume();
    EJitRegistrationStore::instance().consumeError();
  };
  Reset();
  struct RuntimeCleanup {
    decltype(Reset) &ResetState;
    ~RuntimeCleanup() { ResetState(); }
  } Cleanup{Reset};
  uint32_t CellDim = kEJitInvalidDimType, Func = 0;
  ejit_register_lifecycle("cell", &CellDim);
  ejit_register_funcindex("probe", &Func);
  ASSERT_NE(CellDim, kEJitInvalidDimType);
  auto &Store = EJitRegistrationStore::instance();
  Store.registerBitcode("probe",
                        reinterpret_cast<const uint8_t *>(Bitcode.data()),
                        Bitcode.size());
  Store.registerPeriodArray("cell", "cfg", &Fixture.Pointer, 0);
  if (!Lightweight)
    ASSERT_EQ(ejit_set_allocation_size_lookup(lookupAllocationSize, &Record),
              EJIT_OK);
  ASSERT_EQ(ejit_init_pgo(nullptr), EJIT_OK);
  const ejit_dim_pair_t Dim{CellDim, 1};
  using Probe = uint8_t (*)(uint8_t, uint16_t);
  uint16_t SlotNo = 0;
  void *PreviousTier2 = nullptr;
  for (uint8_t Value : {1, 2}) {
    SCOPED_TRACE(static_cast<unsigned>(Value));
    // No invocation or compiler request is outstanding at the update point.
    // The previous round reached T2, and every read token was released.
    for (unsigned I = 0; I < Fixture.Slots; ++I)
      Fixture.Data[Element * Fixture.Stride + 8 + I * 4 + 1] = Value;
    ASSERT_EQ(ejit_activate("cell", 1), EJIT_OK);
    void *Tier1 = nullptr, *Tier2 = nullptr;
    unsigned Tier1Executions = 0;
    const auto Deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!Tier2 && std::chrono::steady_clock::now() < Deadline) {
      void *Fn = nullptr;
      uint32_t Bucket = 0;
      auto Status = ejit_taskpool_compile_or_get(Func, &Dim, 1, &Fn, &Bucket);
      if (Status == EJIT_OK && Fn) {
        uint8_t Actual = reinterpret_cast<Probe>(Fn)(1, SlotNo++);
        ejit_taskpool_release_read(Bucket);
        ASSERT_EQ(Actual, Value);
        ASSERT_NE(Fn, PreviousTier2);
        if (!Tier1)
          Tier1 = Fn;
        if (Fn == Tier1)
          ++Tier1Executions;
        else
          Tier2 = Fn;
      } else {
        ASSERT_EQ(Status, EJIT_PENDING);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_NE(Tier1, nullptr);
    ASSERT_NE(Tier2, nullptr)
        << "online PGO did not publish T2 before deadline";
    EXPECT_EQ(Tier1Executions, 64U);
    PreviousTier2 = Tier2;
    ASSERT_EQ(ejit_deactivate("cell", 1), EJIT_OK);
    void *Disabled = nullptr;
    uint32_t Bucket = 0;
    EXPECT_EQ(ejit_taskpool_compile_or_get(Func, &Dim, 1, &Disabled, &Bucket),
              EJIT_ERR_INSTANCE_DISABLED);
    EXPECT_EQ(Disabled, nullptr);
  }
  for (unsigned I = 0; I < Fixture.Slots; ++I) {
    EXPECT_EQ(Fixture.Data[Element * Fixture.Stride + 8 + I * 4],
              SlotNo / Fixture.Slots + (I < SlotNo % Fixture.Slots));
    EXPECT_EQ(Fixture.Data[Element * Fixture.Stride + 8 + I * 4 + 1], 2U);
    EXPECT_EQ(Fixture.Data[(1 - Element) * Fixture.Stride + 8 + I * 4], 0U);
  }
#endif
}

TEST(EJitPointerPeriod, OnlinePgoRecompilesAfterLifecycleUpdate) {
  runOnlinePgoLifecycle(false);
}

TEST(EJitPointerPeriod, LightweightOnlinePgoRecompilesWithoutBoundsOrLookup) {
  runOnlinePgoLifecycle(true);
}

TEST(EJitPointerPeriod, LightweightNonzeroOnlinePgoRecompilesWithoutBounds) {
  runOnlinePgoLifecycle(true, 1);
}
