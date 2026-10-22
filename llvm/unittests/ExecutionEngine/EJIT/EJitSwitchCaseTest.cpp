//===-- EJitSwitchCaseTest.cpp - ejit_runtime_dim switch-case arms --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stage 2 of jit_design_doc/EJIT_SWITCH_CASE.md: projection detection, key
// selection, region cloning and the eager path. IR-level tests drive PASS6 and
// the switch-case step directly; the execution tests compile through the real
// engine pipeline and call the result.
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_SWITCH_CASE

#include "llvm/ExecutionEngine/EJIT/EJitSwitchCase.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/ProfileData/InstrProfWriter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace llvm;
using namespace llvm::ejit;

namespace {

constexpr const char *kDataLayout =
    "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128";

// The design doc's §1.1 example. Layout must match %CellCfg below.
struct SlotCfg {
  uint32_t shift, scale, mode;
};
struct CellCfg {
  uint32_t len;
  int32_t coef[4];
  int32_t clip;
  SlotCfg slot[3];
};
static_assert(sizeof(CellCfg) == 60, "layout must match %CellCfg");

/// process() from §1.1, roughly as clang -O2 leaves it. The projection and its
/// zext sit in the entry block, above part A's loop, so the address chain has
/// to be recomputed at the switch point; the scale load is conditional, so the
/// switch point is the latest point dominating both it and the shift load.
constexpr const char *kProcessIR = R"(
%SlotCfg = type { i32, i32, i32 }
%CellCfg = type { i32, [4 x i32], i32, [3 x %SlotCfg] }
@g_cellCfg = external global [8 x %CellCfg], !ejit.metadata !0

define i32 @process(i8 %cell, i32 %slot, ptr %in) !ejit.metadata !2 {
entry:
  %c = zext i8 %cell to i64
  %k = urem i32 %slot, 3
  %kz = zext i32 %k to i64
  %lenp = getelementptr inbounds [8 x %CellCfg], ptr @g_cellCfg, i64 0, i64 %c, i32 0
  %len = load i32, ptr %lenp, align 4, !ejit.may_const !9
  %empty = icmp eq i32 %len, 0
  br i1 %empty, label %partb, label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %inp = getelementptr inbounds i16, ptr %in, i64 %i
  %x = load i16, ptr %inp, align 2
  %xw = sext i16 %x to i32
  %coefp = getelementptr inbounds [8 x %CellCfg], ptr @g_cellCfg, i64 0, i64 %c, i32 1, i64 %i
  %coef = load i32, ptr %coefp, align 4, !ejit.may_const !9
  %m = mul nsw i32 %xw, %coef
  %acc.next = add nsw i32 %acc, %m
  %i.next = add nuw nsw i64 %i, 1
  %len64 = zext i32 %len to i64
  %done = icmp eq i64 %i.next, %len64
  br i1 %done, label %partb, label %loop

partb:
  %accA = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %clipp = getelementptr inbounds [8 x %CellCfg], ptr @g_cellCfg, i64 0, i64 %c, i32 2
  %clip = load i32, ptr %clipp, align 4, !ejit.may_const !9
  %over = icmp sgt i32 %accA, %clip
  %accC = select i1 %over, i32 %clip, i32 %accA
  %shp = getelementptr inbounds [8 x %CellCfg], ptr @g_cellCfg, i64 0, i64 %c, i32 3, i64 %kz, i32 0
  %sh = load i32, ptr %shp, align 4, !ejit.may_const !9
  %r = ashr i32 %accC, %sh
  %modep = getelementptr inbounds [8 x %CellCfg], ptr @g_cellCfg, i64 0, i64 %c, i32 3, i64 %kz, i32 2
  %mode = load i32, ptr %modep, align 4, !ejit.may_const !9
  %on = icmp ne i32 %mode, 0
  br i1 %on, label %scale, label %out

scale:
  %scp = getelementptr inbounds [8 x %CellCfg], ptr @g_cellCfg, i64 0, i64 %c, i32 3, i64 %kz, i32 1
  %sc = load i32, ptr %scp, align 4, !ejit.may_const !9
  %rs = mul i32 %r, %sc
  br label %out

out:
  %rr = phi i32 [ %r, %partb ], [ %rs, %scale ]
  %ret = add i32 %rr, %slot
  ret i32 %ret
}

!0 = !{!1}
!1 = !{!"ejit_period_arr", !"cell", i32 8}
!2 = distinct !{!3, !4, !5}
!3 = !{!"ejit_entry"}
!4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
!5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
!9 = !{!"ejit"}
)";

/// Cell 3 as §1.2 describes it.
void fillCells(CellCfg (&Cells)[8]) {
  std::memset(Cells, 0, sizeof(Cells));
  for (unsigned C = 0; C < 8; ++C) {
    Cells[C].len = 1;
    Cells[C].coef[0] = 1;
    Cells[C].clip = 1 << 20;
  }
  CellCfg &C3 = Cells[3];
  C3.len = 4;
  C3.coef[0] = 1;
  C3.coef[1] = -2;
  C3.coef[2] = 3;
  C3.coef[3] = 1;
  C3.clip = 1000;
  C3.slot[0] = {2, 3, 1};
  C3.slot[1] = {0, 1, 0};
  C3.slot[2] = {4, 5, 1};
}

int32_t processReference(const CellCfg &C, uint32_t Slot, const int16_t *In) {
  int32_t Acc = 0;
  for (uint32_t I = 0; I < C.len; ++I)
    Acc += In[I] * C.coef[I];
  if (Acc > C.clip)
    Acc = C.clip;
  const SlotCfg &S = C.slot[Slot % 3];
  int32_t R = Acc >> S.shift;
  if (S.mode)
    R *= S.scale;
  return R + static_cast<int32_t>(Slot);
}

/// \p ForEngine leaves the data layout empty: the engine refuses a module
/// whose layout differs from the JIT's, and fills an empty one in itself.
std::unique_ptr<Module> parse(StringRef IR, LLVMContext &Ctx,
                              bool ForEngine = false) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(IR, Err, Ctx);
  EXPECT_TRUE(M) << Err.getMessage().str();
  if (M && !ForEngine)
    M->setDataLayout(kDataLayout);
  return M;
}

/// Analysis managers for driving passes by hand.
struct Analyses {
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  Analyses() {
    PassBuilder PB;
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerModuleAnalyses(MAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  }
};

void runInstCombine(Module &M, Analyses &A) {
  FunctionPassManager FPM;
  FPM.addPass(InstCombinePass());
  for (Function &F : M)
    if (!F.isDeclaration())
      FPM.run(F, A.FAM);
}

/// Phases 1a-1c as runPipeline runs them, then the switch-case step: the cell
/// argument (always argument 0 here, when \p Cell is set) is replaced by its
/// instance, InstCombine folds it, and PASS6 substitutes what it can.
EJitSwitchCaseResult specializeAndSwitch(
    Module &M, StringRef Entry, PeriodArrayRegistry &Reg,
    std::optional<uint8_t> Cell, Analyses &A,
    EJitSwitchCaseLimits Limits = EJitSwitchCaseLimits::fromBuild(),
    ArrayRef<EJitBoundPointerView> Views = {},
    const EJitSwitchCaseDecision *Replay = nullptr) {
  Function *F = M.getFunction(Entry);
  EXPECT_NE(F, nullptr);
  if (Cell)
    F->getArg(0)->replaceAllUsesWith(
        ConstantInt::get(F->getArg(0)->getType(), *Cell));
  runInstCombine(M, A);
  EJitStructFieldPass Pass(Reg, Views, Entry);
  Pass.initFromModule(M);
  for (Function &G : M)
    if (!G.isDeclaration())
      Pass.run(G, A.FAM);
  return runSwitchCase(*F, Pass, A.FAM, Limits, Replay);
}

/// Phases 1e-1f: fold the arms' constant keys and substitute their loads.
void foldArms(Module &M, StringRef Entry, PeriodArrayRegistry &Reg, Analyses &A,
              ArrayRef<EJitBoundPointerView> Views = {}) {
  runInstCombine(M, A);
  EJitStructFieldPass Pass(Reg, Views, Entry);
  Pass.initFromModule(M);
  for (Function &G : M)
    if (!G.isDeclaration())
      Pass.run(G, A.FAM);
}

unsigned countMayConstLoads(const Function &F) {
  unsigned N = 0;
  for (const Instruction &I : instructions(F))
    if (auto *LI = dyn_cast<LoadInst>(&I))
      N += LI->hasMetadata(MD_EJIT_MAY_CONST);
  return N;
}

SwitchInst *findDispatch(Function &F) {
  for (Instruction &I : instructions(F))
    if (auto *SI = dyn_cast<SwitchInst>(&I))
      if (SI->getCondition()->getName().starts_with("ejit.sc.key"))
        return SI;
  return nullptr;
}

constexpr uint64_t descriptor(unsigned Width, uint64_t Divisor) {
  return static_cast<uint64_t>(EJitProjectionOp::URem) |
         (static_cast<uint64_t>(Width) << 8) | (Divisor << 32);
}

//===----------------------------------------------------------------------===//
// IR-level: the transformation
//===----------------------------------------------------------------------===//

TEST(EJitSwitchCase, EagerArmsForModulusProjection) {
  LLVMContext Ctx;
  auto M = parse(kProcessIR, Ctx);
  ASSERT_TRUE(M);
  CellCfg Cells[8];
  fillCells(Cells);
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_cellCfg", Cells, 8);
  Analyses A;

  auto R = specializeAndSwitch(*M, "process", Reg, /*Cell=*/3, A);
  ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager) << R.declined;
  EXPECT_EQ(R.projection, descriptor(32, 3));
  EXPECT_EQ(R.domain, 3u);
  EXPECT_EQ(R.sites, 3u) << "shift, mode and the conditional scale load";
  EXPECT_EQ(R.keptKeys, (SmallVector<uint32_t, 8>{0, 1, 2}));

  Function &F = *M->getFunction("process");
  ASSERT_FALSE(verifyFunction(F, &errs()));

  SwitchInst *Dispatch = findDispatch(F);
  ASSERT_NE(Dispatch, nullptr) << "no switch on the key";
  EXPECT_EQ(Dispatch->getNumCases(), 3u);

  // Part A is before the switch point: its loop is never cloned.
  for (BasicBlock &BB : F)
    EXPECT_FALSE(BB.getName().starts_with("loop.sc")) << BB.getName().str();

  // §11 rule 1: the parameter is never substituted. Every copy of
  // `r + slotNo`, in the three arms and the default, still adds the argument.
  Argument *Slot = F.getArg(1);
  unsigned LiveAdds = 0;
  for (User *U : Slot->users())
    if (auto *BO = dyn_cast<BinaryOperator>(U))
      LiveAdds += BO->getOpcode() == Instruction::Add;
  EXPECT_EQ(LiveAdds, 4u);

  // Phase 1f folds every arm; only the default arm keeps generic loads.
  const unsigned Before = countMayConstLoads(F);
  foldArms(*M, "process", Reg, A);
  ASSERT_FALSE(verifyFunction(F, &errs()));
  EXPECT_EQ(countMayConstLoads(F), Before - 3 * R.sites)
      << "an arm kept a key-dependent load";
}

TEST(EJitSwitchCase, DescriptorsDistinguishModuli) {
  // g_arr[15 + slot % C]: every key of both domains lands inside the array.
  auto Run = [](unsigned Divisor) {
    std::string IR = R"(
      @g_arr = external global [8 x [20 x i32]], !ejit.metadata !0
      define i32 @mod(i8 %cell, i32 %slot) !ejit.metadata !2 {
        %c = zext i8 %cell to i64
        %k = urem i32 %slot, )" +
                     std::to_string(Divisor) + R"(
        %idx = add nuw nsw i32 %k, 15
        %iz = zext i32 %idx to i64
        %p = getelementptr inbounds [8 x [20 x i32]], ptr @g_arr, i64 0, i64 %c, i64 %iz
        %v = load i32, ptr %p, align 4, !ejit.may_const !9
        ret i32 %v
      }
      !0 = !{!1}
      !1 = !{!"ejit_period_arr", !"cell", i32 8}
      !2 = distinct !{!3, !4, !5}
      !3 = !{!"ejit_entry"}
      !4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
      !5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
      !9 = !{!"ejit"}
    )";
    LLVMContext Ctx;
    auto M = parse(IR, Ctx);
    static int32_t Arr[8][20];
    PeriodArrayRegistry Reg;
    Reg.registerArray("cell", "g_arr", Arr, 8);
    Analyses A;
    return specializeAndSwitch(*M, "mod", Reg, /*Cell=*/1, A);
  };
  auto R3 = Run(3), R5 = Run(5);
  ASSERT_EQ(R3.path, EJitSwitchCaseResult::Path::Eager) << R3.declined;
  ASSERT_EQ(R5.path, EJitSwitchCaseResult::Path::Eager) << R5.declined;
  EXPECT_EQ(R3.projection, descriptor(32, 3));
  EXPECT_EQ(R5.projection, descriptor(32, 5));
  EXPECT_NE(R3.projection, R5.projection);
  EXPECT_EQ(R5.keptKeys, (SmallVector<uint32_t, 8>{0, 1, 2, 3, 4}));
}

/// `% numSlots`, with numSlots a may_const field: PASS6 folds the divisor
/// before detection looks (§4.1).
TEST(EJitSwitchCase, ConfigFieldModulus) {
  LLVMContext Ctx;
  auto M = parse(R"(
    @g_cfg = external global [8 x { i32, [6 x i32] }], !ejit.metadata !0
    define i32 @cfgmod(i8 %cell, i32 %slot) !ejit.metadata !2 {
      %c = zext i8 %cell to i64
      %np = getelementptr inbounds [8 x { i32, [6 x i32] }], ptr @g_cfg, i64 0, i64 %c, i32 0
      %n = load i32, ptr %np, align 4, !ejit.may_const !9
      %k = urem i32 %slot, %n
      %kz = zext i32 %k to i64
      %p = getelementptr inbounds [8 x { i32, [6 x i32] }], ptr @g_cfg, i64 0, i64 %c, i32 1, i64 %kz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      %r = add i32 %v, %slot
      ret i32 %r
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 8}
    !2 = distinct !{!3, !4, !5}
    !3 = !{!"ejit_entry"}
    !4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
    !9 = !{!"ejit"}
  )",
                 Ctx);
  ASSERT_TRUE(M);
  struct Cfg {
    uint32_t n;
    int32_t v[6];
  } Cfgs[8] = {};
  Cfgs[2].n = 4;
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_cfg", Cfgs, 8);
  Analyses A;
  auto R = specializeAndSwitch(*M, "cfgmod", Reg, /*Cell=*/2, A);
  ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager) << R.declined;
  EXPECT_EQ(R.projection, descriptor(32, 4));
  EXPECT_EQ(R.keptKeys.size(), 4u);
}

/// srem, a mask that is not a low-bit mask and a plain index are the identity
/// row of §4.1: valid, but only for the lazy path, so the eager-only build
/// declines them.
TEST(EJitSwitchCase, NonModulusProjectionsAreIdentity) {
  for (const char *Proj :
       {"srem i32 %slot, 3", "and i32 %slot, 6", "add i32 %slot, 0"}) {
    std::string IR = std::string(R"(
      @g_arr = external global [8 x [4 x i32]], !ejit.metadata !0
      define i32 @ident(i8 %cell, i32 %slot) !ejit.metadata !2 {
        %c = zext i8 %cell to i64
        %k = )") + Proj +
                     R"(
        %kz = sext i32 %k to i64
        %p = getelementptr inbounds [8 x [4 x i32]], ptr @g_arr, i64 0, i64 %c, i64 %kz
        %v = load i32, ptr %p, align 4, !ejit.may_const !9
        ret i32 %v
      }
      !0 = !{!1}
      !1 = !{!"ejit_period_arr", !"cell", i32 8}
      !2 = distinct !{!3, !4, !5}
      !3 = !{!"ejit_entry"}
      !4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
      !5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
      !9 = !{!"ejit"}
    )";
    LLVMContext Ctx;
    auto M = parse(IR, Ctx);
    ASSERT_TRUE(M);
    static int32_t Arr[8][4];
    PeriodArrayRegistry Reg;
    Reg.registerArray("cell", "g_arr", Arr, 8);
    Analyses A;
    auto R = specializeAndSwitch(*M, "ident", Reg, /*Cell=*/0, A);
    EXPECT_EQ(R.path, EJitSwitchCaseResult::Path::None) << Proj;
    EXPECT_EQ(R.declined, "projection-identity-lazy-not-implemented") << Proj;
    EXPECT_EQ(findDispatch(*M->getFunction("ident")), nullptr) << Proj;
  }
}

/// A power-of-two modulus reaches the JIT as a mask: InstCombine rewrites
/// `% 4` to `& 3` (the AOT -O2 pipeline already has). Both spellings are the
/// modulus row, with one descriptor, and the dispatch uses the mask.
TEST(EJitSwitchCase, LowBitMaskIsTheModulus) {
  SmallVector<uint64_t, 2> Descriptors;
  for (const char *Proj : {"urem i32 %slot, 4", "and i32 %slot, 3"}) {
    std::string IR = std::string(R"(
      @g_arr = external global [8 x [4 x i32]], !ejit.metadata !0
      define i32 @pow2(i8 %cell, i32 %slot) !ejit.metadata !2 {
        %c = zext i8 %cell to i64
        %k = )") + Proj +
                     R"(
        %kz = zext i32 %k to i64
        %p = getelementptr inbounds [8 x [4 x i32]], ptr @g_arr, i64 0, i64 %c, i64 %kz
        %v = load i32, ptr %p, align 4, !ejit.may_const !9
        %r = add i32 %v, %slot
        ret i32 %r
      }
      !0 = !{!1}
      !1 = !{!"ejit_period_arr", !"cell", i32 8}
      !2 = distinct !{!3, !4, !5}
      !3 = !{!"ejit_entry"}
      !4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
      !5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
      !9 = !{!"ejit"}
    )";
    LLVMContext Ctx;
    auto M = parse(IR, Ctx);
    ASSERT_TRUE(M);
    static int32_t Arr[8][4];
    PeriodArrayRegistry Reg;
    Reg.registerArray("cell", "g_arr", Arr, 8);
    Analyses A;
    auto R = specializeAndSwitch(*M, "pow2", Reg, /*Cell=*/2, A);
    ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager)
        << Proj << ": " << R.declined;
    EXPECT_EQ(R.domain, 4u) << Proj;
    EXPECT_EQ(R.keptKeys, (SmallVector<uint32_t, 8>{0, 1, 2, 3})) << Proj;
    Descriptors.push_back(R.projection);

    Function &F = *M->getFunction("pow2");
    ASSERT_FALSE(verifyFunction(F, &errs()));
    SwitchInst *Dispatch = findDispatch(F);
    ASSERT_NE(Dispatch, nullptr) << Proj;
    auto *Key = dyn_cast<BinaryOperator>(Dispatch->getCondition());
    ASSERT_NE(Key, nullptr);
    EXPECT_EQ(Key->getOpcode(), Instruction::And) << Proj;
    foldArms(*M, "pow2", Reg, A);
    EXPECT_EQ(countMayConstLoads(F), 1u)
        << Proj << ": only the default arm keeps the load";
  }
  ASSERT_EQ(Descriptors.size(), 2u);
  EXPECT_EQ(Descriptors[0], descriptor(32, 4));
  EXPECT_EQ(Descriptors[1], descriptor(32, 4));
}

/// A mask that keeps every bit of the parameter changes nothing: identity.
TEST(EJitSwitchCase, FullWidthMaskIsIdentity) {
  LLVMContext Ctx;
  auto M = parse(R"(
    @g_arr = external global [256 x i32], !ejit.metadata !0
    define i32 @full(i8 %slot) !ejit.metadata !2 {
      %w = zext i8 %slot to i32
      %k = and i32 %w, 255
      %kz = zext i32 %k to i64
      %p = getelementptr inbounds [256 x i32], ptr @g_arr, i64 0, i64 %kz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      ret i32 %v
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 256}
    !2 = distinct !{!3, !5}
    !3 = !{!"ejit_entry"}
    !5 = !{!"ejit_runtime_dim", !"", i32 0, i32 0}
    !9 = !{!"ejit"}
  )",
                 Ctx);
  ASSERT_TRUE(M);
  static int32_t Arr[256];
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_arr", Arr, 256);
  Analyses A;
  auto R = specializeAndSwitch(*M, "full", Reg, std::nullopt, A);
  EXPECT_EQ(R.path, EJitSwitchCaseResult::Path::None);
  EXPECT_EQ(R.declined, "projection-identity-lazy-not-implemented");
}

/// Sites only in a non-inlined helper share no dominator with the entry.
TEST(EJitSwitchCase, HelperOnlySitesAreDeclined) {
  LLVMContext Ctx;
  auto M = parse(R"(
    @g_arr = external global [8 x [3 x i32]], !ejit.metadata !0
    define i32 @outer(i8 %cell, i32 %slot) !ejit.metadata !2 {
      %r = call i32 @helper(i8 %cell, i32 %slot)
      ret i32 %r
    }
    define internal i32 @helper(i8 %cell, i32 %slot) noinline {
      %c = zext i8 %cell to i64
      %k = urem i32 %slot, 3
      %kz = zext i32 %k to i64
      %p = getelementptr inbounds [8 x [3 x i32]], ptr @g_arr, i64 0, i64 %c, i64 %kz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      ret i32 %v
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 8}
    !2 = distinct !{!3, !4, !5}
    !3 = !{!"ejit_entry"}
    !4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
    !9 = !{!"ejit"}
  )",
                 Ctx);
  ASSERT_TRUE(M);
  static int32_t Arr[8][3];
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_arr", Arr, 8);
  Analyses A;
  auto R = specializeAndSwitch(*M, "outer", Reg, /*Cell=*/0, A);
  EXPECT_EQ(R.path, EJitSwitchCaseResult::Path::None);
  EXPECT_EQ(R.declined, "no-entry-local-site");
}

/// g_arr[17 + slot % 4] on a 20-element row: key 3 names an address outside
/// the object, resolves nothing, and gets no arm (§4.3 step 3).
TEST(EJitSwitchCase, KeyResolvingNoSiteIsNotCloned) {
  LLVMContext Ctx;
  auto M = parse(R"(
    @g_arr = external global [20 x i32], !ejit.metadata !0
    define i32 @edge(i32 %slot) !ejit.metadata !2 {
      %k = urem i32 %slot, 4
      %idx = add nuw nsw i32 %k, 17
      %iz = zext i32 %idx to i64
      %p = getelementptr inbounds [20 x i32], ptr @g_arr, i64 0, i64 %iz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      ret i32 %v
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 20}
    !2 = distinct !{!3, !5}
    !3 = !{!"ejit_entry"}
    !5 = !{!"ejit_runtime_dim", !"", i32 0, i32 0}
    !9 = !{!"ejit"}
  )",
                 Ctx);
  ASSERT_TRUE(M);
  static int32_t Arr[20];
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_arr", Arr, 20);
  Analyses A;
  // A 0-dim entry: its single identity gets the arms.
  auto R = specializeAndSwitch(*M, "edge", Reg, std::nullopt, A);
  ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager) << R.declined;
  EXPECT_EQ(R.keptKeys, (SmallVector<uint32_t, 8>{0, 1, 2}));
  SwitchInst *Dispatch = findDispatch(*M->getFunction("edge"));
  ASSERT_NE(Dispatch, nullptr);
  EXPECT_EQ(Dispatch->getNumCases(), 3u);
}

TEST(EJitSwitchCase, DomainOverArmLimitIsDeclined) {
  LLVMContext Ctx;
  auto M = parse(kProcessIR, Ctx);
  ASSERT_TRUE(M);
  CellCfg Cells[8];
  fillCells(Cells);
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_cellCfg", Cells, 8);
  Analyses A;
  EJitSwitchCaseLimits Limits = EJitSwitchCaseLimits::fromBuild();
  Limits.maxArms = 2;
  auto R = specializeAndSwitch(*M, "process", Reg, 3, A, Limits);
  EXPECT_EQ(R.declined, "domain-exceeds-arms-lazy-not-implemented");

  // The per-entry ejit_runtime_dim(n) wins over the build default.
  LLVMContext Ctx2;
  std::string IR = kProcessIR;
  IR.replace(IR.find("i32 1, i32 0}"), 13, "i32 1, i32 3}");
  auto M2 = parse(IR, Ctx2);
  ASSERT_TRUE(M2);
  Analyses A2;
  auto R2 = specializeAndSwitch(*M2, "process", Reg, 3, A2, Limits);
  EXPECT_EQ(R2.path, EJitSwitchCaseResult::Path::Eager) << R2.declined;
}

TEST(EJitSwitchCase, RegionAndCloneLimitsDecline) {
  for (bool Region : {true, false}) {
    LLVMContext Ctx;
    auto M = parse(kProcessIR, Ctx);
    ASSERT_TRUE(M);
    CellCfg Cells[8];
    fillCells(Cells);
    PeriodArrayRegistry Reg;
    Reg.registerArray("cell", "g_cellCfg", Cells, 8);
    Analyses A;
    EJitSwitchCaseLimits Limits = EJitSwitchCaseLimits::fromBuild();
    (Region ? Limits.maxRegion : Limits.maxCloned) = 4;
    auto R = specializeAndSwitch(*M, "process", Reg, 3, A, Limits);
    EXPECT_EQ(R.declined, Region ? "region-over-limit" : "cloned-over-limit");
    EXPECT_EQ(findDispatch(*M->getFunction("process")), nullptr)
        << "a declined entry must be left untouched";
  }
}

/// An ejit_bound_ptr entry gets eager arms: the compile already carries the
/// call's descriptor (§5.2).
TEST(EJitSwitchCase, BoundPointerEntryGetsEagerArms) {
  LLVMContext Ctx;
  auto M = parse(R"(
    define i32 @bound(i8 %cell, i32 %slot, ptr %cfg) !ejit.metadata !0 {
      %k = urem i32 %slot, 3
      %kz = zext i32 %k to i64
      %p = getelementptr inbounds [3 x i32], ptr %cfg, i64 0, i64 %kz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      %r = add i32 %v, %slot
      ret i32 %r
    }
    !0 = distinct !{!1, !2, !3, !4}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !3 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
    !4 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 12, !5, !6, !7}
    !5 = !{i64 0, i64 4}
    !6 = !{i64 4, i64 4}
    !7 = !{i64 8, i64 4}
    !9 = !{!"ejit"}
  )",
                 Ctx);
  ASSERT_TRUE(M);
  uint32_t Object[3] = {10, 20, 30};
  SmallVector<EJitBoundPointerView, 1> Views{
      {reinterpret_cast<const uint8_t *>(Object), sizeof(Object), 2, 1}};
  PeriodArrayRegistry Reg;
  Analyses A;
  auto R = specializeAndSwitch(*M, "bound", Reg, /*Cell=*/1, A,
                               EJitSwitchCaseLimits::fromBuild(), Views);
  ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager) << R.declined;
  EXPECT_EQ(R.keptKeys, (SmallVector<uint32_t, 8>{0, 1, 2}));
  foldArms(*M, "bound", Reg, A, Views);
  EXPECT_EQ(countMayConstLoads(*M->getFunction("bound")), 1u)
      << "only the default arm keeps the load";
}

/// A bound field is recognized by its offset. Only bytes [4, 8) are may_const
/// and the load carries no !ejit.may_const, so it is a candidate at key 1
/// only: key 0 reaching an unmarked field must not hide the site.
TEST(EJitSwitchCase, BoundPointerSiteAtNonzeroKeyOnly) {
  LLVMContext Ctx;
  auto M = parse(R"(
    define i32 @bound1(i8 %cell, i32 %slot, ptr %cfg) !ejit.metadata !0 {
      %k = urem i32 %slot, 3
      %kz = zext i32 %k to i64
      %p = getelementptr inbounds [3 x i32], ptr %cfg, i64 0, i64 %kz
      %v = load i32, ptr %p, align 4
      %r = add i32 %v, %slot
      ret i32 %r
    }
    !0 = distinct !{!1, !2, !3, !4}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !3 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
    !4 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 12, !5}
    !5 = !{i64 4, i64 4}
  )",
                 Ctx);
  ASSERT_TRUE(M);
  uint32_t Object[3] = {10, 20, 30};
  SmallVector<EJitBoundPointerView, 1> Views{
      {reinterpret_cast<const uint8_t *>(Object), sizeof(Object), 2, 1}};
  PeriodArrayRegistry Reg;
  Analyses A;
  auto R = specializeAndSwitch(*M, "bound1", Reg, /*Cell=*/1, A,
                               EJitSwitchCaseLimits::fromBuild(), Views);
  ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager) << R.declined;
  EXPECT_EQ(R.sites, 1u);
  EXPECT_EQ(R.keptKeys, (SmallVector<uint32_t, 8>{1}));

  Function &F = *M->getFunction("bound1");
  foldArms(*M, "bound1", Reg, A, Views);
  ASSERT_FALSE(verifyFunction(F, &errs()));
  unsigned Loads = 0;
  for (Instruction &I : instructions(F))
    Loads += isa<LoadInst>(I);
  EXPECT_EQ(Loads, 1u) << "arm 1 folds field [4, 8); only the default loads";
}

/// P is decided by sites alone. `input[slot % 5]` is an ordinary load: its
/// projection must not mix with the may_const site's `% 3`.
TEST(EJitSwitchCase, NonSiteProjectionsDoNotMix) {
  LLVMContext Ctx;
  auto M = parse(R"(
    @g_arr = external global [3 x i32], !ejit.metadata !0
    define i32 @mix(i32 %slot, ptr %input) !ejit.metadata !2 {
      %k = urem i32 %slot, 3
      %kz = zext i32 %k to i64
      %p = getelementptr inbounds [3 x i32], ptr @g_arr, i64 0, i64 %kz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      %j = urem i32 %slot, 5
      %jz = zext i32 %j to i64
      %q = getelementptr inbounds i32, ptr %input, i64 %jz
      %w = load i32, ptr %q, align 4
      %r = add i32 %v, %w
      ret i32 %r
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 3}
    !2 = distinct !{!3, !5}
    !3 = !{!"ejit_entry"}
    !5 = !{!"ejit_runtime_dim", !"", i32 0, i32 0}
    !9 = !{!"ejit"}
  )",
                 Ctx);
  ASSERT_TRUE(M);
  static int32_t Arr[3];
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_arr", Arr, 3);
  Analyses A;
  auto R = specializeAndSwitch(*M, "mix", Reg, std::nullopt, A);
  ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager) << R.declined;
  EXPECT_EQ(R.sites, 1u);
  EXPECT_EQ(R.projection, descriptor(32, 3));
  EXPECT_EQ(R.keptKeys, (SmallVector<uint32_t, 8>{0, 1, 2}));
  ASSERT_FALSE(verifyFunction(*M->getFunction("mix"), &errs()));
}

/// `% 5` and `% 10` sites have no single P (§4.2 rule 2).
TEST(EJitSwitchCase, MixedModuliAreDeclined) {
  LLVMContext Ctx;
  auto M = parse(R"(
    @g_a = external global [10 x i32], !ejit.metadata !0
    define i32 @two(i32 %slot) !ejit.metadata !2 {
      %j = urem i32 %slot, 5
      %jz = zext i32 %j to i64
      %p = getelementptr inbounds [10 x i32], ptr @g_a, i64 0, i64 %jz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      %k = urem i32 %slot, 10
      %kz = zext i32 %k to i64
      %q = getelementptr inbounds [10 x i32], ptr @g_a, i64 0, i64 %kz
      %w = load i32, ptr %q, align 4, !ejit.may_const !9
      %r = add i32 %v, %w
      ret i32 %r
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 10}
    !2 = distinct !{!3, !5}
    !3 = !{!"ejit_entry"}
    !5 = !{!"ejit_runtime_dim", !"", i32 0, i32 10}
    !9 = !{!"ejit"}
  )",
                 Ctx);
  ASSERT_TRUE(M);
  static int32_t Arr[10];
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_a", Arr, 10);
  Analyses A;
  auto R = specializeAndSwitch(*M, "two", Reg, std::nullopt, A);
  EXPECT_EQ(R.path, EJitSwitchCaseResult::Path::None);
  EXPECT_EQ(R.declined, "mixed-projections");
  EXPECT_EQ(findDispatch(*M->getFunction("two")), nullptr);
}

/// Two sites on `slot % 3`: g_tab[k] and g_tab[freeze(vec[k]) & 3], whose
/// index is also stored above the switch. For slot = 2 the extract is poison;
/// a copy of the freeze in arm 2 would fold on its own and disagree with the
/// stored index.
constexpr const char *kFrozenIndexIR = R"(
    @g_tab = external global [1 x [4 x i32]], !ejit.metadata !0
    define i32 @frozen(i8 %cell, i32 noundef %slot, ptr %vp, i1 noundef %flag,
                       ptr %obs) !ejit.metadata !2 {
    entry:
      %c = zext i8 %cell to i64
      %vec = load <2 x i32>, ptr %vp, align 8, !noundef !10
      %k = urem i32 %slot, 3
      %e = extractelement <2 x i32> %vec, i32 %k
      %fr = freeze i32 %e
      %idx = and i32 %fr, 3
      store i32 %idx, ptr %obs, align 4
      br i1 %flag, label %plain, label %masked
    plain:
      %kz = zext i32 %k to i64
      %p = getelementptr inbounds [1 x [4 x i32]], ptr @g_tab, i64 0, i64 %c, i64 %kz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      ret i32 %v
    masked:
      %iz = zext i32 %idx to i64
      %q = getelementptr inbounds [1 x [4 x i32]], ptr @g_tab, i64 0, i64 %c, i64 %iz
      %w = load i32, ptr %q, align 4, !ejit.may_const !9
      ret i32 %w
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 1}
    !2 = distinct !{!3, !4, !5}
    !3 = !{!"ejit_entry"}
    !4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
    !9 = !{!"ejit"}
    !10 = !{}
)";

TEST(EJitSwitchCase, FreezeInAddressChainIsNotRematerialized) {
  static int32_t Tab[1][4] = {{10, 20, 30, 40}};
  LLVMContext Ctx;
  auto M = parse(kFrozenIndexIR, Ctx);
  ASSERT_TRUE(M);
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_tab", Tab, 1);
  Analyses A;
  auto R = specializeAndSwitch(*M, "frozen", Reg, /*Cell=*/0, A);
  EXPECT_EQ(R.sites, 2u);
  EXPECT_EQ(R.projection, descriptor(32, 3)) << "both sites agree on P";
  EXPECT_EQ(R.path, EJitSwitchCaseResult::Path::None);
  EXPECT_EQ(R.declined, "address-chain-not-rematerializable");
  Function &F = *M->getFunction("frozen");
  EXPECT_EQ(findDispatch(F), nullptr) << "a declined entry must be untouched";
  unsigned Freezes = 0;
  for (Instruction &I : instructions(F))
    Freezes += isa<FreezeInst>(I);
  EXPECT_EQ(Freezes, 1u) << "the freeze was copied";
}

/// Convergent and noduplicate calls decline the region; ordinary calls are the
/// control. The call-site-only case is indirect because InstCombine drops
/// `convergent` from a direct call to a non-convergent callee.
TEST(EJitSwitchCase, ConvergentCallInRegionIsDeclined) {
  constexpr const char *IR = R"(
    @g_arr = external global [3 x i32], !ejit.metadata !0
    declare i32 @communicate(i32) ATTRS
    define i32 @conv(i32 %slot, ptr %fp) !ejit.metadata !2 {
      %k = urem i32 %slot, 3
      %kz = zext i32 %k to i64
      %p = getelementptr inbounds [3 x i32], ptr @g_arr, i64 0, i64 %kz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      %r = call i32 CALLEE(i32 %v) SITE
      ret i32 %r
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 3}
    !2 = distinct !{!3, !5}
    !3 = !{!"ejit_entry"}
    !5 = !{!"ejit_runtime_dim", !"", i32 0, i32 0}
    !9 = !{!"ejit"}
  )";
  struct Case {
    const char *Callee, *Decl, *Site, *Declined;
  };
  for (Case C : {Case{"@communicate", "", "", ""},
                 Case{"%fp", "", "", ""},
                 Case{"@communicate", "convergent", "", "region-convergent-call"},
                 Case{"%fp", "", "convergent", "region-convergent-call"},
                 Case{"@communicate", "noduplicate", "",
                      "region-noduplicate-call"}}) {
    std::string Src = IR;
    Src.replace(Src.find("ATTRS"), 5, C.Decl);
    Src.replace(Src.find("CALLEE"), 6, C.Callee);
    Src.replace(Src.find("SITE"), 4, C.Site);
    LLVMContext Ctx;
    auto M = parse(Src, Ctx);
    ASSERT_TRUE(M);
    static int32_t Arr[3];
    PeriodArrayRegistry Reg;
    Reg.registerArray("cell", "g_arr", Arr, 3);
    Analyses A;
    auto R = specializeAndSwitch(*M, "conv", Reg, std::nullopt, A);
    Function &F = *M->getFunction("conv");
    unsigned Calls = 0;
    for (Instruction &I : instructions(F))
      Calls += isa<CallInst>(I);
    if (!*C.Declined) {
      EXPECT_EQ(R.path, EJitSwitchCaseResult::Path::Eager)
          << C.Callee << ": " << R.declined;
      EXPECT_EQ(Calls, 4u) << "three arms and the default";
      continue;
    }
    EXPECT_EQ(R.declined, C.Declined) << C.Callee << " " << C.Decl << C.Site;
    EXPECT_EQ(findDispatch(F), nullptr);
    EXPECT_EQ(Calls, 1u) << "the call was cloned";
  }
}

/// Tier-2 rebuilds exactly Tier-1's arms, or none, and declines a replay that
/// does not fit (§10).
TEST(EJitSwitchCase, ReplayRebuildsExactlyTier1Arms) {
  CellCfg Cells[8];
  fillCells(Cells);
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_cellCfg", Cells, 8);
  auto Run = [&](const EJitSwitchCaseDecision &Replay, LLVMContext &Ctx,
                 std::unique_ptr<Module> &M) {
    M = parse(kProcessIR, Ctx);
    Analyses A;
    return specializeAndSwitch(*M, "process", Reg, /*Cell=*/3, A,
                               EJitSwitchCaseLimits::fromBuild(), {}, &Replay);
  };

  EJitSwitchCaseDecision Subset{true, descriptor(32, 3), {0, 2}};
  LLVMContext Ctx1;
  std::unique_ptr<Module> M1;
  auto R1 = Run(Subset, Ctx1, M1);
  ASSERT_EQ(R1.path, EJitSwitchCaseResult::Path::Eager) << R1.declined;
  EXPECT_EQ(R1.keptKeys, (SmallVector<uint32_t, 8>{0, 2}));
  SwitchInst *Dispatch = findDispatch(*M1->getFunction("process"));
  ASSERT_NE(Dispatch, nullptr);
  EXPECT_EQ(Dispatch->getNumCases(), 2u);
  ASSERT_FALSE(verifyFunction(*M1->getFunction("process"), &errs()));

  EJitSwitchCaseDecision None{true, 0, {}};
  LLVMContext Ctx2;
  std::unique_ptr<Module> M2;
  auto R2 = Run(None, Ctx2, M2);
  EXPECT_EQ(R2.declined, "pgo-replay-no-arms");
  EXPECT_EQ(findDispatch(*M2->getFunction("process")), nullptr);

  for (EJitSwitchCaseDecision Bad :
       {EJitSwitchCaseDecision{true, descriptor(32, 5), {0, 1}},
        EJitSwitchCaseDecision{true, descriptor(32, 3), {0, 3}}}) {
    LLVMContext Ctx3;
    std::unique_ptr<Module> M3;
    auto R3 = Run(Bad, Ctx3, M3);
    EXPECT_EQ(R3.declined, "pgo-replay-mismatch");
    EXPECT_EQ(findDispatch(*M3->getFunction("process")), nullptr);
  }
}

//===----------------------------------------------------------------------===//
// Execution: through the engine pipeline, then called
//===----------------------------------------------------------------------===//

std::string toBitcode(Module &M) {
  std::string Bitcode;
  raw_string_ostream OS(Bitcode);
  WriteBitcodeToFile(M, OS);
  OS.flush();
  return Bitcode;
}

/// Compile \p Entry for cell \p Cell through the full engine pipeline, with
/// \p Symbols resolved to the test's own objects. Returns null on failure.
void *compile(EJitOrcEngine &Engine, const std::string &Bitcode,
              const char *Entry, std::optional<uint8_t> Cell, uint64_t Key) {
  SpecializationContext SCtx;
  SCtx.fnName = Entry;
  SCtx.cacheKey = Key;
  if (Cell)
    SCtx.dimensions.push_back({"cell", *Cell});
  Engine.setActiveContext(&SCtx);
  if (Error E = Engine.loadBitcodeModule(Bitcode, Key, Entry)) {
    ADD_FAILURE() << toString(std::move(E));
    Engine.setActiveContext(nullptr);
    return nullptr;
  }
  auto FnOrErr = Engine.lookup(Key, Entry);
  Engine.setActiveContext(nullptr);
  if (!FnOrErr) {
    ADD_FAILURE() << toString(FnOrErr.takeError());
    return nullptr;
  }
  return reinterpret_cast<void *>(*FnOrErr);
}

/// The stage 2 gate: slotNo = 5 reaches arm 2 and still returns r + 5.
/// After compiling, the slot data is overwritten; every slot still returns the
/// compiled-in configuration, which shows all three keys run an arm.
TEST(EJitSwitchCaseExec, ProcessMatchesReference) {
  static CellCfg Cells[8];
  fillCells(Cells);
  const CellCfg Compiled = Cells[3];

  LLVMContext Ctx;
  auto M = parse(kProcessIR, Ctx, /*ForEngine=*/true);
  ASSERT_TRUE(M);
  const std::string Bitcode = toBitcode(*M);

  EJitRuntimeState State;
  State.getRegistry().registerArray("cell", "g_cellCfg", Cells, 8);
  Config Cfg;
  auto EngineOrErr = EJitOrcEngine::Create(Cfg, State.getRegistry(), State);
  ASSERT_TRUE(static_cast<bool>(EngineOrErr));
  auto Engine = std::move(*EngineOrErr);
  Engine->addUserSymbol("g_cellCfg", Cells);

  using FnTy = int32_t (*)(uint8_t, uint32_t, const int16_t *);
  auto Fn = reinterpret_cast<FnTy>(
      compile(*Engine, Bitcode, "process", /*Cell=*/3, 0x5c01));
  ASSERT_NE(Fn, nullptr);

  const int16_t In[4] = {100, 20, 30, 7};
  for (uint32_t Slot : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 1000000u})
    EXPECT_EQ(Fn(3, Slot, In), processReference(Compiled, Slot, In))
        << "slot " << Slot;
  // acc = 100 - 40 + 90 + 7 = 157; arm 2: (157 >> 4) * 5 = 45; + 5.
  EXPECT_EQ(Fn(3, 5, In), 50);

  for (SlotCfg &S : Cells[3].slot)
    S = {1, 7, 1};
  for (uint32_t Slot : {0u, 1u, 2u, 5u})
    EXPECT_EQ(Fn(3, Slot, In), processReference(Compiled, Slot, In))
        << "slot " << Slot << " read live data: it ran the default arm";
}

/// A key-dependent load behind a condition inside a loop.
constexpr const char *kLoopyIR = R"(
    @g_tab = external global [4 x [3 x i32]], !ejit.metadata !0
    define i32 @loopy(i8 %cell, i32 %slot, i32 %n) !ejit.metadata !2 {
    entry:
      %c = zext i8 %cell to i64
      %k = urem i32 %slot, 3
      %kz = zext i32 %k to i64
      %pos = icmp sgt i32 %n, 0
      br i1 %pos, label %ph, label %exit
    ph:
      br label %loop
    loop:
      %i = phi i32 [ 0, %ph ], [ %i.n, %latch ]
      %acc = phi i32 [ 0, %ph ], [ %acc.n, %latch ]
      %odd = and i32 %i, 1
      %isodd = icmp ne i32 %odd, 0
      br i1 %isodd, label %get, label %latch
    get:
      %p = getelementptr inbounds [4 x [3 x i32]], ptr @g_tab, i64 0, i64 %c, i64 %kz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      br label %latch
    latch:
      %a = phi i32 [ %v, %get ], [ 1, %loop ]
      %acc.n = add i32 %acc, %a
      %i.n = add nuw nsw i32 %i, 1
      %more = icmp slt i32 %i.n, %n
      br i1 %more, label %loop, label %exit
    exit:
      %s = phi i32 [ 0, %entry ], [ %acc.n, %latch ]
      %r = add i32 %s, %slot
      ret i32 %r
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 4}
    !2 = distinct !{!3, !4, !5}
    !3 = !{!"ejit_entry"}
    !4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
    !9 = !{!"ejit"}
)";

/// The switch point moves to the loop's preheader (§4.3), so P is computed
/// even when the loop runs zero times, which must not change the result.
TEST(EJitSwitchCaseExec, LoopSiteHoistedToPreheader) {
  static int32_t Tab[4][3] = {{0}, {5, 6, 7}, {0}, {0}};

  // IR level: the switch point is the preheader, outside the loop.
  {
    LLVMContext Ctx;
    auto M = parse(kLoopyIR, Ctx);
    ASSERT_TRUE(M);
    PeriodArrayRegistry Reg;
    Reg.registerArray("cell", "g_tab", Tab, 4);
    Analyses A;
    auto R = specializeAndSwitch(*M, "loopy", Reg, /*Cell=*/1, A);
    ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager) << R.declined;
    SwitchInst *Dispatch = findDispatch(*M->getFunction("loopy"));
    ASSERT_NE(Dispatch, nullptr);
    EXPECT_EQ(Dispatch->getParent()->getName(), "ph");
    ASSERT_FALSE(verifyFunction(*M->getFunction("loopy"), &errs()));
  }

  LLVMContext Ctx;
  auto M = parse(kLoopyIR, Ctx, /*ForEngine=*/true);
  ASSERT_TRUE(M);
  const std::string Bitcode = toBitcode(*M);
  EJitRuntimeState State;
  State.getRegistry().registerArray("cell", "g_tab", Tab, 4);
  Config Cfg;
  auto EngineOrErr = EJitOrcEngine::Create(Cfg, State.getRegistry(), State);
  ASSERT_TRUE(static_cast<bool>(EngineOrErr));
  auto Engine = std::move(*EngineOrErr);
  Engine->addUserSymbol("g_tab", Tab);

  using FnTy = int32_t (*)(uint8_t, uint32_t, int32_t);
  auto Fn = reinterpret_cast<FnTy>(
      compile(*Engine, Bitcode, "loopy", /*Cell=*/1, 0x5c02));
  ASSERT_NE(Fn, nullptr);

  auto Reference = [](uint32_t Slot, int32_t N) {
    int32_t Acc = 0;
    for (int32_t I = 0; I < N; ++I)
      Acc += (I & 1) ? Tab[1][Slot % 3] : 1;
    return Acc + static_cast<int32_t>(Slot);
  };
  for (uint32_t Slot : {0u, 1u, 2u, 4u, 11u})
    for (int32_t N : {0, 1, 2, 5, -3})
      EXPECT_EQ(Fn(1, Slot, N), Reference(Slot, N))
          << "slot " << Slot << " n " << N;
}

/// The switch in the preheader runs even when the loop never reaches the
/// load. With slot = poison and n = 1 the source returns 1 without using the
/// key, so the dispatch must branch on a frozen parameter, not on poison.
/// A noundef parameter needs no freeze.
TEST(EJitSwitchCase, DispatchFreezesAMaybePoisonParameter) {
  static int32_t Tab[4][3] = {{0}, {5, 6, 7}, {0}, {0}};
  std::string IR = kLoopyIR;
  IR.replace(IR.find("add i32 %s, %slot"), 17, "add i32 %s, 0");
  for (bool NoUndef : {false, true}) {
    std::string Src = IR;
    if (NoUndef)
      Src.replace(Src.find("i32 %slot, i32 %n"), 17,
                  "i32 noundef %slot, i32 %n");
    LLVMContext Ctx;
    auto M = parse(Src, Ctx);
    ASSERT_TRUE(M);
    PeriodArrayRegistry Reg;
    Reg.registerArray("cell", "g_tab", Tab, 4);
    Analyses A;
    auto R = specializeAndSwitch(*M, "loopy", Reg, /*Cell=*/1, A);
    ASSERT_EQ(R.path, EJitSwitchCaseResult::Path::Eager) << R.declined;

    Function &F = *M->getFunction("loopy");
    ASSERT_FALSE(verifyFunction(F, &errs()));
    SwitchInst *Dispatch = findDispatch(F);
    ASSERT_NE(Dispatch, nullptr);
    Value *KeyArg = cast<Instruction>(Dispatch->getCondition())->getOperand(0);
    if (NoUndef) {
      EXPECT_EQ(KeyArg, F.getArg(1)) << "a noundef parameter needs no freeze";
      continue;
    }
    auto *Frozen = dyn_cast<FreezeInst>(KeyArg);
    ASSERT_NE(Frozen, nullptr) << "the dispatch branches on the raw parameter";
    EXPECT_EQ(Frozen->getOperand(0), F.getArg(1));

    // The case itself: O2 must still return 1, not prove the path UB. The
    // entry metadata goes first, or O2's EJIT AOT passes wrap the function.
    F.getArg(1)->replaceAllUsesWith(PoisonValue::get(F.getArg(1)->getType()));
    F.getArg(2)->replaceAllUsesWith(
        ConstantInt::get(F.getArg(2)->getType(), 1));
    F.setMetadata(MD_EJIT_METADATA, nullptr);
    PassBuilder PB;
    ModulePassManager MPM =
        PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    MPM.run(*M, A.MAM);
    ASSERT_FALSE(verifyFunction(F, &errs()));
    auto *Ret = dyn_cast<ReturnInst>(F.getEntryBlock().getTerminator());
    ASSERT_NE(Ret, nullptr) << "control flow survived O2";
    auto *C = dyn_cast<ConstantInt>(Ret->getReturnValue());
    ASSERT_NE(C, nullptr);
    EXPECT_EQ(C->getSExtValue(), 1);
  }
}

/// Keys without an arm run the default arm, which reads live data. Index
/// `slot % 4 < 2 ? slot % 4 : g_idx`: keys 0 and 1 resolve, keys 2 and 3 reach
/// an ordinary global and are not kept. The divisor is a power of two, so this
/// also runs the mask dispatch.
TEST(EJitSwitchCaseExec, UnkeptKeysRunTheDefaultArm) {
  static int32_t Tab[2][4] = {{0}, {10, 11, 12, 13}};
  static int32_t Idx = 3;
  LLVMContext Ctx;
  auto M = parse(R"(
    @g_tab4 = external global [2 x [4 x i32]], !ejit.metadata !0
    @g_idx = external global i32
    define i32 @dflt(i8 %cell, i32 %slot) !ejit.metadata !2 {
      %c = zext i8 %cell to i64
      %k = urem i32 %slot, 4
      %lo = icmp ult i32 %k, 2
      %x = load i32, ptr @g_idx, align 4
      %ix = select i1 %lo, i32 %k, i32 %x
      %iz = zext i32 %ix to i64
      %p = getelementptr inbounds [2 x [4 x i32]], ptr @g_tab4, i64 0, i64 %c, i64 %iz
      %v = load i32, ptr %p, align 4, !ejit.may_const !9
      %r = add i32 %v, %slot
      ret i32 %r
    }
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 2}
    !2 = distinct !{!3, !4, !5}
    !3 = !{!"ejit_entry"}
    !4 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !5 = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
    !9 = !{!"ejit"}
  )",
                 Ctx, /*ForEngine=*/true);
  ASSERT_TRUE(M);
  const std::string Bitcode = toBitcode(*M);

  EJitRuntimeState State;
  State.getRegistry().registerArray("cell", "g_tab4", Tab, 2);
  Config Cfg;
  auto EngineOrErr = EJitOrcEngine::Create(Cfg, State.getRegistry(), State);
  ASSERT_TRUE(static_cast<bool>(EngineOrErr));
  auto Engine = std::move(*EngineOrErr);
  Engine->addUserSymbol("g_tab4", Tab);
  Engine->addUserSymbol("g_idx", &Idx);

  using FnTy = int32_t (*)(uint8_t, uint32_t);
  auto Fn = reinterpret_cast<FnTy>(
      compile(*Engine, Bitcode, "dflt", /*Cell=*/1, 0x5c03));
  ASSERT_NE(Fn, nullptr);

  // Change the data after compiling: arms keep what was compiled in, the
  // default arm sees the new values.
  for (int32_t &V : Tab[1])
    V += 100;
  EXPECT_EQ(Fn(1, 0), 10 + 0);
  EXPECT_EQ(Fn(1, 5), 11 + 5);
  EXPECT_EQ(Fn(1, 2), 113 + 2) << "key 2 has no arm; default reads g_idx";
  EXPECT_EQ(Fn(1, 7), 113 + 7);
}

/// Whatever the freeze picks, the masked path returns the entry at the stored
/// index. Slot 2 is the poison lane.
TEST(EJitSwitchCaseExec, FrozenIndexStoreMatchesReturn) {
  static int32_t Tab[1][4] = {{10, 20, 30, 40}};
  LLVMContext Ctx;
  auto M = parse(kFrozenIndexIR, Ctx, /*ForEngine=*/true);
  ASSERT_TRUE(M);
  const std::string Bitcode = toBitcode(*M);

  EJitRuntimeState State;
  State.getRegistry().registerArray("cell", "g_tab", Tab, 1);
  Config Cfg;
  auto EngineOrErr = EJitOrcEngine::Create(Cfg, State.getRegistry(), State);
  ASSERT_TRUE(static_cast<bool>(EngineOrErr));
  auto Engine = std::move(*EngineOrErr);
  Engine->addUserSymbol("g_tab", Tab);

  using FnTy = int32_t (*)(uint8_t, uint32_t, const int32_t *, bool, int32_t *);
  auto Fn = reinterpret_cast<FnTy>(
      compile(*Engine, Bitcode, "frozen", /*Cell=*/0, 0x5c04));
  ASSERT_NE(Fn, nullptr);

  // Nonzero low bits: an arm folding its own freeze to 0 would return 10.
  alignas(8) const int32_t Vec[2] = {5, 6};
  for (uint32_t Slot : {0u, 1u, 2u, 3u, 4u, 5u}) {
    int32_t Obs = -1;
    int32_t Got = Fn(0, Slot, Vec, /*Flag=*/false, &Obs);
    ASSERT_GE(Obs, 0);
    ASSERT_LT(Obs, 4);
    EXPECT_EQ(Got, Tab[0][Obs]) << "slot " << Slot << ": returned an entry "
                                << "other than the one at the stored index";
    if (Slot % 3 != 2)
      EXPECT_EQ(Obs, Vec[Slot % 3] & 3) << "slot " << Slot;
    EXPECT_EQ(Fn(0, Slot, Vec, /*Flag=*/true, &Obs), Tab[0][Slot % 3]);
  }
}

//===----------------------------------------------------------------------===//
// Online PGO: Tier-1 Gen and Tier-2 Use around the arms (§10)
//===----------------------------------------------------------------------===//

/// The CFG hash and counter count Tier-1's PGO Gen recorded for \p Name.
bool readProfileShape(const Module &M, const std::string &Name, uint64_t &Hash,
                      unsigned &NumCounters) {
  const GlobalVariable *Profd = M.getGlobalVariable("__profd_" + Name, true);
  const GlobalVariable *Profc = M.getGlobalVariable("__profc_" + Name, true);
  if (!Profd || !Profc)
    return false;
  auto *Init = dyn_cast<ConstantStruct>(Profd->getInitializer());
  if (!Init || Init->getNumOperands() < 2)
    return false;
  auto *H = dyn_cast<ConstantInt>(Init->getOperand(1));
  if (!H)
    return false;
  Hash = H->getZExtValue();
  NumCounters = cast<ArrayType>(Profc->getValueType())->getNumElements();
  return true;
}

/// One all-zero record. PGO Use sets the entry count only if the CFG hash
/// matches, so the entry count shows whether Tier-2 saw Tier-1's CFG.
std::string zeroProfile(const std::string &Name, uint64_t Hash,
                        unsigned NumCounters) {
  InstrProfWriter Writer;
  consumeError(Writer.mergeProfileKind(InstrProfKind::IRInstrumentation));
  Writer.addRecord(NamedInstrProfRecord(Name, Hash,
                                        std::vector<uint64_t>(NumCounters, 0)),
                   1, [](Error E) { consumeError(std::move(E)); });
  auto Buf = Writer.writeBuffer();
  return Buf ? Buf->getBuffer().str() : std::string();
}

/// Replaying Tier-1's arms keeps the CFG hash, so Tier-2 consumes the profile;
/// other arms would not.
TEST(EJitSwitchCasePgo, Tier2ReplaysTier1ArmsAndConsumesItsProfile) {
  CellCfg Cells[8];
  fillCells(Cells);
  PeriodArrayRegistry Reg;
  Reg.registerArray("cell", "g_cellCfg", Cells, 8);
  EJitOptimizer Opt(Reg);

  LLVMContext Ctx;
  // PGO Use reports a mismatch as a warning; keep it from aborting the test.
  Ctx.setDiagnosticHandlerCallBack(
      [](const DiagnosticInfo *, void *) {}, nullptr);
  auto M0 = parse(kProcessIR, Ctx);
  ASSERT_TRUE(M0);
  auto Spec = [](CompileTier Tier) {
    SpecializationContext S;
    S.fnName = "process";
    S.cacheKey = 0x5c10;
    S.dimensions.push_back({"cell", 3});
    S.tier = Tier;
    return S;
  };

  auto M1 = CloneModule(*M0);
  Opt.runPipeline(*M1, Spec(CompileTier::Instrumented));
  const EJitSwitchCaseDecision T1 = Opt.getLastSwitchCase();
  ASSERT_TRUE(T1.valid);
  EXPECT_EQ(T1.projection, descriptor(32, 3));
  EXPECT_EQ(T1.keys, (SmallVector<uint32_t, 8>{0, 1, 2}));
  uint64_t Hash = 0;
  unsigned NumCounters = 0;
  ASSERT_TRUE(readProfileShape(*M1, "process", Hash, NumCounters));
  const std::string Profile = zeroProfile("process", Hash, NumCounters);
  ASSERT_FALSE(Profile.empty());

  auto Use = [&](const EJitSwitchCaseDecision &Replay) {
    Opt.clearAnalyses();
    auto M2 = CloneModule(*M0);
    SpecializationContext S = Spec(CompileTier::PGOUse);
    S.profileData = Profile;
    S.switchCaseReplay = Replay;
    Opt.runPipeline(*M2, S);
    return M2->getFunction("process")->getEntryCount().has_value();
  };

  EXPECT_TRUE(Use(T1)) << "Tier-2's CFG differs from Tier-1's";
  EXPECT_EQ(Opt.getLastSwitchCase().keys, T1.keys);

  EJitSwitchCaseDecision Other{true, descriptor(32, 3), {0}};
  EXPECT_FALSE(Use(Other)) << "the profile matched a different CFG";
  EXPECT_EQ(Opt.getLastSwitchCase().keys, Other.keys);
}

} // namespace

#endif // EJIT_SWITCH_CASE
