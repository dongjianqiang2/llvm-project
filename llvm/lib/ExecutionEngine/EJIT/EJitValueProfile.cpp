//===-- EJitValueProfile.cpp - scalar/loop-bound value profiling ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitValueProfile.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-value-profile"

//===----------------------------------------------------------------------===//
// Site discovery (shared by both modes - the site numbering MUST be identical
// between the Tier-1 instrumented module and the Tier-2 annotated module, and
// both run over the same post-critical-edge-split CFG).
//
// A site is a loop whose single exiting block ends in a conditional branch on
// a direct integer comparison, with EXACTLY ONE operand loop-invariant and
// non-constant: that operand is the observed "bound". Comparison against a
// constant, both-invariant or both-variant operands are skipped (no site).
//===----------------------------------------------------------------------===//
namespace {

struct ScalarSite {
  BranchInst *branch = nullptr;
  Value *bound = nullptr; // loop-invariant, non-constant integer operand
};

/// Recognize the loop bound of a conditional exit branch: a direct integer
/// comparison with EXACTLY ONE operand loop-invariant and non-constant, and
/// exactly one successor inside the loop. Returns null otherwise. Shared by
/// discovery and the transform-time re-derivation (the shapes must agree).
Value *findLoopBound(BranchInst *BI, Loop &L) {
  auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
  if (!Cmp)
    return nullptr;
  // Exactly one successor stays inside the loop (an exit branch).
  if (L.contains(BI->getSuccessor(0)) == L.contains(BI->getSuccessor(1)))
    return nullptr;

  Value *A = Cmp->getOperand(0);
  Value *B = Cmp->getOperand(1);
  bool AInv = L.isLoopInvariant(A);
  bool BInv = L.isLoopInvariant(B);
  if (AInv == BInv)
    return nullptr; // both or neither invariant: not a clean bound
  Value *Bound = AInv ? A : B;
  if (!Bound->getType()->isIntegerTy() || isa<Constant>(Bound))
    return nullptr;
  // Skip pointer-sized-but-non-integer exotic comparisons defensively.
  if (Bound->getType()->getIntegerBitWidth() > 64)
    return nullptr;
  return Bound;
}

std::optional<ScalarSite> findSite(Loop &L) {
  // One site per loop: the single exiting block's conditional branch.
  BasicBlock *Exiting = L.getExitingBlock();
  if (!Exiting)
    return std::nullopt;
  auto *BI = dyn_cast<BranchInst>(Exiting->getTerminator());
  if (!BI || !BI->isConditional())
    return std::nullopt;
  Value *Bound = findLoopBound(BI, L);
  if (!Bound)
    return std::nullopt;
  return ScalarSite{BI, Bound};
}

FunctionCallee getRecordScalarFn(Module &M, bool SessionAware) {
  LLVMContext &Ctx = M.getContext();
  SmallVector<Type *, 4> Args;
  if (SessionAware)
    Args.push_back(Type::getInt64Ty(Ctx));
  Args.push_back(Type::getInt64Ty(Ctx));
  Args.push_back(Type::getInt32Ty(Ctx));
  Args.push_back(Type::getInt64Ty(Ctx));
  auto *FT = FunctionType::get(Type::getVoidTy(Ctx), Args, false);
  return M.getOrInsertFunction(SessionAware ? "ejit_vp_record_scalar_session"
                                            : "ejit_vp_record_scalar",
                               FT);
}

} // namespace

void ejit::runValueProfileOnFunction(
    Function &F, FunctionAnalysisManager &FAM, EJitValueProfileMode Mode,
    function_ref<void(StringRef, uint32_t)> OnFunctionSites,
    uint64_t SamplingSessionId) {
  if (F.isDeclaration())
    return;

  LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
  if (LI.empty())
    return;

  const uint64_t FuncHash = IndexedInstrProf::ComputeHash(getIRPGOFuncName(F));
  Module &M = *F.getParent();
  uint32_t SiteIdx = 0;

  // Deterministic order: loops in preorder as LoopInfo yields them (identical
  // on the Tier-1 and Tier-2 modules built from the same specialized CFG).
  for (Loop *L : LI.getLoopsInPreorder()) {
    std::optional<ScalarSite> S = findSite(*L);
    if (!S)
      continue;

    if (Mode == EJitValueProfileMode::Instrument) {
      // Prefer ONE observation per loop ENTRY (hoisted to the preheader) over
      // one per iteration: the bound is loop-invariant, so the distribution is
      // identical and only the totals scale (~trip-count x cheaper). Fall back
      // to the exit branch when there is no preheader or the bound does not
      // dominate it. The site numbering is unaffected - it follows discovery
      // order, not the call placement - and no CFG edge changes, so the PGO
      // hash stays identical.
      Instruction *InsertPt = S->branch;
      if (BasicBlock *PH = L->getLoopPreheader()) {
        bool DominatesPH = true;
        if (auto *I = dyn_cast<Instruction>(S->bound))
          DominatesPH =
              FAM.getResult<DominatorTreeAnalysis>(F).dominates(I->getParent(),
                                                                PH);
        if (DominatesPH)
          InsertPt = PH->getTerminator();
      }
      FunctionCallee Callee = getRecordScalarFn(M, SamplingSessionId != 0);
      IRBuilder<> B(InsertPt);
      Value *Zext =
          B.CreateZExtOrTrunc(S->bound, B.getInt64Ty(), "ejit.vp.bound");
      SmallVector<Value *, 4> Args;
      if (SamplingSessionId != 0)
        Args.push_back(B.getInt64(SamplingSessionId));
      Args.push_back(B.getInt64(FuncHash));
      Args.push_back(B.getInt32(SiteIdx));
      Args.push_back(Zext);
      B.CreateCall(Callee, Args);
      LLVM_DEBUG(dbgs() << "ejit-value-profile: instrument " << F.getName()
                        << " site=" << SiteIdx
                        << " bound=" << *S->bound << "\n");
    } else {
      // Annotate: metadata only, no CFG/instruction-content change.
      LLVMContext &Ctx = M.getContext();
      MDNode *MD = MDNode::get(
          Ctx, {ConstantAsMetadata::get(ConstantInt::get(
                    Type::getInt32Ty(Ctx), SiteIdx)),
                ConstantAsMetadata::get(ConstantInt::get(
                    Type::getInt64Ty(Ctx), FuncHash))});
      S->branch->setMetadata(MD_EJIT_VP_SITE, MD);
    }
    ++SiteIdx;
  }

  if (SiteIdx != 0 && OnFunctionSites)
    OnFunctionSites(F.getName(), SiteIdx);
}

//===----------------------------------------------------------------------===//
// Tier-2 guarded specialization.
//===----------------------------------------------------------------------===//

EJitScalarValueSpecPass::EJitScalarValueSpecPass(ArrayRef<PgoScalarSite> sites)
    : sites_(sites.begin(), sites.end()) {}

namespace {

const PgoScalarSite *lookupSite(ArrayRef<PgoScalarSite> sites,
                                uint64_t funcHash, uint32_t siteIdx) {
  for (const PgoScalarSite &S : sites)
    if (S.funcHash == funcHash && S.siteIndex == siteIdx)
      return &S;
  return nullptr;
}

/// Conservative shape gate (EJIT_VALUE_PROFILE.md §7.2): dedicated preheader,
/// single exiting block (the site's), unique exit block, single latch,
/// innermost loop (cloneLoopWithPreheader's documented restriction), and the
/// bound dominates the preheader (loop invariance alone does not imply
/// dominance). Also bound the loop size so cloning cost stays sane.
bool shapeGate(BranchInst *BI, Loop *L, DominatorTree &DT, Value *Bound,
               std::string &reason) {
  if (!L->isInnermost()) {
    reason = "not an innermost loop";
    return false;
  }
  if (!L->getLoopPreheader()) {
    reason = "no dedicated preheader";
    return false;
  }
  if (L->getExitingBlock() != BI->getParent()) {
    reason = "multiple exiting blocks";
    return false;
  }
  if (!L->getUniqueExitBlock()) {
    reason = "no unique exit block";
    return false;
  }
  if (!L->getLoopLatch()) {
    reason = "no unique latch";
    return false;
  }
  if (L->getNumBlocks() > 64) {
    reason = "loop too large";
    return false;
  }
  // The bound must dominate the preheader: loop invariance only says it is
  // defined outside the loop, not that it is available at the guard point.
  if (auto *I = dyn_cast<Instruction>(Bound))
    if (!DT.dominates(I->getParent(), L->getLoopPreheader())) {
      reason = "bound does not dominate the preheader";
      return false;
    }
  return true;
}

bool specializeSite(Function &F, LoopInfo &LI, DominatorTree &DT,
                    ScalarEvolution &SE, BranchInst *BI, Value *Bound,
                    uint64_t topValue) {
  Loop *L = LI.getLoopFor(BI->getParent());
  std::string reason;
  if (!L || !shapeGate(BI, L, DT, Bound, reason)) {
    LLVM_DEBUG(dbgs() << "ejit-value-profile: skip site in " << F.getName()
                      << ": " << reason << "\n");
    return false;
  }

  // LCSSA first: every loop-defined value used after the loop flows through a
  // PHI in the unique exit block, so the shared exit can join the hot and cold
  // versions with one extra incoming edge each.
  formLCSSARecursively(*L, DT, &LI, &SE);
  // Re-fetch after LCSSA rewrites (preheader/exit may have been split anew).
  L = LI.getLoopFor(BI->getParent());
  if (!L) {
    LLVM_DEBUG(dbgs() << "ejit-value-profile: loop lost after LCSSA\n");
    return false;
  }
  BasicBlock *PH = L->getLoopPreheader();
  BasicBlock *ExitBB = L->getUniqueExitBlock();
  BasicBlock *Exiting = L->getExitingBlock();
  if (!PH || !ExitBB || !Exiting) {
    LLVM_DEBUG(dbgs() << "ejit-value-profile: shape lost after LCSSA\n");
    return false;
  }

  // Split the preheader: PH keeps everything up to the guard; HotPH becomes
  // the (empty) preheader of the original loop, which is the hot version.
  BasicBlock *HotPH =
      SplitBlock(PH, PH->getTerminator(), &DT, &LI, nullptr, "vp.hot.ph");

  // Clone the loop (with its new preheader) into the cold fallback, then remap
  // the cloned instructions (PHI incoming blocks/values) through VMap.
  ValueToValueMapTy VMap;
  SmallVector<BasicBlock *, 8> ColdBlocks;
  (void)cloneLoopWithPreheader(
      /*Before=*/HotPH, /*LoopDomBB=*/PH, L, VMap, ".vp.cold", &LI, &DT,
      ColdBlocks);
  remapInstructionsInBlocks(ColdBlocks, VMap);
  BasicBlock *ColdPH = cast<BasicBlock>(VMap[HotPH]);
  BasicBlock *ColdExiting = cast<BasicBlock>(VMap[Exiting]);

  // The guard selects the hot loop (original) or the cold fallback clone.
  IRBuilder<> B(PH->getTerminator());
  Value *Guard = B.CreateICmpEQ(
      Bound, ConstantInt::get(Bound->getType(), topValue), "ejit.vp.guard");
  B.CreateCondBr(Guard, HotPH, ColdPH);
  PH->getTerminator()->eraseFromParent();

  // Hot path: every IN-LOOP use of the bound becomes the constant the guard
  // just proved; uses outside the loop (exit PHIs, later code) stay dynamic.
  Constant *ConstV = ConstantInt::get(Bound->getType(), topValue);
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      I.replaceUsesOfWith(Bound, ConstV);

  // Join the cold loop into the shared exit block: each LCSSA PHI gains the
  // cloned value on the cold exiting edge. The hot edge keeps the original.
  for (PHINode &PN : ExitBB->phis()) {
    Value *HotInc = PN.getIncomingValueForBlock(Exiting);
    Value *ColdInc = HotInc;
    auto It = VMap.find(HotInc);
    if (It != VMap.end())
      ColdInc = It->second;
    PN.addIncoming(ColdInc, ColdExiting);
  }

  // Document the proven fact for the optimization pipeline.
  Function *AssumeFn = Intrinsic::getOrInsertDeclaration(
      F.getParent(), Intrinsic::assume);
  CallInst::Create(AssumeFn, {Guard}, "", &HotPH->front());

  LLVM_DEBUG(dbgs() << "ejit-value-profile: specialized " << F.getName()
                    << " bound -> " << topValue << "\n");
  return true;
}

} // namespace

PreservedAnalyses EJitScalarValueSpecPass::run(Function &F,
                                               FunctionAnalysisManager &FAM) {
  if (sites_.empty())
    return PreservedAnalyses::all();

  // Collect annotated sites first (iterating while erasing metadata is fine,
  // but transforms invalidate nothing until we act on them all).
  struct Candidate {
    BranchInst *BI = nullptr;
    Value *Bound = nullptr;
    uint64_t topValue = 0;
    uint64_t topCount = 0;
    uint64_t total = 0;
  };
  SmallVector<Candidate, 4> Candidates;
  SmallPtrSet<Loop *, 4> HandledLoops;

  LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
  DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      MDNode *MD = I.getMetadata(MD_EJIT_VP_SITE);
      if (!MD || MD->getNumOperands() < 2)
        continue;
      auto *IdxC = mdconst::dyn_extract<ConstantInt>(MD->getOperand(0));
      auto *HashC = mdconst::dyn_extract<ConstantInt>(MD->getOperand(1));
      auto *BI = dyn_cast<BranchInst>(&I);
      if (!IdxC || !HashC || !BI || !BI->isConditional()) {
        I.setMetadata(MD_EJIT_VP_SITE, nullptr);
        continue;
      }
      const uint32_t siteIdx = static_cast<uint32_t>(IdxC->getZExtValue());
      const uint64_t funcHash = HashC->getZExtValue();
      const PgoScalarSite *Site = lookupSite(sites_, funcHash, siteIdx);
      if (!Site) {
        I.setMetadata(MD_EJIT_VP_SITE, nullptr);
        continue;
      }
      // Defense in depth: the driver already filtered below-threshold sites;
      // re-check here so the pass never specializes on weak evidence.
      if (Site->topCount < EJIT_SRE_VP_MIN_SAMPLES ||
          Site->topCount * 100 < Site->total * EJIT_SRE_VP_MIN_CONF_PERCENT) {
        LLVM_DEBUG(dbgs() << "ejit-value-profile: below threshold func="
                          << F.getName() << " site=" << siteIdx
                          << " top=" << Site->topCount
                          << " total=" << Site->total << "\n");
        I.setMetadata(MD_EJIT_VP_SITE, nullptr);
        continue;
      }
      Loop *L = LI.getLoopFor(BI->getParent());
      if (!L || !HandledLoops.insert(L).second) {
        // One specialization per loop; further sites in the same loop stay on
        // the generic path (their metadata is dropped).
        I.setMetadata(MD_EJIT_VP_SITE, nullptr);
        continue;
      }
      Value *Bound = findLoopBound(BI, *L);
      if (!Bound) {
        I.setMetadata(MD_EJIT_VP_SITE, nullptr);
        HandledLoops.erase(L);
        continue;
      }
      Candidates.push_back(
          {BI, Bound, Site->topValue, Site->topCount, Site->total});
    }
  }

  if (Candidates.empty())
    return PreservedAnalyses::all();

  bool Changed = false;
  for (const Candidate &C : Candidates) {
    if (specializeSite(F, LI, DT, SE, C.BI, C.Bound, C.topValue)) {
      Changed = true;
      ejitVpBumpScalarSpecialized(1);
      EJIT_DIAG_DEBUG("scalar spec func=%s top=%llu count=%llu total=%llu",
                      F.getName().str().c_str(),
                      static_cast<unsigned long long>(C.topValue),
                      static_cast<unsigned long long>(C.topCount),
                      static_cast<unsigned long long>(C.total));
    }
    // Strip the metadata either way: the site was consumed.
    C.BI->setMetadata(MD_EJIT_VP_SITE, nullptr);
  }
  if (!Changed)
    return PreservedAnalyses::all();
  // LI/DT were updated in place, but BFI/PSI/LI dependents are stale.
  return PreservedAnalyses::none();
}
