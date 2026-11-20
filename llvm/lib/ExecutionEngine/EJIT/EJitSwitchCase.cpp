//===-- EJitSwitchCase.cpp - ejit_runtime_dim switch-case arms ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See EJitSwitchCase.h and jit_design_doc/EJIT_SWITCH_CASE.md §4-§5.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSwitchCase.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <optional>

using namespace llvm;
using namespace llvm::ejit;

#ifndef EJIT_SWITCH_CASE_MAX_ARMS
#define EJIT_SWITCH_CASE_MAX_ARMS 8
#endif
#ifndef EJIT_SWITCH_CASE_MAX_REGION
#define EJIT_SWITCH_CASE_MAX_REGION 2000
#endif
#ifndef EJIT_SWITCH_CASE_MAX_CLONED
#define EJIT_SWITCH_CASE_MAX_CLONED 8000
#endif

EJitSwitchCaseLimits EJitSwitchCaseLimits::fromBuild() {
  return {EJIT_SWITCH_CASE_MAX_ARMS, EJIT_SWITCH_CASE_MAX_REGION,
          EJIT_SWITCH_CASE_MAX_CLONED};
}

Argument *llvm::ejit::getEJitRuntimeDim(const Function &F, unsigned *MaxArms) {
  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return nullptr;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 3)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    if (!Tag || Tag->getString() != TAG_EJIT_RUNTIME_DIM)
      continue;
    auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
    if (!Idx || Idx->getZExtValue() >= F.arg_size())
      return nullptr;
    Argument *Arg = F.getArg(static_cast<unsigned>(Idx->getZExtValue()));
    // Sema enforces this; the JIT does not trust bitcode it did not check.
    auto *Ty = dyn_cast<IntegerType>(Arg->getType());
    if (!Ty || Ty->getBitWidth() > 32)
      return nullptr;
    if (MaxArms) {
      *MaxArms = 0;
      if (Sub->getNumOperands() >= 4)
        if (auto *N = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(3)))
          *MaxArms = static_cast<unsigned>(N->getZExtValue());
    }
    return Arg;
  }
  return nullptr;
}

namespace {

/// A recognized modulus projection, `urem (A or zext A), C`, or its
/// power-of-two form `and (A or zext A), C - 1`.
struct Projection {
  unsigned width = 0; ///< bit width of the urem or and
  uint64_t divisor = 0;

  uint64_t descriptor() const {
    return static_cast<uint64_t>(EJitProjectionOp::URem) |
           (static_cast<uint64_t>(width) << 8) | (divisor << 32);
  }
  bool operator==(const Projection &O) const {
    return width == O.width && divisor == O.divisor;
  }
};

/// Is \p V the modulus projection of \p A? Unsigned only: srem, sdiv, trunc and
/// sext change which values share a key, so they are the identity row (§4.1).
/// InstCombine turns `% 2^n` into a low-bit mask; the mask is recorded as that
/// urem, so `% 4` and `& 3` share a descriptor.
std::optional<Projection> matchModulus(const Value *V, const Argument *A) {
  auto *BO = dyn_cast<BinaryOperator>(V);
  if (!BO || (BO->getOpcode() != Instruction::URem &&
              BO->getOpcode() != Instruction::And))
    return std::nullopt;
  const Value *X = BO->getOperand(0);
  if (auto *Z = dyn_cast<ZExtInst>(X))
    X = Z->getOperand(0);
  if (X != A)
    return std::nullopt;
  auto *C = dyn_cast<ConstantInt>(BO->getOperand(1));
  if (!C || C->isZero())
    return std::nullopt;
  APInt Divisor = C->getValue();
  if (BO->getOpcode() == Instruction::And) {
    if (!Divisor.isMask())
      return std::nullopt;
    Divisor = Divisor.zext(Divisor.getBitWidth() + 1) + 1;
  }
  // A divisor beyond the parameter's range is the identity. This is what keeps
  // the 32-bit descriptor field lossless.
  const unsigned ArgWidth = A->getType()->getIntegerBitWidth();
  if (Divisor.getActiveBits() > ArgWidth)
    return std::nullopt;
  return Projection{BO->getType()->getIntegerBitWidth(),
                    Divisor.getZExtValue()};
}

/// How a value depends on the runtime dim; combining two takes the larger. It
/// decides whether arms are worth building, never whether they are correct:
/// an arm only replaces instances of P and is entered only when P is its key.
enum class Dep { None, Projected, Direct, Unknown };

/// Classifies addresses, recording the projections the walk ends at.
class DepWalker {
public:
  explicit DepWalker(const Argument *A) : A(A) {}

  Dep classify(const Value *V) { return walk(V, 0); }

  /// Every modulus instruction a Projected walk ended at.
  SmallSetVector<const Instruction *, 4> Projections;

private:
  /// Address expressions deeper than this are not ones v1 reasons about.
  static constexpr unsigned MaxDepth = 32;

  Dep walk(const Value *V, unsigned Depth) {
    if (V == A)
      return Dep::Direct;
    auto *I = dyn_cast<Instruction>(V);
    if (!I)
      return Dep::None;
    if (matchModulus(I, A)) {
      Projections.insert(I);
      return Dep::Projected;
    }
    // A node already on the walk is a phi cycle; the outer frame collects its
    // other operands. Reading it as Unknown would decline any entry with a
    // loop-variant index.
    auto [It, Inserted] = Memo.try_emplace(I, Dep::None);
    if (!Inserted)
      return It->second;
    if (Depth > MaxDepth) {
      It->second = Dep::Unknown;
      return Dep::Unknown;
    }
    Dep Result = Dep::None;
    for (const Value *Op : I->operands())
      Result = std::max(Result, walk(Op, Depth + 1));
    Memo[I] = Result;
    return Result;
  }

  const Argument *A;
  DenseMap<const Instruction *, Dep> Memo;
};

/// Does a copy of \p I compute the same value as \p I? Uses above the switch
/// keep the original and uses below take the copy. A freeze does not: it
/// picks afresh for a poison operand (§4.2 rule 3).
bool isReproducible(const Instruction *I) {
  if (isa<PHINode>(I) || isa<FreezeInst>(I) || I->mayReadOrWriteMemory() ||
      !isSafeToSpeculativelyExecute(I))
    return false;
  if (auto *CB = dyn_cast<CallBase>(I))
    return isa<IntrinsicInst>(CB) && !CB->isConvergent();
  return true;
}

/// Collect, root first, the key-dependent instructions between P and \p Ptr
/// that \p InRegion says lie before the switch point. Returns false if any of
/// them cannot be recomputed there (§4.2 rule 3).
bool collectOutsideChain(Value *Ptr, const Argument *A, DepWalker &Walker,
                         function_ref<bool(const Instruction *)> InRegion,
                         SmallSetVector<Instruction *, 8> &Chain,
                         DenseMap<Instruction *, bool> &Visited) {
  auto *I = dyn_cast<Instruction>(Ptr);
  if (!I)
    return true;
  auto [It, Inserted] = Visited.try_emplace(I, true);
  if (!Inserted)
    return It->second;

  if (Walker.classify(I) != Dep::Projected) // independent of the key
    return true;
  bool OK = true;
  if (!matchModulus(I, A))
    for (Value *Op : I->operands())
      OK &= collectOutsideChain(Op, A, Walker, InRegion, Chain, Visited);
  if (OK && !InRegion(I)) {
    if (isReproducible(I))
      Chain.insert(I);
    else
      OK = false;
  }
  It = Visited.find(I);
  It->second = OK;
  return OK;
}

/// The one projection every instruction in \p Us is, or none if they disagree
/// or \p Us is empty.
std::optional<Projection> agreedProjection(ArrayRef<const Instruction *> Us,
                                           const Argument *A) {
  std::optional<Projection> P;
  for (const Instruction *U : Us) {
    Projection Q = *matchModulus(U, A);
    if (P && !(*P == Q))
      return std::nullopt;
    P = Q;
  }
  return P;
}

/// Is \p LI may_const with no key, or at any key of its own \p Projections? A
/// bound-pointer field is recognized by its offset, so it may be one at some
/// keys only. Without one projection within \p MaxArms, only key 0 is tried.
bool isSiteCandidate(LoadInst *LI, const Argument *A,
                     ArrayRef<const Instruction *> Projections,
                     EJitStructFieldPass &Resolver, unsigned MaxArms) {
  if (Resolver.isMayConstCandidate(LI, AssumedArgMap()))
    return true;
  uint64_t Keys = 1;
  if (auto P = agreedProjection(Projections, A); P && P->divisor <= MaxArms)
    Keys = P->divisor;
  for (uint64_t K = 0; K < Keys; ++K)
    if (Resolver.isMayConstCandidate(LI, AssumedArgMap{{A, K}}))
      return true;
  return false;
}

/// §4.2 rule 4, plus what cloning itself cannot do.
const char *checkRegionShape(const SmallVectorImpl<BasicBlock *> &Blocks) {
  for (BasicBlock *BB : Blocks) {
    if (BB->hasAddressTaken())
      return "region-block-address-taken";
    if (BB->isEHPad())
      return "region-eh-pad";
    const Instruction *T = BB->getTerminator();
    if (isa<InvokeInst>(T) || isa<CallBrInst>(T) || isa<IndirectBrInst>(T))
      return "region-unsupported-terminator";
    for (const Instruction &I : *BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->cannotDuplicate())
          return "region-noduplicate-call";
        // Arms split the threads reaching it; the key is not known uniform.
        if (CB->isConvergent())
          return "region-convergent-call";
      }
      if (I.getType()->isTokenTy())
        return "region-token-value";
    }
  }
  return nullptr;
}

unsigned countInstructions(const SmallVectorImpl<BasicBlock *> &Blocks) {
  unsigned N = 0;
  for (BasicBlock *BB : Blocks)
    N += BB->size();
  return N;
}

[[maybe_unused]] std::string joinKeys(ArrayRef<uint32_t> Keys) {
  std::string S;
  for (uint32_t K : Keys) {
    if (!S.empty())
      S += ',';
    S += utostr(K);
  }
  return S.empty() ? "-" : S;
}

} // namespace

EJitSwitchCaseResult
llvm::ejit::runSwitchCase(Function &F, EJitStructFieldPass &Resolver,
                          FunctionAnalysisManager &FAM,
                          const EJitSwitchCaseLimits &Limits,
                          const EJitSwitchCaseDecision *Replay) {
  EJitSwitchCaseResult R;
  [[maybe_unused]] const std::string FnName = F.getName().str();
  auto decline = [&](std::string Why) {
    R.path = EJitSwitchCaseResult::Path::None;
    R.declined = std::move(Why);
    EJIT_DIAG(
        "rtdim func=%s path=none declined=%s sites=%u projection=0x%016llx",
        FnName.c_str(), R.declined.c_str(), R.sites,
        static_cast<unsigned long long>(R.projection));
    return R;
  };

  unsigned EntryMaxArms = 0;
  Argument *A = getEJitRuntimeDim(F, &EntryMaxArms);
  if (!A)
    return decline("no-runtime-dim");
  const unsigned MaxArms = EntryMaxArms ? EntryMaxArms : Limits.maxArms;

  // Cloning needs no SSAUpdater because every predecessor of a region block is
  // in the region; an unreachable one would break that, so drop those first.
  if (removeUnreachableBlocks(F))
    FAM.invalidate(F, PreservedAnalyses::none());

  // Tier-1 built no arms, so Tier-2 builds none (§10).
  if (Replay && Replay->keys.empty())
    return decline("pgo-replay-no-arms");

  // Sites: may_const loads that PASS6 declined and whose address depends on
  // the runtime dim (§4.1). Each is walked on its own, so that only sites'
  // projections decide P; the shared walker is just a cheap filter.
  const AssumedArgMap NoKey;
  SmallVector<LoadInst *, 8> Sites;
  SmallSetVector<const Instruction *, 4> SiteProjections;
  DepWalker Walker(A);
  bool AnyDirect = false, AnyUnknown = false;
  for (Instruction &I : instructions(F)) {
    auto *LI = dyn_cast<LoadInst>(&I);
    if (!LI || LI->isVolatile() || LI->isAtomic())
      continue;
    if (Walker.classify(LI->getPointerOperand()) == Dep::None)
      continue;
    DepWalker Own(A);
    Dep D = Own.classify(LI->getPointerOperand());
    if (D == Dep::None ||
        !isSiteCandidate(LI, A, Own.Projections.getArrayRef(), Resolver,
                         MaxArms))
      continue;
    if (Resolver.resolveMayConstLoad(LI, NoKey))
      continue;
    Sites.push_back(LI);
    SiteProjections.insert_range(Own.Projections);
    AnyDirect |= D == Dep::Direct;
    AnyUnknown |= D == Dep::Unknown;
  }
  R.sites = Sites.size();

  if (Sites.empty())
    return decline("no-entry-local-site");
  if (AnyUnknown)
    return decline("address-not-analyzable");
  if (AnyDirect) {
    // The identity row of §4.1: valid, but only the lazy path serves it.
    R.projection = static_cast<uint64_t>(EJitProjectionOp::Identity);
    return decline("projection-identity-lazy-not-implemented");
  }

  // P must be one projection: every modulus the sites went through agrees.
  assert(!SiteProjections.empty() && "a Projected walk records its projection");
  std::optional<Projection> P =
      agreedProjection(SiteProjections.getArrayRef(), A);
  if (!P)
    return decline("mixed-projections");
  R.projection = P->descriptor();
  R.domain = P->divisor;

  // §5.1: eager only when the whole domain fits the arm limit.
  if (R.domain > MaxArms)
    return decline("domain-exceeds-arms-lazy-not-implemented");

  // §10: Tier-2 takes Tier-1's keys so both tiers see the same CFG. Any key
  // in the domain gives a correct arm.
  if (Replay) {
    if (Replay->projection != R.projection ||
        any_of(Replay->keys, [&](uint32_t K) { return K >= R.domain; }))
      return decline("pgo-replay-mismatch");
    R.keptKeys = Replay->keys;
  }

  // §4.3 step 3: keep each key that resolves a site, before any IR changes.
  // Assuming the parameter is k assumes P is k, since k < divisor.
  for (uint64_t K = 0; !Replay && K < R.domain; ++K) {
    const AssumedArgMap Key{{A, K}};
    unsigned Resolved = 0;
    for (LoadInst *LI : Sites)
      if (Resolver.isMayConstCandidate(LI, Key) &&
          Resolver.resolveMayConstLoad(LI, Key))
        ++Resolved;
    EJIT_DIAG_VERBOSE("rtdim func=%s key=%llu sites=%u%s", FnName.c_str(),
                      static_cast<unsigned long long>(K), Resolved,
                      Resolved ? "" : " not-kept");
    if (Resolved)
      R.keptKeys.push_back(static_cast<uint32_t>(K));
  }
  if (R.keptKeys.empty())
    return decline("no-key-resolves-a-site");

  // §4.3: the switch point is the latest point that dominates every site and
  // is not inside a loop.
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  BasicBlock *D = Sites.front()->getParent();
  for (LoadInst *S : Sites)
    D = DT.findNearestCommonDominator(D, S->getParent());
  Instruction *SplitBefore = nullptr;
  if (Loop *L = LI.getLoopFor(D)) {
    while (Loop *Parent = L->getParentLoop())
      L = Parent;
    BasicBlock *Preheader = L->getLoopPreheader();
    if (!Preheader)
      return decline("loop-without-preheader");
    D = Preheader;
    SplitBefore = Preheader->getTerminator();
  } else {
    SplitBefore = D->getTerminator();
    for (LoadInst *S : Sites)
      if (S->getParent() == D && S->comesBefore(SplitBefore))
        SplitBefore = S;
  }

  // The region: D from the switch point on, and every block D strictly
  // dominates. Computed before splitting, so a decline leaves the IR intact.
  SmallVector<BasicBlock *, 16> Strict;
  for (BasicBlock &BB : F)
    if (&BB != D && DT.dominates(D, &BB))
      Strict.push_back(&BB);
  SmallVector<BasicBlock *, 16> ShapeBlocks(Strict.begin(), Strict.end());
  ShapeBlocks.push_back(D);
  if (const char *Why = checkRegionShape(ShapeBlocks))
    return decline(Why);
  unsigned Tail = 0;
  for (auto It = SplitBefore->getIterator(); It != D->end(); ++It)
    ++Tail;

  SmallPtrSet<BasicBlock *, 16> StrictSet(Strict.begin(), Strict.end());
  // In the region: a block D strictly dominates, or D from the switch point
  // on, which the split moves into the region's head.
  auto InRegion = [&](const Instruction *I) {
    return StrictSet.contains(I->getParent()) ||
           (I->getParent() == D && !I->comesBefore(SplitBefore));
  };
  SmallSetVector<Instruction *, 8> Copies;
  DenseMap<Instruction *, bool> Visited;
  for (LoadInst *S : Sites)
    if (!collectOutsideChain(S->getPointerOperand(), A, Walker, InRegion,
                             Copies, Visited))
      return decline("address-chain-not-rematerializable");

  R.regionSize = countInstructions(Strict) + Tail + Copies.size();
  if (R.regionSize > Limits.maxRegion)
    return decline("region-over-limit");
  R.cloned = R.regionSize * R.keptKeys.size();
  if (R.cloned > Limits.maxCloned)
    return decline("cloned-over-limit");

  //===--- Transform. Above, only unreachable blocks were removed. ---===//

  BasicBlock *Head =
      SplitBlock(D, SplitBefore, static_cast<DominatorTree *>(nullptr),
                 /*LI=*/nullptr, /*MSSAU=*/nullptr, "ejit.sc.default");
  SmallVector<BasicBlock *, 16> RegionBlocks;
  SmallPtrSet<BasicBlock *, 16> RegionSet(StrictSet.begin(), StrictSet.end());
  RegionSet.insert(Head);
  for (BasicBlock &BB : F)
    if (RegionSet.contains(&BB))
      RegionBlocks.push_back(&BB);

  // §4.2 rule 3: recompute the address chain at the top of the region, so
  // that the clones can substitute the key into it.
  Instruction *InsertPt = &*Head->getFirstInsertionPt();
  SmallVector<std::pair<Instruction *, Instruction *>, 8> Remat;
  ValueToValueMapTy RematMap;
  for (Instruction *I : Copies) {
    Instruction *C = I->clone();
    C->setName(I->getName() + ".sc");
    C->insertBefore(InsertPt->getIterator());
    RemapInstruction(C, RematMap,
                     RF_IgnoreMissingLocals | RF_NoModuleLevelChanges);
    RematMap[I] = C;
    Remat.push_back({I, C});
  }
  for (auto [Orig, Copy] : Remat)
    Orig->replaceUsesWithIf(Copy, [&](Use &U) {
      auto *User = cast<Instruction>(U.getUser());
      return User != Copy && RegionSet.contains(User->getParent());
    });

  SmallVector<Instruction *, 4> RegionProjections;
  for (BasicBlock *BB : RegionBlocks)
    for (Instruction &I : *BB)
      if (auto Q = matchModulus(&I, A); Q && *Q == *P)
        RegionProjections.push_back(&I);

  // Dispatch (§4.4), on a key recomputed from the parameter.
  BasicBlock *Pre = Head->getSinglePredecessor();
  assert(Pre && "SplitBlock leaves one predecessor");
  IRBuilder<> B(Pre->getTerminator());
  auto *KeyTy = IntegerType::get(F.getContext(), P->width);
  // The switch also runs on paths that reach no site, where branching on an
  // undef or poison parameter would be new UB: freeze it, as loop unswitching
  // does, unless it is noundef.
  Value *KeyArg = A;
  if (!isGuaranteedNotToBeUndefOrPoison(A))
    KeyArg = B.CreateFreeze(A, A->getName() + ".fr");
  if (KeyArg->getType() != KeyTy)
    KeyArg = B.CreateZExt(KeyArg, KeyTy);
  // A power-of-two domain dispatches on the mask, as InstCombine would.
  Value *KeyVal =
      isPowerOf2_64(P->divisor)
          ? B.CreateAnd(KeyArg, ConstantInt::get(KeyTy, P->divisor - 1),
                        "ejit.sc.key")
          : B.CreateURem(KeyArg, ConstantInt::get(KeyTy, P->divisor),
                         "ejit.sc.key");
  SwitchInst *Dispatch = B.CreateSwitch(KeyVal, Head, R.keptKeys.size());
  Pre->getTerminator()->eraseFromParent();

  for (uint32_t K : R.keptKeys) {
    ValueToValueMapTy VMap;
    SmallVector<BasicBlock *, 16> Clones;
    for (BasicBlock *BB : RegionBlocks) {
      BasicBlock *NB = CloneBasicBlock(BB, VMap, ".sc" + Twine(K), &F);
      VMap[BB] = NB;
      Clones.push_back(NB);
    }
    remapInstructionsInBlocks(Clones, VMap);

    // Region exits gain a predecessor per clone. Done while VMap still maps
    // the projections substituted below.
    for (BasicBlock *BB : RegionBlocks) {
      auto *NB = cast<BasicBlock>(VMap[BB]);
      SmallPtrSet<BasicBlock *, 4> Seen;
      for (BasicBlock *Succ : successors(BB)) {
        if (RegionSet.contains(Succ) || !Seen.insert(Succ).second)
          continue;
        for (PHINode &PN : Succ->phis()) {
          SmallVector<Value *, 2> Incoming;
          for (unsigned I = 0, E = PN.getNumIncomingValues(); I != E; ++I)
            if (PN.getIncomingBlock(I) == BB) {
              Value *V = PN.getIncomingValue(I);
              auto It = VMap.find(V);
              Incoming.push_back(
                  It != VMap.end() ? static_cast<Value *>(It->second) : V);
            }
          for (Value *V : Incoming)
            PN.addIncoming(V, NB);
        }
      }
    }

    // P is the key in this arm. Only P: the parameter stays live (§11 rule 1).
    for (Instruction *U : RegionProjections) {
      auto *NU = cast<Instruction>(VMap[U]);
      NU->replaceAllUsesWith(ConstantInt::get(NU->getType(), K));
      NU->eraseFromParent();
    }
    Dispatch->addCase(ConstantInt::get(KeyTy, K), cast<BasicBlock>(VMap[Head]));
  }

  FAM.invalidate(F, PreservedAnalyses::none());
  R.path = EJitSwitchCaseResult::Path::Eager;
  EJIT_DIAG("rtdim func=%s path=eager projection=0x%016llx M=%llu sites=%u "
            "kept=%s keys-from=%s switch=%s region=%u cloned=%u",
            FnName.c_str(), static_cast<unsigned long long>(R.projection),
            static_cast<unsigned long long>(R.domain), R.sites,
            joinKeys(R.keptKeys).c_str(), Replay ? "tier1" : "analysis",
            Pre->getName().str().c_str(), R.regionSize, R.cloned);
  return R;
}
