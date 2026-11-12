//===-- EJitPhiWitnessTest.cpp - Equal PHI field witnesses
//-----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <array>
#include <sstream>

using namespace llvm;
using namespace llvm::ejit;

namespace {
enum RootKind { ArrayRoot, BoundedPointer, UnboundedPointer, SymbolicStatic };
enum Scenario {
  Equal,
  Different,
  UnmarkedInput,
  UnmarkedSlot,
  PoisonInput,
  OutOfBounds,
  MissingField,
  MissingRegistration,
  MissingContract,
  Volatile,
  Atomic,
  MalformedMarker,
  Misaligned,
  Cycle,
  TooManyEdges,
  Correlated,
  CorrelatedDifferent,
  ConstantEdges
};

struct Field {
  unsigned Bits, Offset, Align;
};
constexpr Field Fields[] = {{64, 0, 64}, {32, 12, 4}, {8, 21, 1}, {8, 22, 2}};

static void checkPhi(Field Fld, RootKind Root, Scenario Case, bool BigEndian,
                     ejit::OptimizationLevel Level, CompileTier Tier,
                     bool FullPipeline = true) {
  SCOPED_TRACE(::testing::Message()
               << "bits=" << Fld.Bits << " off=" << Fld.Offset
               << " root=" << Root << " case=" << Case << " BE=" << BigEndian
               << " level=" << unsigned(Level) << " tier=" << unsigned(Tier)
               << " pipeline=" << FullPipeline);
  unsigned Slots = Fld.Bits == 64 ? 1 : 5;
  unsigned RecordSize = Fld.Bits == 64 ? 64 : 1240;
  unsigned Stride = RecordSize * Slots;
  uint64_t Expected = Fld.Bits == 64 ? 100663296 : Fld.Bits == 32 ? 1234 : 1;
  alignas(64) std::array<uint8_t, 19 * 6200> Data{};
  for (unsigned I = 0; I < 19; ++I)
    for (unsigned S = 0; S < Slots; ++S) {
      uint64_t Value = ((Case == Different && I == 1) ||
                        (Case == CorrelatedDifferent && I == 2))
                           ? 0
                           : Expected;
      for (unsigned B = 0; B < Fld.Bits / 8; ++B) {
        unsigned Shift = BigEndian ? Fld.Bits / 8 - 1 - B : B;
        Data[I * Stride + S * RecordSize + Fld.Offset + B] =
            Value >> (8 * Shift);
      }
    }
  void *Pointer = Data.data();
  bool PointerRoot = Root == BoundedPointer || Root == UnboundedPointer;
  std::ostringstream IR;
  IR << "target datalayout = \"" << (BigEndian ? 'E' : 'e')
     << "-p:64:64-i64:64-n8:16:32:64-S128\"\n";
  IR << (Fld.Bits == 64
             ? "%R = type { i64, [56 x i8] }\n"
             : "%R = type { [12 x i8], i32, [5 x i8], i8, i8, [1217 x i8] }\n");
  IR << "@root = external global ";
  if (PointerRoot)
    IR << "ptr";
  else
    IR << "[18 x [" << Slots << " x %R]]";
  IR << ", !ejit.metadata !0\n"
        "declare i8 @__ejit_local_free_dim_v1_i8(i8) #0\n"
        "declare i16 @__ejit_local_free_dim_v1_i16(i16) #0\n"
        "declare void @consume(i64, i16, ptr)\n"
        "define void @probe(i8 %cell, ptr %ctx, ptr %flags) !ejit.metadata !5 "
        "{ ret void }\n"
        "attributes #0 = { nounwind willreturn memory(none) nocallback nosync ";
  if (Case != MalformedMarker)
    IR << "\"ejit.local_free_dim\"=\"v1\" ";
  IR << "}\n!0 = !{!1";
  unsigned Count = Root == SymbolicStatic ? 18 * Slots : Slots;
  if (Case != MissingField) {
    if (PointerRoot)
      IR << ", !2";
    else
      for (unsigned I = 0; I < Count; ++I)
        IR << ", !" << 100 + I;
  }
  IR << "}\n!1 = !{!\""
     << (Root == SymbolicStatic ? "ejit_period" : "ejit_period_arr")
     << "\", !\"" << (Root == SymbolicStatic ? "static" : "cell") << "\"";
  if (Root != SymbolicStatic)
    IR << ", i32 " << (PointerRoot ? 0 : 18);
  IR << "}\n!2 = !{!\"ejit_pointer_layout\", i64 " << Stride;
  for (unsigned I = 0; I < Slots; ++I)
    IR << ", !" << 100 + I;
  IR << "}\n!5 = distinct !{";
  if (Case != MissingContract)
    IR << "!6, !7";
  IR << "}\n!6 = !{!\"ejit_entry\"}\n"
        "!7 = !{!\"ejit_period_arr_ind\", !\"cell\", i32 0}\n";
  for (unsigned I = 0; I < Count; ++I) {
    IR << "!" << 100 + I << " = !{";
    if (!PointerRoot)
      IR << "!\"ejit_may_const_field\", ";
    IR << "i64 " << I * RecordSize + Fld.Offset;
    if (PointerRoot)
      IR << ", i64 " << Fld.Bits / 8;
    IR << "}\n";
  }
  LLVMContext C;
  SMDiagnostic Error;
  auto M = parseAssemblyString(IR.str(), Error, C);
  ASSERT_NE(M, nullptr) << Error.getMessage().str();
  Function *F = M->getFunction("probe");
  F->getEntryBlock().eraseFromParent();
  IRBuilder<> B(C);
  auto Block = [&](const char *Name) { return BasicBlock::Create(C, Name, F); };
  BasicBlock *Entry = Block("entry"), *Default = Block("default"),
             *Dynamic = Block("dynamic"), *Valid = Block("valid"),
             *Merge = Block("merge"), *Exit = Block("exit");
  B.SetInsertPoint(Entry);
  Value *SlotP = B.CreateGEP(B.getInt8Ty(), F->getArg(1), B.getInt64(20));
  Value *RawSlot = B.CreateAlignedLoad(B.getInt16Ty(), SlotP, Align(4));
  Value *Slot = RawSlot;
  if (Case != UnmarkedSlot)
    Slot = B.CreateCall(M->getFunction("__ejit_local_free_dim_v1_i16"), {Slot});
  Slot = B.CreateZExt(B.CreateURem(Slot, B.getInt16(Slots)), B.getInt64Ty());
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
  Value *Input = Raw;
  if (Case != UnmarkedInput)
    Input = B.CreateCall(M->getFunction("__ejit_local_free_dim_v1_i8"), {Raw});
  Input = B.CreateZExt(Input, B.getInt64Ty());
  if (Case == ConstantEdges)
    Input = B.getInt64(0);
  if (Case == PoisonInput)
    Input = B.CreateSub(Input, B.getInt64(1), "poison", true);
  if (Fld.Bits != 64)
    Input = B.CreateMul(Input, B.getInt64(Stride), "scaled", true, true);
  B.CreateBr(Merge);
  B.SetInsertPoint(Merge);
  auto *Index = B.CreatePHI(B.getInt64Ty(), 2, "choice");
  unsigned Fixed = Case == OutOfBounds ? 18 : 1;
  uint64_t FixedOffset = Fixed * (Fld.Bits == 64 ? 1 : Stride);
  if (Case == Misaligned)
    ++FixedOffset;
  Index->addIncoming(B.getInt64(FixedOffset), Default);
  Index->addIncoming(Input, Valid);
  Value *Offset = Index;
  if (Case == Correlated || Case == CorrelatedDifferent) {
    auto *Other = B.CreatePHI(B.getInt64Ty(), 2, "correlated");
    Other->addIncoming(B.getInt64(0), Default);
    Other->addIncoming(B.getInt64(Fld.Bits == 64 ? 1 : Stride), Valid);
    Offset = B.CreateAdd(Index, Other);
  }
  if (Case == TooManyEdges) {
    // Each extra predecessor is a real CFG edge, not a malformed PHI fixture.
    auto *Old = cast<BranchInst>(Default->getTerminator());
    Old->eraseFromParent();
    IRBuilder<> DB(Default);
    auto *SwitchFlag = DB.CreateAlignedLoad(
        DB.getInt8Ty(),
        DB.CreateGEP(DB.getInt8Ty(), F->getArg(2), DB.getInt64(1)), Align(1));
    auto *Switch = DB.CreateSwitch(SwitchFlag, Merge);
    for (unsigned I = 2; I < 17; ++I) {
      auto *Pred = Block("extra");
      IRBuilder<>(Pred).CreateBr(Merge);
      Switch->addCase(DB.getInt8(I), Pred);
      Index->addIncoming(DB.getInt64(I * (Fld.Bits == 64 ? 1 : Stride)), Pred);
    }
  }
  Value *Base = ConstantExpr::getIntToPtr(
      B.getInt64(reinterpret_cast<uintptr_t>(Data.data())), B.getPtrTy());
  if (Root == SymbolicStatic)
    Base = M->getGlobalVariable("root");
  Type *Record = StructType::getTypeByName(C, "R");
  Value *Ptr = nullptr;
  if (Fld.Bits == 64)
    Ptr = B.CreateInBoundsGEP(Record, Base, Offset);
  else {
    Ptr = B.CreateInBoundsGEP(B.getInt8Ty(), Base, Offset);
    Ptr = B.CreateInBoundsGEP(Record, Ptr, Slot);
    Ptr = B.CreateInBoundsGEP(B.getInt8Ty(), Ptr, B.getInt64(Fld.Offset));
  }
  auto *Load =
      B.CreateAlignedLoad(B.getIntNTy(Fld.Bits), Ptr, Align(Fld.Align));
  Load->setMetadata("ejit.may_const", MDNode::get(C, {}));
  if (Case == Volatile)
    Load->setVolatile(true);
  if (Case == Atomic)
    Load->setAtomic(AtomicOrdering::Monotonic);
  B.CreateCall(M->getFunction("consume"),
               {B.CreateZExtOrTrunc(Load, B.getInt64Ty()), RawSlot, Ptr});
  if (Case == Cycle) {
    auto *Latch = Block("latch");
    B.CreateCondBr(Cond, Latch, Exit);
    B.SetInsertPoint(Latch);
    auto *Next = B.CreateAdd(Index, B.getInt64(1));
    Index->addIncoming(Next, Latch);
    B.CreateBr(Merge);
  } else
    B.CreateBr(Exit);
  B.SetInsertPoint(Exit);
  B.CreateRetVoid();
  ASSERT_FALSE(verifyModule(*M, &errs()));

  PeriodArrayRegistry Registry;
  if (Case != MissingRegistration) {
    if (Root == SymbolicStatic)
      Registry.registerStaticVar("root", Data.data());
    else
      Registry.registerArray("cell", "root",
                             PointerRoot ? static_cast<void *>(&Pointer)
                                         : Data.data(),
                             PointerRoot ? 0 : 18);
  }
  if (Root == BoundedPointer)
    ASSERT_TRUE(
        Registry.registerPointerPeriodBounds("root", Pointer, 18 * Stride));
  if (FullPipeline) {
    SpecializationContext S;
    S.fnName = "probe";
    S.dimensions.push_back({"cell", 0});
    S.optLevel = Level;
    S.tier = Tier;
    EJitOptimizer Optimizer(Registry);
    Optimizer.runPipeline(*M, S);
  } else {
    EJitLocalWitnessPeriod Period{"cell", 0};
    EJitStructFieldPass Pass(Registry, ArrayRef<EJitBoundPointerView>{},
                             "probe", ArrayRef<EJitLocalWitnessPeriod>(Period));
    Pass.initFromModule(*M);
    FunctionAnalysisManager AM;
    Pass.run(*F, AM);
  }
  ASSERT_FALSE(verifyModule(*M, &errs()));
  bool ShouldFold =
      Case == Equal || Case == Correlated || Case == ConstantEdges;
  unsigned Sinks = 0;
  for (Instruction &I : instructions(*F))
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (!CB->getCalledFunction())
        continue;
      if (CB->getCalledFunction()->getName() == "consume") {
        ++Sinks;
        Value *V = CB->getArgOperand(0);
        if (!FullPipeline && Fld.Bits < 64)
          V = cast<Instruction>(V)->getOperand(0);
        auto *K = dyn_cast<ConstantInt>(V);
        EXPECT_EQ(bool(K), ShouldFold);
        if (K)
          EXPECT_EQ(K->getZExtValue(), Expected);
        EXPECT_FALSE(isa<Constant>(CB->getArgOperand(1)));
        EXPECT_FALSE(isa<Constant>(CB->getArgOperand(2)));
      }
      if (FullPipeline && Case != MalformedMarker)
        EXPECT_FALSE(CB->getCalledFunction()->getName().starts_with(
            "__ejit_local_free_dim"));
    }
  EXPECT_EQ(Sinks, 1u);
}
} // namespace

TEST(EJitPhiWitness, EqualFieldsAcrossRootsLayoutsAndTiers) {
  for (RootKind Root :
       {ArrayRoot, BoundedPointer, UnboundedPointer, SymbolicStatic})
    for (Field Fld : Fields)
      for (bool BE : {false, true})
        for (auto Level :
             {ejit::OptimizationLevel::L2, ejit::OptimizationLevel::L3})
          for (auto Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                            CompileTier::PGOUse})
            checkPhi(Fld, Root, Equal, BE, Level, Tier);
}

TEST(EJitPhiWitness, RejectsUnsafeIncomingValues) {
  for (Scenario Case : {Different, UnmarkedInput, UnmarkedSlot, PoisonInput,
                        OutOfBounds, MissingField, MissingRegistration,
                        Volatile, Atomic, MalformedMarker, Misaligned})
    for (Field Fld : Fields) {
      // A 64-byte record with no slot axis cannot exercise these two cases.
      if (Fld.Bits == 64 && (Case == UnmarkedSlot || Case == Misaligned))
        continue;
      for (auto Level :
           {ejit::OptimizationLevel::L2, ejit::OptimizationLevel::L3})
        for (auto Tier : {CompileTier::Baseline, CompileTier::Instrumented,
                          CompileTier::PGOUse})
          checkPhi(Fld, BoundedPointer, Case, false, Level, Tier);
    }
}

TEST(EJitPhiWitness, BoundsEnumerationAndRejectsCycles) {
  for (Scenario Case :
       {Cycle, TooManyEdges, Correlated, CorrelatedDifferent, ConstantEdges})
    for (bool Full : {false, true})
      for (Field Fld : Fields) {
        // Normal CFG simplification can prove edge correlation and eliminate
        // infeasible combinations. Test conservative rejection in the pass.
        if (Case == CorrelatedDifferent && Full)
          continue;
        checkPhi(Fld, ArrayRoot, Case, false, ejit::OptimizationLevel::L2,
                 CompileTier::Baseline, Full);
      }
}

TEST(EJitPhiWitness, RequiresEntryContract) {
  // A non-entry function can be internalized/deleted by the full pipeline.
  for (Field Fld : Fields)
    checkPhi(Fld, BoundedPointer, MissingContract, false,
             ejit::OptimizationLevel::L2, CompileTier::Baseline, false);
}
