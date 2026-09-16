//===-- EJitStructFieldPass.cpp - JIT Constant Substitution ---------------===//

#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitPreservedScalar.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Local.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-struct-field"

void EJitStructFieldPass::initFromModule(Module &M) {
  // A pass instance may be reused after the previous module was destroyed.
  // Never preserve keys that point into old IR across a rebuild.
  mapsBuilt_ = false;
  gvPeriodMap_.clear();
  mayConstFieldMap_.clear();
  EJIT_DIAG_VERBOSE("struct-field initFromModule module=%s globals=%zu",
                    M.getName().str().c_str(), M.global_size());
  // Build GV period map.
  for (GlobalVariable &GV : M.globals()) {
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;

    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;

      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag)
        continue;

      if (Tag->getString() == TAG_EJIT_PERIOD_ARR) {
        auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
        size_t sz = 0;
        if (Sub->getNumOperands() >= 3)
          if (auto *CI = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2)))
            sz = CI->getZExtValue();
        if (PN)
          gvPeriodMap_[&GV] = {PN->getString().str(), true, sz};
      } else if (Tag->getString() == TAG_EJIT_PERIOD) {
        auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
        std::string pn = PN ? PN->getString().str() : "";
        gvPeriodMap_[&GV] = {pn, false, 0};
      }
    }

    // Build may_const field offset map (v1.7 fallback for when
    // optimization passes drop per-load !ejit.may_const metadata).
    SmallVector<uint64_t, 4> offsets;
    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;
      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_MAY_CONST_FIELD)
        continue;
      if (auto *CI = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(1)))
        offsets.push_back(CI->getZExtValue());
    }
    if (!offsets.empty())
      mayConstFieldMap_[&GV] = std::move(offsets);
  }

  initBoundArgumentPropagation(M);
  initFreeDimAssumptions(M);
  initPreservedDimensions(M);
  mapsBuilt_ = true;
#ifdef EJIT_DIAG_ENABLE
  EJIT_DIAG_DEBUG("struct-field initFromModule module=%s globals=%zu "
                  "gvPeriod=%zu mayConstField=%zu",
                  M.getName().str().c_str(), M.global_size(), gvPeriodMap_.size(),
                  mayConstFieldMap_.size());
#endif
}

//===----------------------------------------------------------------------===//
// IR analysis helpers
//===----------------------------------------------------------------------===//

/// Walk pointer casts and GEP chains up to the root GlobalVariable.
static const GlobalVariable *findRootGV(const Value *V) {
  V = V->stripPointerCasts();
  if (auto *GV = dyn_cast<GlobalVariable>(V))
    return GV;
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return findRootGV(GEP->getPointerOperand());
  return nullptr;
}

static const Argument *findRootArgument(const Value *V) {
  V = V->stripPointerCasts();
  if (auto *Arg = dyn_cast<Argument>(V))
    return Arg;
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return findRootArgument(GEP->getPointerOperand());
  return nullptr;
}

static bool functionBindsArgument(const Function &F, unsigned ArgIndex) {
  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return false;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 3)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
    if (Tag && Tag->getString() == TAG_EJIT_BOUND_PTR && Idx &&
        Idx->getZExtValue() == ArgIndex)
      return true;
  }
  return false;
}

static MDNode *getBoundArgumentMetadata(const Function &F, unsigned ArgIndex) {
  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return nullptr;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 4)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
    if (Tag && Tag->getString() == TAG_EJIT_BOUND_PTR && Idx &&
        Idx->getZExtValue() == ArgIndex)
      return Sub;
  }
  return nullptr;
}

static bool hasMatchingBoundArgumentContract(
    const Function &F, unsigned ArgIndex, StringRef PeriodName,
    uint64_t BaseOffset, uint64_t RootSize,
    ArrayRef<std::pair<uint64_t, uint64_t>> RootFields) {
  MDNode *MD = getBoundArgumentMetadata(F, ArgIndex);
  if (!MD)
    return false;

  auto *Period = dyn_cast<MDString>(MD->getOperand(1));
  auto *Size = mdconst::dyn_extract<ConstantInt>(MD->getOperand(3));
  if (!Period || Period->getString() != PeriodName || !Size)
    return false;
  const uint64_t BoundSize = Size->getZExtValue();
  if (!BoundSize || BaseOffset > RootSize || BoundSize > RootSize - BaseOffset)
    return false;

  SmallVector<std::pair<uint64_t, uint64_t>, 4> DeclaredFields;
  for (unsigned I = 4; I < MD->getNumOperands(); ++I) {
    // Only MDNode operands are field descriptors; fixed scalars may follow
    // Size (the pointee alignment does). Skipping non-nodes keeps this loop
    // position-independent, so adding another scalar cannot silently
    // invalidate every bound pointer. A malformed NODE is still rejected.
    auto *Field = dyn_cast<MDNode>(MD->getOperand(I));
    if (!Field)
      continue;
    if (Field->getNumOperands() != 2)
      return false;
    auto *Offset = mdconst::dyn_extract<ConstantInt>(Field->getOperand(0));
    auto *FieldSize = mdconst::dyn_extract<ConstantInt>(Field->getOperand(1));
    if (!Offset || !FieldSize || !FieldSize->getZExtValue() ||
        Offset->getZExtValue() > BoundSize ||
        FieldSize->getZExtValue() > BoundSize - Offset->getZExtValue())
      return false;
    DeclaredFields.emplace_back(Offset->getZExtValue(),
                                FieldSize->getZExtValue());
  }

  SmallVector<std::pair<uint64_t, uint64_t>, 4> ExpectedFields;
  for (const auto &[Offset, FieldSize] : RootFields) {
    if (Offset < BaseOffset)
      continue;
    const uint64_t Relative = Offset - BaseOffset;
    if (Relative > BoundSize || FieldSize > BoundSize - Relative)
      continue;
    ExpectedFields.emplace_back(Relative, FieldSize);
  }
  llvm::sort(DeclaredFields);
  llvm::sort(ExpectedFields);
  return DeclaredFields == ExpectedFields;
}

static std::optional<uint64_t>
accumulateArgumentOffset(const DataLayout &DL, const Value *PtrOp,
                         const Argument *Root, const AssumedArgMap &Assumed);

/// Depth cap for evalWithAssumed. An index expression that needs more than this
/// is not one this pass should be reasoning about.
static constexpr unsigned kMaxAssumedEvalDepth = 16;

/// Constant-fold an integer expression, taking the value of any argument in
/// \p Assumed from that map. Returns std::nullopt when the expression reaches
/// anything else — a load, a call, an unmapped argument, an unhandled opcode —
/// or when folding would produce poison.
///
/// This is what lets an ejit_free_dim parameter unblock an address: after
/// IPSCCP folds the period index, `unitIdx * 5 + slotNo % 5` is
/// `add(15, urem(%slotNo, 5))`, which no constant test accepts but which folds
/// to 15 once %slotNo is assumed 0.
///
/// Deliberately an evaluation and not a rewrite of the expression: the caller
/// wants a byte offset to read process memory at, and the IR must be left
/// exactly as it was so the stores through the same address stay correct.
static std::optional<APInt>
evalWithAssumed(const Value *V, const AssumedArgMap &Assumed, unsigned Depth) {
  if (Depth > kMaxAssumedEvalDepth || !V->getType()->isIntegerTy())
    return std::nullopt;

  if (const auto *CI = dyn_cast<ConstantInt>(V))
    return CI->getValue();

  if (const auto *Arg = dyn_cast<Argument>(V)) {
    auto It = Assumed.find(Arg);
    if (It == Assumed.end())
      return std::nullopt;
    return APInt(V->getType()->getIntegerBitWidth(), It->second);
  }

  // An icmp feeding a select is the only comparison worth folding here; its
  // result is i1, so it cannot share the binary-operator path below.
  if (const auto *Cmp = dyn_cast<ICmpInst>(V)) {
    auto L = evalWithAssumed(Cmp->getOperand(0), Assumed, Depth + 1);
    if (!L)
      return std::nullopt;
    auto R = evalWithAssumed(Cmp->getOperand(1), Assumed, Depth + 1);
    if (!R)
      return std::nullopt;
    // `icmp samesign` promises both operands have the same sign; the
    // comparison is poison when they do not. Like the arithmetic flags below,
    // this is a fact about the values the program passes, and the witness may
    // not be one of them -- `icmp samesign slt i32 %slot, -1` is poison at
    // witness 0, and the select it feeds would otherwise yield an index that
    // passes every bounds check.
    if (Cmp->hasSameSign() && L->isNegative() != R->isNegative())
      return std::nullopt;
    return APInt(1, ICmpInst::compare(*L, *R, Cmp->getPredicate()) ? 1 : 0);
  }

  const auto *Op = dyn_cast<Operator>(V);
  if (!Op)
    return std::nullopt;

  const unsigned Opcode = Op->getOpcode();
  const unsigned Width = V->getType()->getIntegerBitWidth();

  switch (Opcode) {
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc: {
    auto X = evalWithAssumed(Op->getOperand(0), Assumed, Depth + 1);
    if (!X)
      return std::nullopt;
    if (Opcode == Instruction::ZExt) {
      // `zext nneg` promises the operand is non-negative as a signed value.
      if (const auto *PNI = dyn_cast<PossiblyNonNegInst>(Op))
        if (PNI->hasNonNeg() && X->isNegative())
          return std::nullopt;
      return X->zext(Width);
    }
    if (Opcode == Instruction::SExt)
      return X->sext(Width);
    // `trunc nuw/nsw` promise the discarded bits carry no information.
    APInt Res = X->trunc(Width);
    if (const auto *TI = dyn_cast<TruncInst>(Op)) {
      if (TI->hasNoUnsignedWrap() && Res.zext(X->getBitWidth()) != *X)
        return std::nullopt;
      if (TI->hasNoSignedWrap() && Res.sext(X->getBitWidth()) != *X)
        return std::nullopt;
    }
    return Res;
  }
  case Instruction::Select: {
    auto C = evalWithAssumed(Op->getOperand(0), Assumed, Depth + 1);
    if (!C)
      return std::nullopt;
    // Only the taken arm is evaluated: the other one may be unfoldable and is
    // irrelevant at this witness.
    return evalWithAssumed(Op->getOperand(C->isZero() ? 2 : 1), Assumed,
                           Depth + 1);
  }
  default:
    break;
  }

  if (Op->getNumOperands() != 2)
    return std::nullopt;
  auto L = evalWithAssumed(Op->getOperand(0), Assumed, Depth + 1);
  if (!L)
    return std::nullopt;
  auto R = evalWithAssumed(Op->getOperand(1), Assumed, Depth + 1);
  if (!R)
    return std::nullopt;

  // Poison-generating flags must be honoured, not just the always-poison cases
  // below. InstCombine attaches nuw/nsw/exact/disjoint/nneg using facts that
  // hold for the values the program actually passes; the witness is a value it
  // may never pass, so a flag whose precondition the witness breaks means this
  // expression is poison at the witness and the address derived from it is
  // fiction. `sub nuw i32 %slot, 1` at witness 0 would otherwise yield
  // 0xFFFFFFFF, which a later mask can launder into a plausible in-bounds
  // index.
  const auto *OBO = dyn_cast<OverflowingBinaryOperator>(Op);
  const bool NUW = OBO && OBO->hasNoUnsignedWrap();
  const bool NSW = OBO && OBO->hasNoSignedWrap();
  const auto *PEO = dyn_cast<PossiblyExactOperator>(Op);
  const bool Exact = PEO && PEO->isExact();

  switch (Opcode) {
  case Instruction::Add: {
    bool OvU = false, OvS = false;
    APInt Res = L->uadd_ov(*R, OvU);
    (void)L->sadd_ov(*R, OvS);
    if ((NUW && OvU) || (NSW && OvS))
      return std::nullopt;
    return Res;
  }
  case Instruction::Sub: {
    bool OvU = false, OvS = false;
    APInt Res = L->usub_ov(*R, OvU);
    (void)L->ssub_ov(*R, OvS);
    if ((NUW && OvU) || (NSW && OvS))
      return std::nullopt;
    return Res;
  }
  case Instruction::Mul: {
    bool OvU = false, OvS = false;
    APInt Res = L->umul_ov(*R, OvU);
    (void)L->smul_ov(*R, OvS);
    if ((NUW && OvU) || (NSW && OvS))
      return std::nullopt;
    return Res;
  }
  case Instruction::And:
    return *L & *R;
  case Instruction::Or:
    // `or disjoint` promises the operands share no set bit.
    if (const auto *PDI = dyn_cast<PossiblyDisjointInst>(Op))
      if (PDI->isDisjoint() && (*L & *R) != 0)
        return std::nullopt;
    return *L | *R;
  case Instruction::Xor:
    return *L ^ *R;
  // A shift at or past the bit width is poison, and a division or remainder
  // whose result is not representable likewise. Bail rather than bake in a
  // value the program would never have produced.
  case Instruction::Shl: {
    if (R->uge(Width))
      return std::nullopt;
    APInt Res = L->shl(*R);
    // nuw: no set bit shifted out. nsw: also the sign bit is preserved.
    if (NUW && Res.lshr(*R) != *L)
      return std::nullopt;
    if (NSW && Res.ashr(*R) != *L)
      return std::nullopt;
    return Res;
  }
  case Instruction::LShr: {
    if (R->uge(Width))
      return std::nullopt;
    APInt Res = L->lshr(*R);
    if (Exact && Res.shl(*R) != *L)
      return std::nullopt;
    return Res;
  }
  case Instruction::AShr: {
    if (R->uge(Width))
      return std::nullopt;
    APInt Res = L->ashr(*R);
    if (Exact && Res.shl(*R) != *L)
      return std::nullopt;
    return Res;
  }
  case Instruction::UDiv:
    if (R->isZero())
      return std::nullopt;
    if (Exact && !L->urem(*R).isZero())
      return std::nullopt;
    return std::optional<APInt>(L->udiv(*R));
  case Instruction::URem:
    return R->isZero() ? std::nullopt : std::optional<APInt>(L->urem(*R));
  case Instruction::SDiv:
    if (R->isZero() || (L->isMinSignedValue() && R->isAllOnes()))
      return std::nullopt;
    if (Exact && !L->srem(*R).isZero())
      return std::nullopt;
    return std::optional<APInt>(L->sdiv(*R));
  case Instruction::SRem:
    return R->isZero() || (L->isMinSignedValue() && R->isAllOnes())
               ? std::nullopt
               : std::optional<APInt>(L->srem(*R));
  default:
    return std::nullopt;
  }
}

/// Compute the cumulative byte offset of a GEP. Every index must be a
/// ConstantInt, or fold to one under \p Assumed.
static std::optional<uint64_t>
computeGEPOffset(const GEPOperator *GEP, const DataLayout &DL,
                 const AssumedArgMap &Assumed, bool *UsedAssumption = nullptr) {
  SmallVector<Value *, 4> IdxList;
  for (auto I = GEP->idx_begin(), E = GEP->idx_end(); I != E; ++I) {
    if (isa<ConstantInt>(*I)) {
      IdxList.push_back(*I);
      continue;
    }
    if (Assumed.empty())
      return std::nullopt;
    auto Folded = evalWithAssumed(*I, Assumed, 0);
    if (!Folded)
      return std::nullopt;
    if (UsedAssumption)
      *UsedAssumption = true;
    // Materialized only to be handed to getIndexedOffsetInType below. Constants
    // are uniqued in the context, not inserted anywhere: the GEP still holds
    // its original, non-constant index operand.
    IdxList.push_back(ConstantInt::get(GEP->getContext(), *Folded));
  }
  return DL.getIndexedOffsetInType(GEP->getSourceElementType(), IdxList);
}

/// Walk a GEP chain from the load's pointer operand down to the root
/// global variable, accumulating the total byte offset. All GEP indices
/// must be constants (already folded by InstCombine after param substitution)
/// or fold to constants under \p Assumed.
static std::optional<uint64_t>
accumulateFullOffset(const DataLayout &DL, const Value *PtrOp,
                     const AssumedArgMap &Assumed,
                     bool *UsedAssumption = nullptr) {
  APInt total(DL.getPointerSizeInBits(0), 0);

  while (PtrOp) {
    PtrOp = PtrOp->stripPointerCasts();
    if (isa<GlobalVariable>(PtrOp))
      break;

    auto *GEP = dyn_cast<GEPOperator>(PtrOp);
    if (!GEP)
      return std::nullopt;

    auto off = computeGEPOffset(GEP, DL, Assumed, UsedAssumption);
    if (!off)
      return std::nullopt;
    total += APInt(total.getBitWidth(), *off);

    PtrOp = GEP->getPointerOperand();
  }

  return total.getZExtValue();
}

static std::optional<uint64_t>
accumulateArgumentOffset(const DataLayout &DL, const Value *PtrOp,
                         const Argument *Root, const AssumedArgMap &Assumed) {
  APInt Total(DL.getPointerSizeInBits(0), 0);
  while (PtrOp) {
    PtrOp = PtrOp->stripPointerCasts();
    if (PtrOp == Root)
      return Total.getZExtValue();
    auto *GEP = dyn_cast<GEPOperator>(PtrOp);
    if (!GEP)
      return std::nullopt;
    auto Off = computeGEPOffset(GEP, DL, Assumed);
    if (!Off)
      return std::nullopt;
    Total += APInt(Total.getBitWidth(), *Off);
    PtrOp = GEP->getPointerOperand();
  }
  return std::nullopt;
}

static std::optional<uint64_t>
getBoundPointerOffset(const Value *V, const AssumedArgMap &BoundArguments,
                      const DataLayout &DL, const AssumedArgMap &Assumed) {
  const Argument *Root = findRootArgument(V);
  if (!Root)
    return std::nullopt;
  auto It = BoundArguments.find(Root);
  if (It == BoundArguments.end())
    return std::nullopt;
  auto Relative = accumulateArgumentOffset(DL, V, Root, Assumed);
  if (!Relative ||
      It->second > std::numeric_limits<uint64_t>::max() - *Relative)
    return std::nullopt;
  return It->second + *Relative;
}

static Function *getDirectCallee(CallBase &CB) {
  return dyn_cast<Function>(CB.getCalledOperand()->stripPointerCasts());
}

/// A bound pointer may cross a call edge only when the callee is itself an
/// EJIT entry with the same dimension in its cache identity. Without this
/// contract, a private helper could be specialized in the caller's module
/// even though it has no independently addressable cell version.
static std::optional<unsigned>
getMatchingBoundEntryDimensionArg(const Function &F,
                                  StringRef BoundPeriodName) {
  if (!hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY))
    return std::nullopt;

  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  std::optional<unsigned> MatchingArg;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 3)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    auto *Period = dyn_cast<MDString>(Sub->getOperand(1));
    auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
    if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR_IND || !Period ||
        Period->getString() != BoundPeriodName)
      continue;
    uint64_t ArgIndex = Idx ? Idx->getZExtValue() : 0;
    if (!Idx || ArgIndex >= F.arg_size() ||
        !F.getArg(static_cast<unsigned>(ArgIndex))->getType()->isIntegerTy())
      return std::nullopt;
    if (MatchingArg)
      return std::nullopt;
    MatchingArg = static_cast<unsigned>(ArgIndex);
  }
  return MatchingArg;
}

/// Strip integer casts from a dimension value. preReplacePeriodIndices leaves
/// the Argument object in place, but replaces all of its uses with a constant;
/// before that pass, direct calls commonly carry the caller's formal through a
/// zext/trunc. Both forms are accepted below.
static const Value *stripDimensionCasts(const Value *V) {
  while (V) {
    const Value *Stripped = V->stripPointerCasts();
    if (Stripped != V) {
      V = Stripped;
      continue;
    }

    auto *Op = dyn_cast<Operator>(V);
    if (!Op || Op->getNumOperands() != 1 || !Op->getType()->isIntegerTy() ||
        !Op->getOperand(0)->getType()->isIntegerTy())
      break;
    switch (Op->getOpcode()) {
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::BitCast:
      V = Op->getOperand(0);
      continue;
    default:
      break;
    }
    break;
  }
  return V;
}

static bool isSpecializedDimensionConstant(const Value *V, uint8_t Expected) {
  V = stripDimensionCasts(V);
  auto *CI = dyn_cast_or_null<ConstantInt>(V);
  if (!CI || CI->getValue().getActiveBits() > 8)
    return false;
  return CI->getZExtValue() == Expected;
}

static bool
callUsesSameBoundDimension(const CallBase &CB, StringRef BoundPeriodName,
                           unsigned CalleeDimensionArg,
                           std::optional<uint8_t> ExpectedInstance) {
  const Function *Caller = CB.getFunction();
  auto CallerDimension =
      getMatchingBoundEntryDimensionArg(*Caller, BoundPeriodName);
  if (!CallerDimension || CalleeDimensionArg >= CB.arg_size())
    return false;

  const Value *Actual = CB.getArgOperand(CalleeDimensionArg);
  const Argument *CallerArg = Caller->getArg(*CallerDimension);
  if (stripDimensionCasts(Actual) == CallerArg)
    return true;
  return ExpectedInstance &&
         isSpecializedDimensionConstant(Actual, *ExpectedInstance);
}

/// Collect the ejit_free_dim parameters of every function in the module and map
/// each to the witness its addresses are evaluated at.
///
/// The witness is 0, unconditionally. It is not a tuning knob: the attribute
/// asserts the may_const data does not vary with the parameter, so every legal
/// value reads the same field values. The caller also guarantees that the fixed
/// witness 0 names a valid address. Reading through element 0 is what "drop
/// the `+ slotNo % 5` term" means once the address is actually computed.
///
/// Matching is per function rather than entry-only because an argument index is
/// meaningful only against the function it was recorded on, and CodeGen records
/// each parameter on its own function. A callee that was not annotated is
/// simply absent from the map; the assumption does not propagate across a call
/// that survived inlining.
void EJitStructFieldPass::initFreeDimAssumptions(Module &M) {
  freeDimArgs_.clear();
  for (Function &F : M) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;
    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 3)
        continue;
      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_FREE_DIM)
        continue;
      auto *IdxC = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
      if (!IdxC)
        continue;
      uint64_t ArgIdx = IdxC->getZExtValue();
      if (ArgIdx >= F.arg_size())
        continue;
      Argument *Arg = F.getArg(static_cast<unsigned>(ArgIdx));
      if (!Arg->getType()->isIntegerTy())
        continue;
      freeDimArgs_[Arg] = 0;
    }
  }
  EJIT_DIAG_VERBOSE("struct-field free dims: %zu", freeDimArgs_.size());
}

void EJitStructFieldPass::initBoundArgumentPropagation(Module &M) {
  boundStates_.clear();
  if (boundPointers_.empty())
    return;

  // Bound-pointer propagation decides which callee formals alias the borrowed
  // object, and that has to hold for EVERY call. A free-dim assumption is only
  // valid for the value being read at one address, so it must not be used to
  // prove an argument relationship: two calls whose offsets differ in the free
  // parameter would map one formal to one of them. Address computation for a
  // may_const LOAD does use the assumption — see run().
  const AssumedArgMap NoAssumed;
  const DataLayout &DL = M.getDataLayout();
  SmallVector<CallBase *, 32> DirectCalls;
  DenseMap<const Function *, SmallVector<CallBase *, 4>> CallsByCallee;
  for (Function &Caller : M) {
    if (Caller.isDeclaration())
      continue;
    for (BasicBlock &BB : Caller)
      for (Instruction &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        Function *Callee = getDirectCallee(*CB);
        if (!Callee || Callee->isDeclaration())
          continue;
        DirectCalls.push_back(CB);
        CallsByCallee[Callee].push_back(CB);
      }
  }

  for (const EJitBoundPointerView &View : boundPointers_) {
    if (!View.rawPtr || !View.size)
      continue;

    Function *Root = nullptr;
    if (!boundRootFunction_.empty()) {
      Root = M.getFunction(boundRootFunction_);
    } else {
      // Infer a root only when this argument index identifies one function.
      // With multiple bound pointers the same function is intentionally found
      // once per distinct metadata argument.
      for (Function &F : M) {
        if (!functionBindsArgument(F, View.argIndex))
          continue;
        if (Root) {
          Root = nullptr;
          break;
        }
        Root = &F;
      }
    }
    if (!Root || Root->isDeclaration() || View.argIndex >= Root->arg_size() ||
        !functionBindsArgument(*Root, View.argIndex))
      continue;

    Argument *RootArgument = Root->getArg(View.argIndex);
    if (!RootArgument->getType()->isPointerTy())
      continue;

    BoundPointerState State;
    State.view = View;
    State.boundArguments[RootArgument] = 0;
    StringRef BoundPeriodName;
    if (MDNode *BoundMD = getBoundArgumentMetadata(*Root, View.argIndex)) {
      if (auto *Period = dyn_cast<MDString>(BoundMD->getOperand(1)))
        BoundPeriodName = Period->getString();
      for (unsigned I = 4; I < BoundMD->getNumOperands(); ++I) {
        auto *Field = dyn_cast<MDNode>(BoundMD->getOperand(I));
        if (!Field || Field->getNumOperands() != 2)
          continue;
        auto *Offset = mdconst::dyn_extract<ConstantInt>(Field->getOperand(0));
        auto *Size = mdconst::dyn_extract<ConstantInt>(Field->getOperand(1));
        if (Offset && Size)
          State.mayConstFields.push_back(
              {Offset->getZExtValue(), Size->getZExtValue()});
      }
    }

    if (!BoundPeriodName.empty()) {
      bool Changed;
      do {
        Changed = false;
        for (CallBase *CB : DirectCalls) {
          Function *Callee = getDirectCallee(*CB);
          auto CalleeDimension =
              getMatchingBoundEntryDimensionArg(*Callee, BoundPeriodName);
          if (!CalleeDimension)
            continue;
          if (!callUsesSameBoundDimension(
                  *CB, BoundPeriodName, *CalleeDimension,
                  View.periodInstance == std::numeric_limits<uint32_t>::max()
                      ? std::nullopt
                      : std::optional<uint8_t>(
                            static_cast<uint8_t>(View.periodInstance))))
            continue;
          unsigned Count =
              std::min<unsigned>(CB->arg_size(), Callee->arg_size());
          for (unsigned ArgIndex = 0; ArgIndex < Count; ++ArgIndex) {
            Argument *Formal = Callee->getArg(ArgIndex);
            if (!Formal->getType()->isPointerTy())
              continue;
            auto Offset =
                getBoundPointerOffset(CB->getArgOperand(ArgIndex),
                                      State.boundArguments, DL, NoAssumed);
            if (!Offset || *Offset >= View.size)
              continue;
            if (!hasMatchingBoundArgumentContract(
                    *Callee, ArgIndex, BoundPeriodName, *Offset, View.size,
                    State.mayConstFields))
              continue;
            Changed |= State.boundArguments.try_emplace(Formal, *Offset).second;
          }
        }
      } while (Changed);

      // Every mapped helper formal must be reached only through calls that
      // carry both the same bound object and the same period identity. This
      // fixed-point prune makes a single ambiguous edge conservatively remove
      // the whole downstream chain.
      do {
        Changed = false;
        SmallVector<const Argument *, 8> Invalid;
        for (const auto &[Formal, ExpectedOffset] : State.boundArguments) {
          if (Formal == RootArgument)
            continue;
          const Function *Callee = Formal->getParent();
          if (Callee->hasAddressTaken()) {
            Invalid.push_back(Formal);
            continue;
          }
          auto CalleeDimension =
              getMatchingBoundEntryDimensionArg(*Callee, BoundPeriodName);
          if (!CalleeDimension ||
              !hasMatchingBoundArgumentContract(
                  *Callee, Formal->getArgNo(), BoundPeriodName, ExpectedOffset,
                  View.size, State.mayConstFields)) {
            Invalid.push_back(Formal);
            continue;
          }
          bool SawCall = false;
          bool AllMatch = true;
          auto CallsIt = CallsByCallee.find(Callee);
          if (CallsIt != CallsByCallee.end()) {
            for (CallBase *CB : CallsIt->second) {
              SawCall = true;
              unsigned ArgIndex = Formal->getArgNo();
              if (ArgIndex >= CB->arg_size()) {
                AllMatch = false;
                continue;
              }
              if (!callUsesSameBoundDimension(
                      *CB, BoundPeriodName, *CalleeDimension,
                      View.periodInstance ==
                              std::numeric_limits<uint32_t>::max()
                          ? std::nullopt
                          : std::optional<uint8_t>(
                                static_cast<uint8_t>(View.periodInstance)))) {
                AllMatch = false;
                continue;
              }
              auto ActualOffset =
                  getBoundPointerOffset(CB->getArgOperand(ArgIndex),
                                        State.boundArguments, DL, NoAssumed);
              if (!ActualOffset || *ActualOffset != ExpectedOffset)
                AllMatch = false;
            }
          }
          if (!SawCall || !AllMatch)
            Invalid.push_back(Formal);
        }
        for (const Argument *Formal : Invalid)
          Changed |= State.boundArguments.erase(Formal);
      } while (Changed);
    }

    boundStates_.push_back(std::move(State));
  }
}

static bool
isBoundMayConstLoad(LoadInst *LI, const AssumedArgMap &BoundArguments,
                    ArrayRef<std::pair<uint64_t, uint64_t>> MayConstFields,
                    const DataLayout &DL, const AssumedArgMap &Assumed) {
  if (LI->isVolatile() || LI->isAtomic())
    return false;
  auto Offset = getBoundPointerOffset(LI->getPointerOperand(), BoundArguments,
                                      DL, Assumed);
  if (!Offset)
    return false;
  if (LI->hasMetadata(MD_EJIT_MAY_CONST))
    return true;

  TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
  if (AccessSize.isScalable())
    return false;
  for (const auto &[Begin, Size] : MayConstFields)
    if (*Offset >= Begin && *Offset - Begin <= Size &&
        AccessSize.getFixedValue() <= Size - (*Offset - Begin))
      return true;
  return false;
}

/// Check whether a load is (or can be treated as) a may_const access.
static bool
isMayConstLoad(const LoadInst *LI, const MayConstOffsetMap &mayConstFieldMap,
               const DataLayout &DL) {
  // Never substituted. Checked ahead of the metadata, not just ahead of the
  // fallback, so that a pass which copies !ejit.may_const onto a volatile or
  // atomic load cannot defeat the frontend's exclusion.
  if (LI->isVolatile() || LI->isAtomic())
    return false;

  if (LI->hasMetadata(MD_EJIT_MAY_CONST))
    return true;

  // v1.7 fallback for loads whose marker an earlier pass dropped. The recorded
  // offsets are element-relative, so match on the field coordinate rather than
  // the total offset from the global, and require the access to fit inside the
  // field it starts at.
  const GlobalVariable *RootGV = nullptr;
  auto Off = ejitMayConstFieldOffset(LI->getPointerOperand(), DL, RootGV);
  if (Off && RootGV) {
    auto It = mayConstFieldMap.find(RootGV);
    if (It != mayConstFieldMap.end() && is_contained(It->second, *Off)) {
      TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
      if (!AccessSize.isScalable() &&
          ejitAccessFitsMayConstField(RootGV, *Off, AccessSize.getFixedValue(),
                                      DL))
        return true;
    }
  }
  return false;
}

#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
std::vector<EJitMayConstLoadSite>
EJitStructFieldPass::collectMayConstLoadSites(const Module &M) const {
  assert(mapsBuilt_ && "initFromModule() must precede may_const audit");
  std::vector<EJitMayConstLoadSite> Sites;
  const DataLayout &DL = M.getDataLayout();
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const auto *LI = dyn_cast<LoadInst>(&I);
        if (!LI || !isMayConstLoad(LI, mayConstFieldMap_, DL))
          continue;

        EJitMayConstLoadSite Site;
        Site.functionName = F.getName().str();
        if (MDNode *SiteMD = LI->getMetadata(MayConstAuditSiteMD))
          if (SiteMD->getNumOperands() == 1)
            if (auto *SiteID =
                    mdconst::dyn_extract<ConstantInt>(SiteMD->getOperand(0)))
              Site.siteId = SiteID->getZExtValue();
        const GlobalVariable *RootGV = nullptr;
        if (auto Offset =
                ejitMayConstFieldOffset(LI->getPointerOperand(), DL, RootGV)) {
          Site.fieldOffset = *Offset;
          Site.hasFieldOffset = true;
        }
        if (RootGV)
          Site.globalName = RootGV->getName().str();
        if (DebugLoc Loc = LI->getDebugLoc()) {
          Site.sourceFile = Loc->getFilename().str();
          Site.sourceLine = Loc.getLine();
          Site.sourceColumn = Loc.getCol();
        }
        Sites.push_back(std::move(Site));
      }
    }
  }
  return Sites;
}

std::vector<EJitMayConstLoadSite>
EJitStructFieldPass::instrumentMayConstLoadSites(Module &M) {
  std::vector<EJitMayConstLoadSite> Sites = collectMayConstLoadSites(M);
  if (Sites.empty())
    return Sites;

  LLVMContext &Ctx = M.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  ArrayType *CounterTy = ArrayType::get(I64, Sites.size());
  auto *Counters = new GlobalVariable(
      M, CounterTy, false, GlobalValue::ExternalLinkage,
      ConstantAggregateZero::get(CounterTy), MayConstCounterName);
  Counters->setAlignment(Align(8));

  const DataLayout &DL = M.getDataLayout();
  uint64_t SiteIndex = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *LI = dyn_cast<LoadInst>(&I);
        if (!LI || !isMayConstLoad(LI, mayConstFieldMap_, DL))
          continue;

        const uint64_t SiteID = SiteIndex + 1;
        LI->setMetadata(
            MayConstAuditSiteMD,
            MDNode::get(Ctx, ConstantAsMetadata::get(
                                 ConstantInt::get(I64, SiteID))));
        Sites[SiteIndex].siteId = SiteID;

        IRBuilder<> Builder(LI);
        Value *Counter = Builder.CreateInBoundsGEP(
            CounterTy, Counters,
            {Builder.getInt32(0), Builder.getInt64(SiteIndex)});
        Builder.CreateAtomicRMW(AtomicRMWInst::Add, Counter,
                                Builder.getInt64(1), Align(8),
                                AtomicOrdering::Monotonic);
        ++SiteIndex;
      }
    }
  }
  assert(SiteIndex == Sites.size() && "may_const site inventory changed");
  return Sites;
}

void EJitStructFieldPass::removeMayConstLoadInstrumentation(Module &M) {
  GlobalVariable *Counters = M.getGlobalVariable(MayConstCounterName);
  if (!Counters)
    return;

  SmallVector<AtomicRMWInst *, 16> Increments;
  for (Function &F : M)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *RMW = dyn_cast<AtomicRMWInst>(&I))
          if (getUnderlyingObject(RMW->getPointerOperand()) == Counters)
            Increments.push_back(RMW);

  for (AtomicRMWInst *RMW : Increments) {
    auto *Address = dyn_cast<Instruction>(RMW->getPointerOperand());
    RMW->eraseFromParent();
    if (Address)
      RecursivelyDeleteTriviallyDeadInstructions(Address);
  }
  assert(Counters->use_empty() && "may_const counter still has users");
  Counters->eraseFromParent();
}
#endif

//===----------------------------------------------------------------------===//
// Runtime value helpers
//===----------------------------------------------------------------------===//

/// Resolve the runtime base address of a global variable (array or static).
static void *resolveBase(const GlobalVariable *GV, const GVPeriodInfo &info,
                         PeriodArrayRegistry &reg) {
  if (info.isArray) {
    const auto *arrs = reg.getArrays(info.periodName);
    if (!arrs || arrs->empty())
      return nullptr;
    if (arrs->size() == 1)
      return arrs->front().baseAddr;
    const auto *paInfo = reg.getArrayInfo(GV->getName().str());
    return paInfo ? paInfo->baseAddr : nullptr;
  }
  return reg.getStaticVarAddr(GV->getName().str());
}

/// Create an LLVM Constant from raw memory bytes.
static Constant *createConstantFromMemory(const void *addr, Type *Ty,
                                          const DataLayout &DL) {
  LLVMContext &Ctx = Ty->getContext();
  unsigned byteSize = DL.getTypeStoreSize(Ty);

  if (Ty->isIntegerTy()) {
    // Only integers that fit in a single 64-bit word are materialized here.
    // Wider integers (e.g. __int128, or _BitInt(N) with N > 64) are left
    // un-substituted.
    if (byteSize > 8)
      return nullptr;
    uint64_t raw = 0;
    std::memcpy(&raw, addr, byteSize);
    if (!DL.isLittleEndian())
      raw >>= (8 - byteSize) * 8;
    return ConstantInt::get(Ty, APInt(byteSize * 8, raw));
  }
  if (Ty->isFloatTy()) {
    float v;
    std::memcpy(&v, addr, sizeof(v));
    return ConstantFP::get(Ty, v);
  }
  if (Ty->isDoubleTy()) {
    double v;
    std::memcpy(&v, addr, sizeof(v));
    return ConstantFP::get(Ty, v);
  }
  if (Ty->isPointerTy()) {
    const unsigned PointerSize =
        DL.getPointerSize(Ty->getPointerAddressSpace());
    if (PointerSize == 0 || PointerSize > sizeof(uint64_t))
      return nullptr;

    uint64_t raw = 0;
    const auto *Bytes = static_cast<const uint8_t *>(addr);
    if (DL.isLittleEndian()) {
      std::memcpy(&raw, Bytes, PointerSize);
    } else {
      for (unsigned I = 0; I < PointerSize; ++I)
        raw = (raw << 8) | Bytes[I];
    }
    return ConstantExpr::getIntToPtr(
        ConstantInt::get(IntegerType::get(Ctx, PointerSize * 8), raw), Ty);
  }
  return nullptr;
}

/// Replace a load of a pointer-valued period global with the registered
/// pointee address. Pointer-form ejit_period/ejit_period_arr globals are
/// registered by the address of their pointer slot, so resolveBase() returns
/// the slot and createConstantFromMemory() performs the required dereference.
///
/// This load is intentionally not required to carry !ejit.may_const: the
/// pointer is the address root of the period object, while the annotation
/// belongs to fields inside that object. Materializing the root lets IPSCCP
/// propagate it through a non-inlined helper, after which the normal
/// may_const-field replacement can resolve the field load.
static Constant *
tryReplacePeriodPointerBase(LoadInst *LI, const GVPeriodMap &gvMap,
                            PeriodArrayRegistry &reg, const DataLayout &DL) {
  if (LI->isVolatile() || LI->isAtomic() || !LI->getType()->isPointerTy())
    return nullptr;

  auto *GV = dyn_cast<GlobalVariable>(
      LI->getPointerOperand()->stripPointerCasts());
  if (!GV || !GV->getValueType()->isPointerTy())
    return nullptr;

  auto It = gvMap.find(GV);
  if (It == gvMap.end())
    return nullptr;

  void *PointerSlot = resolveBase(GV, It->second, reg);
  if (!PointerSlot)
    return nullptr;
  return createConstantFromMemory(PointerSlot, LI->getType(), DL);
}

/// Check the address addition rule used by a GEP with the nusw flag. The
/// current address is truncated to the pointer index type and interpreted as
/// unsigned, while the offset is interpreted as signed. This is deliberately
/// not APInt::sadd_ov: the address operand is unsigned even when the offset is
/// negative.
static bool absoluteAddressAddSignedOffsetNoWrap(const APInt &Address,
                                                 const APInt &Offset) {
  assert(Address.getBitWidth() == Offset.getBitWidth());
  if (Offset.isNegative()) {
    APInt Magnitude = -Offset;
    return !Address.ult(Magnitude);
  }
  APInt Max = APInt::getMaxValue(Address.getBitWidth());
  return Address.ule(Max - Offset);
}

/// Compute one GEP's byte offset without calling DataLayout's int64 helper
/// until after the index arithmetic has been checked. The latter uses signed
/// int64 multiplication internally, so a hostile constant/index can otherwise
/// overflow before the caller gets a chance to reject it.
///
/// When \p BaseAddress is supplied, also check every pointer-index arithmetic
/// obligation carried by the GEP and optionally return the address after each
/// individual index. This matters because a valid final address does not make
/// an earlier flagged step valid.
static std::optional<APInt>
computeAbsoluteGEPOffset(const GEPOperator *GEP, const DataLayout &DL,
                        const AssumedArgMap &Assumed,
                        const APInt *BaseAddress = nullptr,
                        SmallVectorImpl<APInt> *IntermediateAddresses = nullptr,
                        bool *HasNonZeroIndex = nullptr,
                        bool *UsedAssumption = nullptr) {
  constexpr unsigned HostPointerBits = sizeof(uintptr_t) * 8;
  if (!GEP->getType()->isPointerTy() ||
      DL.getIndexTypeSizeInBits(GEP->getType()) != HostPointerBits)
    return std::nullopt;

  if (IntermediateAddresses)
    IntermediateAddresses->clear();
  if (HasNonZeroIndex)
    *HasNonZeroIndex = false;

  const bool HasNUSW = GEP->hasNoUnsignedSignedWrap();
  const bool HasNUW = GEP->hasNoUnsignedWrap();

  SmallVector<APInt, 4> Indices;
  for (auto I = GEP->idx_begin(), E = GEP->idx_end(); I != E; ++I) {
    if (const auto *CI = dyn_cast<ConstantInt>(*I)) {
      Indices.push_back(CI->getValue());
      continue;
    }
    if (Assumed.empty())
      return std::nullopt;
    auto Folded = evalWithAssumed(*I, Assumed, 0);
    if (!Folded || Folded->getBitWidth() > 64)
      return std::nullopt;
    if (UsedAssumption)
      *UsedAssumption = true;
    Indices.push_back(*Folded);
  }

  APInt Result(HostPointerBits, 0);
  APInt OffsetWithoutBase(HostPointerBits, 0);
  APInt Current = BaseAddress ? BaseAddress->zextOrTrunc(HostPointerBits)
                              : APInt(HostPointerBits, 0);
  auto IndexIt = Indices.begin();
  for (auto GTI = gep_type_begin(GEP), GTE = gep_type_end(GEP);
       GTI != GTE; ++GTI, ++IndexIt) {
    if (IndexIt == Indices.end())
      return std::nullopt;
    const APInt &RawIndex = *IndexIt;
    if (HasNonZeroIndex && !RawIndex.isZero())
      *HasNonZeroIndex = true;

    APInt Contribution(HostPointerBits, 0);
    if (StructType *STy = GTI.getStructTypeOrNull()) {
      if (RawIndex.getActiveBits() > 32)
        return std::nullopt;
      const uint64_t FieldNo = RawIndex.getZExtValue();
      if (FieldNo >= STy->getNumElements())
        return std::nullopt;
      const uint64_t FieldOffset =
          DL.getStructLayout(STy)->getElementOffset(FieldNo);
      if (FieldOffset > static_cast<uint64_t>(INT64_MAX))
        return std::nullopt;
      Contribution = APInt(HostPointerBits, FieldOffset);
    } else {
      TypeSize StrideSize = DL.getTypeAllocSize(GTI.getIndexedType());
      if (StrideSize.isScalable())
        return std::nullopt;
      const uint64_t Stride = StrideSize.getFixedValue();
      if (RawIndex.getBitWidth() > HostPointerBits) {
        APInt Truncated = RawIndex.trunc(HostPointerBits);
        if (HasNUW && Truncated.zext(RawIndex.getBitWidth()) != RawIndex)
          return std::nullopt;
        if (HasNUSW && Truncated.sext(RawIndex.getBitWidth()) != RawIndex)
          return std::nullopt;
      }

      APInt Index = RawIndex.sextOrTrunc(HostPointerBits);
      APInt StrideValue(HostPointerBits, Stride);
      bool UnsignedMulOverflow = false;
      bool SignedMulOverflow = false;
      Contribution = Index * StrideValue;
      (void)Index.umul_ov(StrideValue, UnsignedMulOverflow);
      (void)Index.smul_ov(StrideValue, SignedMulOverflow);
      if ((HasNUW && UnsignedMulOverflow) ||
          (HasNUSW && SignedMulOverflow))
        return std::nullopt;
    }

    bool UnsignedOffsetOverflow = false;
    bool SignedOffsetOverflow = false;
    (void)OffsetWithoutBase.uadd_ov(Contribution, UnsignedOffsetOverflow);
    (void)OffsetWithoutBase.sadd_ov(Contribution, SignedOffsetOverflow);
    if ((HasNUW && UnsignedOffsetOverflow) ||
        (HasNUSW && SignedOffsetOverflow))
      return std::nullopt;

    if (BaseAddress) {
      bool UnsignedAddressOverflow = false;
      (void)Current.uadd_ov(Contribution, UnsignedAddressOverflow);
      if (HasNUW && UnsignedAddressOverflow)
        return std::nullopt;
      if (HasNUSW &&
          !absoluteAddressAddSignedOffsetNoWrap(Current, Contribution))
        return std::nullopt;
      Current = Current + Contribution;
      if (IntermediateAddresses)
        IntermediateAddresses->push_back(Current);
    }

    bool ResultOverflow = false;
    Result = Result.sadd_ov(Contribution, ResultOverflow);
    if (ResultOverflow)
      return std::nullopt;
    OffsetWithoutBase = OffsetWithoutBase + Contribution;
  }
  return Result;
}

/// Walk GEPs down to a constant inttoptr root and evaluate their offset under
/// the supplied witness map. This deliberately leaves the IR untouched: the
/// live GEP is still used by stores and by callers after only the authorized
/// load result is replaced.
static bool hasIntermediateAddressSpaceCast(const Value *Ptr) {
  SmallPtrSet<const Value *, 8> Seen;
  const Value *V = Ptr;
  while (V && Seen.insert(V).second) {
    if (isa<AddrSpaceCastOperator>(V))
      return true;
    if (const auto *GEP = dyn_cast<GEPOperator>(V)) {
      V = GEP->getPointerOperand();
      continue;
    }
    if (const auto *BC = dyn_cast<BitCastOperator>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    break;
  }
  return false;
}

struct AbsoluteGEPChain {
  APInt RootAddress;
  SmallVector<const GEPOperator *, 4> LeafToRoot;

  explicit AbsoluteGEPChain(unsigned PointerBits)
      : RootAddress(PointerBits, 0) {}
};

static std::optional<AbsoluteGEPChain>
collectAbsoluteGEPChain(const Value *Ptr, const DataLayout &DL) {
  constexpr unsigned HostPointerBits = sizeof(uintptr_t) * 8;
  if (!Ptr || !Ptr->getType()->isPointerTy() ||
      Ptr->getType()->getPointerAddressSpace() != 0 ||
      DL.getIndexTypeSizeInBits(Ptr->getType()) != HostPointerBits ||
      hasIntermediateAddressSpaceCast(Ptr))
    return std::nullopt;

  AbsoluteGEPChain Chain(HostPointerBits);
  const Value *V = Ptr;
  while (V) {
    V = V->stripPointerCasts();
    auto *GEP = dyn_cast<GEPOperator>(V);
    if (!GEP)
      break;
    Chain.LeafToRoot.push_back(GEP);
    V = GEP->getPointerOperand();
  }

  V = V ? V->stripPointerCasts() : nullptr;
  auto *IntToPtr = dyn_cast_or_null<ConstantExpr>(V);
  if (!IntToPtr || IntToPtr->getOpcode() != Instruction::IntToPtr ||
      !IntToPtr->getType()->isPointerTy() ||
      IntToPtr->getType()->getPointerAddressSpace() != 0)
    return std::nullopt;
  auto *AddressInt = dyn_cast<ConstantInt>(IntToPtr->getOperand(0));
  if (!AddressInt || AddressInt->getValue().getActiveBits() > HostPointerBits)
    return std::nullopt;
  Chain.RootAddress = AddressInt->getValue().zextOrTrunc(HostPointerBits);
  return Chain;
}

static std::optional<APInt>
getAbsoluteTarget(const Value *Ptr, const DataLayout &DL,
                 const AssumedArgMap &Assumed, bool *UsedAssumption = nullptr) {
  constexpr unsigned HostPointerBits = sizeof(uintptr_t) * 8;
  auto Chain = collectAbsoluteGEPChain(Ptr, DL);
  if (!Chain)
    return std::nullopt;

  APInt Offset(HostPointerBits, 0);
  APInt Current = Chain->RootAddress;
  for (auto It = Chain->LeafToRoot.rbegin(); It != Chain->LeafToRoot.rend();
       ++It) {
    const GEPOperator *GEP = *It;
    bool GEPUsedAssumption = false;
    auto GEPOffset = computeAbsoluteGEPOffset(
        GEP, DL, Assumed, &Current, nullptr, nullptr, &GEPUsedAssumption);
    if (!GEPOffset)
      return std::nullopt;
    bool Overflow = false;
    Offset = Offset.sadd_ov(*GEPOffset, Overflow);
    if (Overflow)
      return std::nullopt;
    if (UsedAssumption)
      *UsedAssumption |= GEPUsedAssumption;
    Current = Current + *GEPOffset;
  }

  APInt Target = Chain->RootAddress;
  if (Offset.isNegative()) {
    APInt Magnitude = -Offset;
    if (Target.ult(Magnitude))
      return std::nullopt;
    Target -= Magnitude;
  } else {
    bool Overflow = false;
    Target = Target.uadd_ov(Offset, Overflow);
    if (Overflow)
      return std::nullopt;
  }
  return Target;
}

static bool hasAbsoluteAddressContract(const Function &F,
                                       StringRef PeriodName) {
  if (!hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY))
    return false;
  // The built-in static period has no dimension argument; all other periods
  // need the consuming entry's matching period identity and argument.
  return PeriodName == "static" ||
         getMatchingBoundEntryDimensionArg(F, PeriodName).has_value();
}

struct RegisteredAbsoluteObject {
  const uint8_t *base = nullptr;
  uint64_t size = 0;
  bool isArray = false;
};

static std::optional<RegisteredAbsoluteObject>
getRegisteredNonPointerObject(const GlobalVariable *GV,
                              const GVPeriodInfo &Info,
                              PeriodArrayRegistry &Reg,
                              const DataLayout &DL) {
  Type *ValueTy = GV->getValueType();
  if (!ValueTy || ValueTy->isPointerTy() || !ValueTy->isSized())
    return std::nullopt;
  TypeSize ObjectSize = DL.getTypeAllocSize(ValueTy);
  if (ObjectSize.isScalable() || !ObjectSize.getFixedValue())
    return std::nullopt;

  if (Info.isArray) {
    auto *ArrayTy = dyn_cast<ArrayType>(ValueTy);
    if (!ArrayTy || !Info.arraySize ||
        ArrayTy->getNumElements() != Info.arraySize)
      return std::nullopt;
    const PeriodArrayInfo *Registered =
        Reg.getArrayInfo(GV->getName().str());
    if (!Registered || Registered->periodName != Info.periodName ||
        Registered->arraySize != Info.arraySize || !Registered->baseAddr)
      return std::nullopt;
    return RegisteredAbsoluteObject{
        static_cast<const uint8_t *>(Registered->baseAddr),
        ObjectSize.getFixedValue(), true};
  }

  void *Address = Reg.getStaticVarAddr(GV->getName().str());
  if (!Address)
    return std::nullopt;
  return RegisteredAbsoluteObject{static_cast<const uint8_t *>(Address),
                                  ObjectSize.getFixedValue(), false};
}

static bool absoluteRangeContains(uintptr_t Base, uint64_t ObjectSize,
                                  uintptr_t Target, uint64_t AccessSize,
                                  uint64_t &Relative) {
  if (ObjectSize > std::numeric_limits<uintptr_t>::max() - Base ||
      Target < Base)
    return false;
  const uintptr_t End = Base + static_cast<uintptr_t>(ObjectSize);
  if (Target > End)
    return false;
  Relative = static_cast<uint64_t>(Target - Base);
  return AccessSize <= ObjectSize - Relative;
}

static bool absoluteAddressInObject(const APInt &Address, uintptr_t ObjectBase,
                                    uint64_t ObjectSize) {
  if (ObjectSize > std::numeric_limits<uintptr_t>::max() - ObjectBase)
    return false;
  const uintptr_t End = ObjectBase + static_cast<uintptr_t>(ObjectSize);
  const uintptr_t Value = static_cast<uintptr_t>(Address.getZExtValue());
  return Value >= ObjectBase && Value <= End;
}

/// Validate the root-to-leaf obligations of inbounds GEPs against the one
/// registered object being considered. A physically valid final address is
/// insufficient: every flagged intermediate address must stay within the
/// same allocation, and an inbounds GEP's base must already be in that
/// allocation (or one-past it).
static bool absoluteGEPChainFitsObject(const Value *Ptr, const DataLayout &DL,
                                       const AssumedArgMap &Assumed,
                                       uintptr_t ObjectBase,
                                       uint64_t ObjectSize) {
  auto Chain = collectAbsoluteGEPChain(Ptr, DL);
  if (!Chain)
    return false;

  APInt Current = Chain->RootAddress;
  for (auto It = Chain->LeafToRoot.rbegin(); It != Chain->LeafToRoot.rend();
       ++It) {
    const GEPOperator *GEP = *It;
    SmallVector<APInt, 4> IntermediateAddresses;
    bool HasNonZeroIndex = false;
    auto Offset = computeAbsoluteGEPOffset(
        GEP, DL, Assumed, &Current, &IntermediateAddresses,
        &HasNonZeroIndex);
    if (!Offset)
      return false;

    if (GEP->isInBounds() && HasNonZeroIndex) {
      if (!absoluteAddressInObject(Current, ObjectBase, ObjectSize))
        return false;
      for (const APInt &Address : IntermediateAddresses)
        if (!absoluteAddressInObject(Address, ObjectBase, ObjectSize))
          return false;
    }
    Current = Current + *Offset;
  }
  return true;
}

static bool absoluteFieldAuthorized(
    const GlobalVariable *GV, const GVPeriodInfo &Info,
    const MayConstOffsetMap &MayConstFieldMap, uint64_t Relative,
    uint64_t AccessSize, const DataLayout &DL) {
  auto FieldIt = MayConstFieldMap.find(GV);
  if (FieldIt == MayConstFieldMap.end())
    return false;
  uint64_t FieldOffset = Relative;
  if (Info.isArray) {
    auto *ArrayTy = dyn_cast<ArrayType>(GV->getValueType());
    if (!ArrayTy)
      return false;
    TypeSize ElementSize = DL.getTypeAllocSize(ArrayTy->getElementType());
    if (ElementSize.isScalable() || !ElementSize.getFixedValue())
      return false;
    FieldOffset %= ElementSize.getFixedValue();
  }
  return llvm::is_contained(FieldIt->second, FieldOffset) &&
         ejitAccessFitsMayConstField(GV, FieldOffset, AccessSize, DL);
}

/// Resolve a may_const load after IPSCCP/InstCombine has propagated a period
/// pointer into a callee and folded its GEP into an absolute address. For a
/// non-pointer registered object, accept only a unique, bounded and authorized
/// field whose complete root-to-leaf GEP witness is valid. Pointer-form objects
/// retain the old exact-field/constant-index path; their pointee extent is not
/// present in the registry, so a free-dim witness is never used for them.
static Constant *tryReplacePeriodAbsoluteAddress(
    LoadInst *LI, const Function &F, const GVPeriodMap &gvMap,
    const MayConstOffsetMap &mayConstFieldMap, PeriodArrayRegistry &reg,
    const DataLayout &DL, const AssumedArgMap &Assumed) {
  if (LI->isVolatile() || LI->isAtomic())
    return nullptr;
  TypeSize AccessTypeSize = DL.getTypeStoreSize(LI->getType());
  if (AccessTypeSize.isScalable() || !AccessTypeSize.getFixedValue())
    return nullptr;
  const uint64_t AccessSize = AccessTypeSize.getFixedValue();
  bool UsedAssumption = false;
  auto TargetValue = getAbsoluteTarget(LI->getPointerOperand(), DL, Assumed,
                                        &UsedAssumption);
  if (!TargetValue)
    return nullptr;
  const uintptr_t Target = static_cast<uintptr_t>(TargetValue->getZExtValue());

  unsigned Matches = 0;
  const uint8_t *Candidate = nullptr;
  for (const auto &[GV, Info] : gvMap) {
    if (GV->getValueType()->isPointerTy()) {
      // Preserve the pre-existing constant-address path for pointer-valued
      // periods. It deliberately does not require the helper itself to carry
      // the consuming entry's lifecycle metadata: the pointer materialization
      // and the old absolute-address fold are also used through non-inlined
      // helpers. The pointee's complete extent is unknown, so this path cannot
      // consume a free-dim witness.
      if (UsedAssumption || DL.getPointerSize(0) != sizeof(void *))
        continue;
      void *PointerSlot = resolveBase(GV, Info, reg);
      if (!PointerSlot)
        continue;
      void *Pointee = nullptr;
      std::memcpy(&Pointee, PointerSlot, sizeof(Pointee));
      const uintptr_t PointeeBase = reinterpret_cast<uintptr_t>(Pointee);
      if (!PointeeBase || Target < PointeeBase)
        continue;
      const uint64_t FieldOffset = Target - PointeeBase;
      auto FieldIt = mayConstFieldMap.find(GV);
      if (FieldIt == mayConstFieldMap.end() ||
          !llvm::is_contained(FieldIt->second, FieldOffset) ||
          AccessSize > std::numeric_limits<uintptr_t>::max() - Target)
        continue;
      Candidate = reinterpret_cast<const uint8_t *>(Target);
      ++Matches;
      continue;
    }

    // New absolute-address witness folding is only enabled when the
    // consuming function carries the complete entry/period contract. This is
    // intentionally separate from the legacy pointer branch above.
    if (!hasAbsoluteAddressContract(F, Info.periodName))
      continue;

    auto Object = getRegisteredNonPointerObject(GV, Info, reg, DL);
    if (!Object)
      continue;
    uint64_t Relative = 0;
    const uintptr_t ObjectBase = reinterpret_cast<uintptr_t>(Object->base);
    if (!absoluteRangeContains(ObjectBase, Object->size, Target, AccessSize,
                               Relative) ||
        !absoluteGEPChainFitsObject(LI->getPointerOperand(), DL, Assumed,
                                    ObjectBase, Object->size) ||
        !absoluteFieldAuthorized(GV, Info, mayConstFieldMap, Relative,
                                 AccessSize, DL))
      continue;
    Candidate = reinterpret_cast<const uint8_t *>(Target);
    ++Matches;
  }

  // The contract is a unique eligible registration, not unique physical
  // storage. Physically overlapping bytes are safe to use only when every
  // other registration is ineligible for this consuming entry's lifecycle and
  // authorized field; that alias/lifetime precondition is what prevents an
  // arbitrary map entry from winning. Cross-lifecycle or otherwise eligible
  // overlap remains ambiguous.
  if (Matches != 1)
    return nullptr;
  return createConstantFromMemory(Candidate, LI->getType(), DL);
}

static Constant *tryReplaceBoundPointer(LoadInst *LI, const uint8_t *Data,
                                        uint32_t Size,
                                        const AssumedArgMap &BoundArguments,
                                        const DataLayout &DL,
                                        const AssumedArgMap &Assumed) {
  if (!Data || !Size)
    return nullptr;
  auto Offset = getBoundPointerOffset(LI->getPointerOperand(), BoundArguments,
                                      DL, Assumed);
  TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
  if (!Offset || AccessSize.isScalable() || *Offset > Size ||
      AccessSize.getFixedValue() > Size - *Offset)
    return nullptr;
  return createConstantFromMemory(Data + *Offset, LI->getType(), DL);
}

//===----------------------------------------------------------------------===//
// Load replacement helpers — one per access pattern
//===----------------------------------------------------------------------===//

/// Is a byte offset derived from a free-dim witness inside \p GV's object?
///
/// The declared type of the AOT global is the extent the source could legally
/// index; a witness-derived offset outside it is not an address the program
/// would ever have formed. Unsized types cannot be checked, so they are
/// refused: this guard only ever runs when an assumption was used, where
/// refusing costs an optimization and accepting risks reading unrelated memory
/// (or faulting during specialization).
static bool offsetFitsInObject(const GlobalVariable *GV, uint64_t ByteOffset,
                               Type *AccessTy, const DataLayout &DL) {
  Type *ValueTy = GV->getValueType();
  if (!ValueTy || !ValueTy->isSized())
    return false;
  TypeSize ObjSize = DL.getTypeAllocSize(ValueTy);
  TypeSize AccessSize = DL.getTypeStoreSize(AccessTy);
  if (ObjSize.isScalable() || AccessSize.isScalable())
    return false;
  const uint64_t Obj = ObjSize.getFixedValue();
  const uint64_t Access = AccessSize.getFixedValue();
  // Written so a wrapped-negative ByteOffset fails rather than overflows.
  return Access <= Obj && ByteOffset <= Obj - Access;
}

/// Pattern 1: load directly from a GlobalVariable (scalar static variable).
static Constant *
tryReplaceDirectGV(LoadInst *LI, const GlobalVariable *GV,
                   const GVPeriodMap &gvMap, PeriodArrayRegistry &reg,
                   const DataLayout &DL) {
  auto it = gvMap.find(GV);
  if (it == gvMap.end())
    return nullptr;

  void *base = resolveBase(GV, it->second, reg);
  if (!base)
    return nullptr;

  return createConstantFromMemory(base, LI->getType(), DL);
}

/// Pattern 2: load via a GEP chain rooted at a GlobalVariable.
/// e.g. @g_cellCfg → GEP 0, idx → GEP 0, fieldIdx → load
static Constant *tryReplaceDirectGEP(LoadInst *LI, const Value *PtrOp,
                                     const GVPeriodMap &gvMap,
                                     PeriodArrayRegistry &reg,
                                     const DataLayout &DL,
                                     const AssumedArgMap &Assumed) {
  const GlobalVariable *GV = findRootGV(PtrOp);
  if (!GV)
    return nullptr;

  auto it = gvMap.find(GV);
  if (it == gvMap.end())
    return nullptr;

  bool UsedAssumption = false;
  auto byteOffset = accumulateFullOffset(DL, PtrOp, Assumed, &UsedAssumption);
  if (!byteOffset)
    return nullptr;

  // An offset the source computed is in bounds by construction. One computed
  // at the witness is not: ejit_free_dim asserts that the marked fields hold
  // the same value for every value the parameter takes, which says nothing
  // about whether the witness is itself a value the parameter takes. For
  // `arr[slot - 1]` with slot in 1..5 every valid element may agree while the
  // witness names arr[-1] -- a negative offset, which arrives here as a huge
  // unsigned and would read whatever precedes the object. Require the whole
  // access to lie inside the object before trusting the address.
  if (UsedAssumption && !offsetFitsInObject(GV, *byteOffset, LI->getType(), DL))
    return nullptr;

  void *base = resolveBase(GV, it->second, reg);
  if (!base)
    return nullptr;

  auto *fieldAddr = static_cast<const uint8_t *>(base) + *byteOffset;
  return createConstantFromMemory(fieldAddr, LI->getType(), DL);
}

/// Pattern 3: load via an indirect pointer — first load a pointer from a GV,
/// then GEP into the pointed-to data.
/// e.g. %ptr = load ptr, ptr @g_pCfg  → GEP %S, ptr %ptr, i32 0, i32 0
static Constant *
tryReplaceIndirect(LoadInst *LI, const Value *PtrOp,
                   const GVPeriodMap &gvMap, PeriodArrayRegistry &reg,
                   const DataLayout &DL) {
  // Deliberately takes no AssumedArgMap. The base here is a pointer read out
  // of a global at compile time, so the pointee's extent is unknown and a
  // witness-derived offset cannot be bounds checked the way
  // tryReplaceDirectGEP bounds one. Declining the assumption is the
  // conservative half of that trade: a constant index still resolves exactly
  // as before, and no address the program may never form is dereferenced.
  const AssumedArgMap Assumed;
  // Walk the GEP chain from the load's pointer operand to find
  // the base LoadInst that reads the pointer value from a GV.
  const Value *V = PtrOp;
  SmallVector<const GEPOperator *, 4> FieldGEPs;
  while (V) {
    V = V->stripPointerCasts();
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      FieldGEPs.push_back(GEP);
      V = GEP->getPointerOperand();
      continue;
    }
    break;
  }

  auto *BaseLoad = dyn_cast<LoadInst>(V);
  if (!BaseLoad)
    return nullptr;

  // Resolve the pointer-valued GV that BaseLoad reads from.
  const Value *LoadPtr = BaseLoad->getPointerOperand()->stripPointerCasts();
  const GlobalVariable *PtrGV = nullptr;
  uint64_t ptrArrayByteOff = 0;

  if (auto *DirectGV = dyn_cast<GlobalVariable>(LoadPtr)) {
    PtrGV = DirectGV;
  } else if (auto *PtrGEP = dyn_cast<GEPOperator>(LoadPtr)) {
    PtrGV = dyn_cast<GlobalVariable>(
        PtrGEP->getPointerOperand()->stripPointerCasts());
    if (PtrGV) {
      auto off = computeGEPOffset(PtrGEP, DL, Assumed);
      if (!off)
        return nullptr;
      ptrArrayByteOff = *off;
    }
  }
  if (!PtrGV)
    return nullptr;

  auto it = gvMap.find(PtrGV);
  if (it == gvMap.end())
    return nullptr;

  void *gvBase = resolveBase(PtrGV, it->second, reg);
  if (!gvBase)
    return nullptr;

  // Read the stored pointer: *(void**)(gvBase + ptrArrayByteOff)
  uintptr_t ptrSlot = reinterpret_cast<uintptr_t>(gvBase) + ptrArrayByteOff;
  void *dataBase = nullptr;
  std::memcpy(&dataBase, reinterpret_cast<void *>(ptrSlot), sizeof(void *));
  if (!dataBase)
    return nullptr;

  // Compute field offset from the GEPs past the pointer dereference.
  uint64_t fieldOff = 0;
  for (auto It = FieldGEPs.rbegin(); It != FieldGEPs.rend(); ++It) {
    auto off = computeGEPOffset(*It, DL, Assumed);
    if (!off)
      return nullptr;
    fieldOff += *off;
  }

  auto *fieldAddr = static_cast<const uint8_t *>(dataBase) + fieldOff;
  return createConstantFromMemory(fieldAddr, LI->getType(), DL);
}

//===----------------------------------------------------------------------===//
// Public interface
//===----------------------------------------------------------------------===//

#ifdef EJIT_DIAG_ENABLE
/// Classify why a may_const load was NOT replaced by any pattern, for
/// diagnostics. One EJIT_DIAG line per failed load. Reasons:
///   no-root-gv        — pointer not rooted at a GlobalVariable
///   (opaque/indirect) gv-not-in-map     — root GV has no ejit.metadata (not a
///   period var) base-unresolved   — GV is a period var but not registered at
///   runtime non-const-offset  — GEP index not folded to a constant
///   witness-out-of-bounds — folded only via an ejit_free_dim witness, and that
///                     address lies outside the object (see
///                     tryReplaceDirectGEP)
///   unsupported-type  — createConstantFromMemory cannot build the load type
static void logReplaceFailure(LoadInst *LI, const GVPeriodMap &gvMap,
                              PeriodArrayRegistry &reg, const DataLayout &DL,
                              const AssumedArgMap &Assumed) {
  Value *Ptr = LI->getPointerOperand();
  const GlobalVariable *GV = findRootGV(Ptr);
  if (!GV) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: no-root-gv");
    return;
  }
  auto it = gvMap.find(GV);
  if (it == gvMap.end()) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: gv-not-in-map gv=%s",
                      GV->getName().str().c_str());
    return;
  }
  if (!resolveBase(GV, it->second, reg)) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: base-unresolved gv=%s",
                      GV->getName().str().c_str());
    return;
  }
  bool UsedAssumption = false;
  auto Off = accumulateFullOffset(DL, Ptr, Assumed, &UsedAssumption);
  if (!Off) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: non-const-offset gv=%s",
                      GV->getName().str().c_str());
    return;
  }
  // Must mirror tryReplaceDirectGEP's guard, and must be tested BEFORE falling
  // through: accumulateFullOffset succeeds for a witness-derived address (it
  // returns the wrapped offset), so without this the rejection would be
  // reported as unsupported-type and send a reader looking at the load's type
  // instead of at the ejit_free_dim annotation that caused it.
  if (UsedAssumption && !offsetFitsInObject(GV, *Off, LI->getType(), DL)) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: witness-out-of-bounds "
                      "gv=%s off=%llu",
                      GV->getName().str().c_str(),
                      static_cast<unsigned long long>(*Off));
    return;
  }
  EJIT_DIAG_VERBOSE("  may_const load NOT replaced: unsupported-type gv=%s",
                    GV->getName().str().c_str());
}
#endif

// The preserved-dimension policy never falls back to the legacy pointer-base
// or absolute-address patterns. A failed proof costs a fold, not correctness.
void EJitStructFieldPass::setPreservedDimensions(
    const SpecializationContext &Ctx) {
  preserveDimensions_ = true;
  mapsBuilt_ = false;
  boundRootFunction_ = Ctx.fnName;
  preservedDimensions_.clear();
  preservedContextValid_ = Ctx.dimensions.size() <= EJIT_ICACHE_MAX_DIMS;
  if (!preservedContextValid_)
    return;
  for (const auto &Dim : Ctx.dimensions)
    preservedDimensions_.emplace_back(Dim.periodName, Dim.cellIdx);
}

std::optional<uint8_t>
EJitStructFieldPass::preservedInstance(StringRef Period) const {
  std::optional<uint8_t> Result;
  for (const auto &Dim : preservedDimensions_) {
    if (Dim.first != Period)
      continue;
    if (Result)
      return std::nullopt;
    Result = Dim.second;
  }
  return Result;
}

void EJitStructFieldPass::initPreservedDimensions(Module &M) {
  preservedArgs_.clear();
  preservedLoadArgs_.clear();
  preservedFunctions_.clear();
  if (!preserveDimensions_ || !preservedContextValid_)
    return;

  // Cold-path inference has a finite budget. Exhaustion retains the loads;
  // it must never re-enable legacy whole-parameter/pointer-base replacement.
  constexpr unsigned MaxFunctions = 256, MaxCalls = 4096, MaxArgs = 1024;
  Function *Root = M.getFunction(boundRootFunction_);
  if (!Root || Root->isDeclaration() || !Root->use_empty() ||
      preservedDimensions_.size() > EJIT_ICACHE_MAX_DIMS ||
      M.size() > MaxFunctions)
    return;
  MDNode *MD = Root->getMetadata(MD_EJIT_METADATA);
  if (!hasMDStringEntry(MD, TAG_EJIT_ENTRY))
    return;
  for (const auto &Dim : preservedDimensions_)
    if (Dim.first.empty() || !preservedInstance(Dim.first))
      return;

  DenseMap<const Function *, SmallVector<CallBase *, 4>> Callers;
  SmallVector<CallBase *, 32> Calls;
  unsigned NumArgs = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.arg_size() > MaxArgs - NumArgs)
      return;
    NumArgs += F.arg_size();
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          if (Calls.size() == MaxCalls)
            return;
          Calls.push_back(CB);
          if (Function *Callee = getDirectCallee(*CB))
            Callers[Callee].push_back(CB);
        }
  }

  // Seed only the selected entry. A same-named period in another function is
  // not proof of the actual argument arriving along an edge into that function.
  SmallVector<StringRef, 4> SeenPeriods;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 3)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR_IND)
      continue;
    auto *Period = dyn_cast<MDString>(Sub->getOperand(1));
    auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
    if (!Period || !Idx || Idx->getValue().getActiveBits() > 32 ||
        Idx->getZExtValue() >= Root->arg_size()) {
      preservedArgs_.clear();
      return;
    }
    auto Instance = preservedInstance(Period->getString());
    if (!Instance)
      continue;
    Argument *Arg = Root->getArg(static_cast<unsigned>(Idx->getZExtValue()));
    if (!Arg->getType()->isIntegerTy() || preservedArgs_.count(Arg) ||
        is_contained(SeenPeriods, Period->getString()) || freeDimArgs_.count(Arg) ||
        APInt(8, *Instance).getActiveBits() >
            Arg->getType()->getIntegerBitWidth()) {
      preservedArgs_.clear();
      return;
    }
    SeenPeriods.push_back(Period->getString());
    preservedArgs_[Arg] = *Instance;
  }

  // Establish a closed direct-call region. Address-taken or external helpers,
  // other entry boundaries, and callers outside the region are not evidence.
  // A referenced/recursive root is conservatively rejected above as well.
  preservedFunctions_.insert(Root);
  bool Changed;
  do {
    Changed = false;
    for (CallBase *CB : Calls) {
      Function *Callee = getDirectCallee(*CB);
      if (!Callee || Callee->isDeclaration() || !Callee->hasLocalLinkage() ||
          Callee->hasAddressTaken() || Callee->isVarArg() ||
          hasMDStringEntry(Callee->getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY) ||
          !preservedFunctions_.count(CB->getFunction()))
        continue;
      Changed |= preservedFunctions_.insert(Callee).second;
    }
  } while (Changed);
  do {
    Changed = false;
    SmallVector<const Function *, 8> Invalid;
    for (const Function *F : preservedFunctions_) {
      if (F == Root)
        continue;
      for (CallBase *CB : Callers[F])
        if (!preservedFunctions_.count(CB->getFunction())) {
          Invalid.push_back(F);
          break;
        }
    }
    for (const Function *F : Invalid)
      Changed |= preservedFunctions_.erase(F);
  } while (Changed);

  // Admit a formal only if every incoming edge proves the same integer value.
  // This monotone process never invents a value for an unknown/recursive cycle.
  // PR223 free_dim witnesses may authorize a LOAD evaluation, never this proof.
  unsigned EdgeEvaluationsLeft = 16384;
  do {
    Changed = false;
    for (const Function *F : preservedFunctions_) {
      if (F == Root)
        continue;
      for (const Argument &Arg : F->args()) {
        if (!Arg.getType()->isIntegerTy() || preservedArgs_.count(&Arg))
          continue;
        std::optional<APInt> Expected;
        bool Valid = !Callers[F].empty();
        for (CallBase *CB : Callers[F]) {
          if (!EdgeEvaluationsLeft) {
            preservedArgs_.clear();
            preservedFunctions_.clear();
            return;
          }
          --EdgeEvaluationsLeft;
          if (Arg.getArgNo() >= CB->arg_size() ||
              CB->getArgOperand(Arg.getArgNo())->getType() != Arg.getType()) {
            Valid = false;
            break;
          }
          auto Actual = evalWithAssumed(CB->getArgOperand(Arg.getArgNo()),
                                        preservedArgs_, 0);
          if (!Actual || Actual->getActiveBits() > 64 ||
              (Expected && *Expected != *Actual)) {
            Valid = false;
            break;
          }
          Expected = std::move(Actual);
        }
        if (Valid && Expected) {
          preservedArgs_[&Arg] = Expected->getZExtValue();
          Changed = true;
        }
      }
    }
  } while (Changed);

  preservedLoadArgs_ = preservedArgs_;
  for (const auto &Arg : freeDimArgs_)
    if (preservedFunctions_.count(Arg.first->getParent()))
      preservedLoadArgs_.try_emplace(Arg.first, Arg.second);
}

// Checked displacement calculation for the preserved policy. Negative steps
// are deliberately unsupported: an out-of-bounds intermediate inbounds GEP
// must not be laundered by a later negative GEP. No source IR is changed.
static std::optional<std::pair<const Value *, uint64_t>>
getPreservedOffset(const Value *Ptr, const DataLayout &DL,
                   const AssumedArgMap &Assumed) {
  if (DL.getPointerSizeInBits(0) != 64 || DL.getIndexSizeInBits(0) != 64 ||
      DL.isNonIntegralAddressSpace(0))
    return std::nullopt;
  APInt Total(64, 0);
  for (unsigned Depth = 0; Ptr && Depth != 16; ++Depth) {
    if (!Ptr->getType()->isPointerTy() ||
        Ptr->getType()->getPointerAddressSpace() != 0)
      return std::nullopt;
    if (isa<GlobalVariable>(Ptr) || isa<Argument>(Ptr))
      return std::make_pair(Ptr, Total.getZExtValue());
    if (const auto *Cast = dyn_cast<BitCastOperator>(Ptr)) {
      Ptr = Cast->getOperand(0);
      continue;
    }
    const auto *GEP = dyn_cast<GEPOperator>(Ptr);
    if (!GEP || GEP->getNumIndices() > 16)
      return std::nullopt;
    for (auto TI = gep_type_begin(GEP), TE = gep_type_end(GEP); TI != TE; ++TI) {
      auto Index = evalWithAssumed(TI.getOperand(), Assumed, 0);
      if (!Index || TI.isVector() || Index->isNegative() ||
          !Index->isSignedIntN(64))
        return std::nullopt;
      APInt Part(64, 0);
      if (TI.isStruct()) {
        if (Index->getActiveBits() > 32 ||
            Index->getZExtValue() >= TI.getStructType()->getNumElements())
          return std::nullopt;
        uint64_t Offset = DL.getStructLayout(TI.getStructType())->getElementOffset(
            static_cast<unsigned>(Index->getZExtValue()));
        if (Offset > INT64_MAX)
          return std::nullopt;
        Part = APInt(64, Offset);
      } else {
        if (!TI.getIndexedType()->isSized())
          return std::nullopt;
        TypeSize Stride = TI.getSequentialElementStride(DL);
        if (Stride.isScalable() || Stride.getFixedValue() > INT64_MAX)
          return std::nullopt;
        bool Overflow = false;
        Part = Index->sextOrTrunc(64).smul_ov(
            APInt(64, Stride.getFixedValue()), Overflow);
        if (Overflow)
          return std::nullopt;
      }
      bool Overflow = false;
      Total = Total.sadd_ov(Part, Overflow);
      if (Overflow)
        return std::nullopt;
    }
    Ptr = GEP->getPointerOperand();
  }
  return std::nullopt;
}

static Constant *readPreservedConstant(const uint8_t *Data, uint64_t Size,
                                        uint64_t Offset, Type *Ty,
                                        const DataLayout &DL) {
  if (!(Ty->isIntegerTy() && Ty->getIntegerBitWidth() <= 64) &&
      !Ty->isFloatTy() && !Ty->isDoubleTy())
    return nullptr;
  const unsigned Bytes = DL.getTypeStoreSize(Ty).getFixedValue();
  uint64_t Bits;
  if (!llvm::ejit::detail::readPreservedScalar(Data, Size, Offset, Bytes,
                                  DL.isLittleEndian(), Bits))
    return nullptr;
  if (Ty->isIntegerTy())
    return ConstantInt::get(Ty, APInt(Ty->getIntegerBitWidth(), Bits));
  const fltSemantics &Semantics =
      Ty->isFloatTy() ? APFloat::IEEEsingle() : APFloat::IEEEdouble();
  return ConstantFP::get(Ty->getContext(),
                         APFloat(Semantics, APInt(Bytes * 8, Bits)));
}

Constant *EJitStructFieldPass::tryReplacePreservedLoad(LoadInst *LI,
                                                     const DataLayout &DL) {
  auto Reject = [](const char *Reason) -> Constant * {
    (void)Reason;
    EJIT_DIAG_VERBOSE("preserved-dim load kept: %s", Reason);
    return nullptr;
  };
  if (!preservedFunctions_.count(LI->getFunction()) || LI->isVolatile() ||
      LI->isAtomic())
    return nullptr;
  bool Authorized = isMayConstLoad(LI, mayConstFieldMap_, DL);
  for (const BoundPointerState &State : boundStates_)
    Authorized |= isBoundMayConstLoad(LI, State.boundArguments,
                                      State.mayConstFields, DL,
                                      preservedLoadArgs_);
  if (!Authorized)
    return nullptr;
  Type *Ty = LI->getType();
  if (!(Ty->isIntegerTy() && Ty->getIntegerBitWidth() <= 64) &&
      !Ty->isFloatTy() && !Ty->isDoubleTy())
    return Reject("unsupported-scalar-type");
  auto Address =
      getPreservedOffset(LI->getPointerOperand(), DL, preservedLoadArgs_);
  if (!Address)
    return Reject("unknown-or-unbounded-address");
  const uint64_t Offset = Address->second;
  const uint64_t Bytes = DL.getTypeStoreSize(Ty).getFixedValue();

  if (const auto *GV = dyn_cast<GlobalVariable>(Address->first)) {
    auto It = gvPeriodMap_.find(GV);
    if (It == gvPeriodMap_.end() ||
        !isMayConstLoad(LI, mayConstFieldMap_, DL))
      return nullptr;
    const GVPeriodInfo &Info = It->second;
    auto Instance = preservedInstance(Info.periodName);
    if (!Instance)
      return Reject("missing-lifecycle-dependency");
    unsigned PeriodTags = 0;
    for (const MDOperand &Op : GV->getMetadata(MD_EJIT_METADATA)->operands())
      if (auto *Sub = dyn_cast<MDNode>(Op.get()))
        if (Sub->getNumOperands() >= 2)
          if (auto *Tag = dyn_cast<MDString>(Sub->getOperand(0)))
            PeriodTags += Tag->getString() == TAG_EJIT_PERIOD_ARR ||
                          Tag->getString() == TAG_EJIT_PERIOD;
    if (PeriodTags != 1 || !GV->getValueType()->isSized() ||
        GV->getValueType()->isPointerTy())
      return Reject("ambiguous-or-unbounded-registration");
    TypeSize Extent = DL.getTypeAllocSize(GV->getValueType());
    if (Extent.isScalable() || Extent.getFixedValue() > INT64_MAX)
      return Reject("unsupported-object-extent");
    const uint8_t *Base = nullptr;
    if (Info.isArray) {
      auto *AT = dyn_cast<ArrayType>(GV->getValueType());
      const auto *Registered = registry_.getArrayInfo(GV->getName().str());
      if (!AT || !Registered || Registered->periodName != Info.periodName ||
          AT->getNumElements() != Info.arraySize ||
          Registered->arraySize != Info.arraySize)
        return Reject("registration-shape-mismatch");
      TypeSize Stride = DL.getTypeAllocSize(AT->getElementType());
      if (Stride.isScalable() ||
          !detail::fitsPreservedElement(Offset, Bytes, Stride.getFixedValue(),
                                        Info.arraySize, *Instance))
        return Reject("untracked-element-or-out-of-bounds");
      Base = static_cast<const uint8_t *>(Registered->baseAddr);
    } else {
      if (*Instance != 0)
        return Reject("untracked-static-instance");
      Base = static_cast<const uint8_t *>(
          registry_.getStaticVarAddr(GV->getName().str()));
    }
    return readPreservedConstant(Base, Extent.getFixedValue(), Offset, Ty, DL);
  }

  // Keep existing bounded borrowed views. This phase adds no delayed/group
  // borrows and retains no object beyond the existing compile callback.
  const auto *Arg = dyn_cast<Argument>(Address->first);
  Function *Root = LI->getModule()->getFunction(boundRootFunction_);
  if (!Arg || !Root)
    return nullptr;
  for (const BoundPointerState &State : boundStates_) {
    auto It = State.boundArguments.find(Arg);
    if (It == State.boundArguments.end())
      continue;
    unsigned Descriptors = 0;
    for (const auto &View : boundPointers_)
      Descriptors += View.argIndex == State.view.argIndex;
    unsigned Contracts = 0;
    for (const MDOperand &Op : Root->getMetadata(MD_EJIT_METADATA)->operands())
      if (auto *Sub = dyn_cast<MDNode>(Op.get()))
        if (Sub->getNumOperands() >= 3) {
          auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
          auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
          if (Tag && Tag->getString() == TAG_EJIT_BOUND_PTR && Idx &&
              Idx->getValue().getActiveBits() <= 32)
            Contracts += Idx->getZExtValue() == State.view.argIndex;
        }
    if (Descriptors != 1 || Contracts != 1)
      return Reject("ambiguous-bound-contract");
    MDNode *BoundMD = getBoundArgumentMetadata(*Root, State.view.argIndex);
    if (!BoundMD)
      return Reject("missing-bound-contract");
    auto *Period = dyn_cast<MDString>(BoundMD->getOperand(1));
    auto *Size = mdconst::dyn_extract<ConstantInt>(BoundMD->getOperand(3));
    auto Instance = Period ? preservedInstance(Period->getString()) : std::nullopt;
    if (!Instance || State.view.periodInstance != *Instance)
      return Reject("missing-bound-lifecycle-dependency");
    if (!Size || Size->getValue().getActiveBits() > 32 ||
        !Size->getZExtValue() || Size->getZExtValue() > State.view.size ||
        It->second > UINT64_MAX - Offset ||
        !isBoundMayConstLoad(LI, State.boundArguments, State.mayConstFields,
                             DL, preservedLoadArgs_))
      return nullptr;
    return readPreservedConstant(State.view.rawPtr, Size->getZExtValue(),
                                  It->second + Offset, Ty, DL);
  }
  return nullptr;
}

PreservedAnalyses
EJitStructFieldPass::run(Function &F, FunctionAnalysisManager &AM) {
  Module *M = F.getParent();
  if (!M) {
    EJIT_DIAG_VERBOSE("struct-field run SKIP func=%s: no parent module",
                      F.getName().str().c_str());
    return PreservedAnalyses::all();
  }

  assert(mapsBuilt_ && "EJitStructFieldPass::initFromModule() must be "
                       "called before run()");

  const DataLayout &DL = M->getDataLayout();

  // 1. Use cached module-level metadata maps (built once per module by
  //    initFromModule(), reused across all function runs).

  // 2. Scan all loads and collect replacements.
  struct Replacement { LoadInst *LI; Constant *ConstVal; };
  SmallVector<Replacement, 16> replacements;
#ifdef EJIT_DIAG_ENABLE
  size_t totalLoads = 0, mayConstLoads = 0;
  // Only the ejit_entry function (or any function that actually has may_const
  // activity / replacements) is worth a per-function diagnostic block. Silent
  // for the common case of an auxiliary callee with no may_const loads, which
  // is the dominant source of struct-field log noise on a specialization
  // module that contains many non-entry callees.
  bool isEjitEntry =
      hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY);
#endif

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *LI = dyn_cast<LoadInst>(&I);
      if (!LI)
        continue;
#ifdef EJIT_DIAG_ENABLE
      ++totalLoads;
#endif

      if (preserveDimensions_) {
#ifdef EJIT_DIAG_ENABLE
        bool IsMayConst = isMayConstLoad(LI, mayConstFieldMap_, DL);
        for (const BoundPointerState &State : boundStates_)
          IsMayConst |= isBoundMayConstLoad(
              LI, State.boundArguments, State.mayConstFields, DL,
              preservedLoadArgs_);
        mayConstLoads += IsMayConst;
#endif
        if (Constant *C = tryReplacePreservedLoad(LI, DL))
          replacements.push_back({LI, C});
        // Failure must not enter a legacy pattern that freezes a pointer base
        // or reads an unbounded pointee. Only a proven load result may change.
        continue;
      }

      // A pointer-form period global is itself the root needed to reach nested
      // may_const fields. It has no may_const marker of its own, but replacing
      // it here exposes a concrete address to IPSCCP and later StructFieldPass
      // rounds, including when the field access lives in a non-inlined helper.
      if (Constant *C = tryReplacePeriodPointerBase(
              LI, gvPeriodMap_, registry_, DL)) {
        replacements.push_back({LI, C});
        continue;
      }

      bool BoundMayConst = false;
      for (const BoundPointerState &State : boundStates_)
        BoundMayConst |= isBoundMayConstLoad(
            LI, State.boundArguments, State.mayConstFields, DL, freeDimArgs_);
      if (!BoundMayConst && !isMayConstLoad(LI, mayConstFieldMap_, DL))
        continue;
#ifdef EJIT_DIAG_ENABLE
      ++mayConstLoads;
#endif

      Value *PtrOp = LI->getPointerOperand();

      // Try each access pattern in order.
      Constant *C = tryReplacePeriodAbsoluteAddress(
          LI, F, gvPeriodMap_, mayConstFieldMap_, registry_, DL,
          freeDimArgs_);

      // Pattern 0: an ejit_bound_ptr parameter. Only the marked load is read
      // from the shared object; the pointer argument and all dynamic fields
      // remain live inputs to the specialization.
      if (!C) {
        for (const BoundPointerState &State : boundStates_) {
          C = tryReplaceBoundPointer(LI, State.view.rawPtr, State.view.size,
                                     State.boundArguments, DL, freeDimArgs_);
          if (C)
            break;
        }
      }

      // Pattern 1: direct GlobalVariable load (scalar static variable).
      if (!C) {
        if (auto *GV = dyn_cast<GlobalVariable>(PtrOp->stripPointerCasts()))
          C = tryReplaceDirectGV(LI, GV, gvPeriodMap_, registry_, DL);
      }

      // Pattern 2: GEP-based access (array or struct field).
      if (!C)
        C = tryReplaceDirectGEP(LI, PtrOp, gvPeriodMap_, registry_, DL,
                                freeDimArgs_);

      // Pattern 3: indirect pointer access (pointer-type period variable).
      if (!C)
        C = tryReplaceIndirect(LI, PtrOp, gvPeriodMap_, registry_, DL);

      if (C)
        replacements.push_back({LI, C});
#ifdef EJIT_DIAG_ENABLE
      else
        logReplaceFailure(LI, gvPeriodMap_, registry_, DL, freeDimArgs_);
#endif
    }
  }

  // 3. Apply replacements.
  bool changed = false;
  for (auto &R : replacements) {
    R.LI->replaceAllUsesWith(R.ConstVal);
    R.LI->eraseFromParent();
    changed = true;
  }

#ifdef EJIT_DIAG_ENABLE
  // Gate the per-function block: emit for the ejit_entry function (always
  // interesting — it is the specialization target) or for any function where a
  // may_const load was seen or a replacement actually happened. Auxiliary
  // callees with no may_const activity stay silent, eliminating the bulk of
  // the struct-field log volume while preserving locatability for the
  // functions that matter. Raise the log level to VERBOSE to see this detail.
  if (isEjitEntry || mayConstLoads > 0 || !replacements.empty()) {
    EJIT_DIAG_VERBOSE("struct-field run func=%s entry=%d replaced=%zu",
                      F.getName().str().c_str(), isEjitEntry ? 1 : 0,
                      replacements.size());
    EJIT_DIAG_VERBOSE("  loads total=%zu may_const=%zu replaced=%zu",
                      totalLoads, mayConstLoads, replacements.size());
  }
#endif
  LLVM_DEBUG(if (changed) dbgs() << "ejit-struct-field: replaced "
                                 << replacements.size() << " load(s) in "
                                 << F.getName() << "\n");
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
