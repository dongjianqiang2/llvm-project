//===-- EJitDedupIndex.cpp - Specialization dedup fingerprint index -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitDedupIndex.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/StructuralHash.h"

using namespace llvm;

namespace llvm {
namespace ejit {

namespace {

/// FNV-1a 64. Deliberately independent of StructuralHash's mixing so that a
/// defect in one hash does not correlate with the other.
struct FNV64 {
  uint64_t H = 1469598103934665603ULL; // FNV offset basis
  void byte(uint8_t B) { H = (H ^ B) * 1099511628211ULL; }
  void u64(uint64_t V) {
    for (unsigned I = 0; I < 8; ++I)
      byte(static_cast<uint8_t>((V >> (8 * I)) & 0xFF));
  }
  void i64(int64_t V) { u64(static_cast<uint64_t>(V)); }
  void str(StringRef S) {
    u64(S.size()); // length first so concatenations cannot alias
    for (char C : S)
      byte(static_cast<uint8_t>(C));
  }
  uint64_t finish() const { return H; }
};

/// Hash one Constant, recursively. GlobalValue references hash by NAME
/// (stable across two parses of the same bitcode, unlike pointer identity).
void hashConstant(FNV64 &H, const Constant *C) {
  if (!C) {
    H.u64(0);
    return;
  }
  H.u64(C->getValueID());
  H.u64(static_cast<uint64_t>(C->getType()->getTypeID()));

  if (const auto *GV = dyn_cast<GlobalValue>(C)) {
    // Not the pointer: two parses of the same bitcode create distinct
    // GlobalValue objects. Name + linkage capture identity for merge purposes.
    H.str(GV->getName());
    H.u64(static_cast<uint64_t>(GV->getLinkage()));
    return;
  }

  if (const auto *CI = dyn_cast<ConstantInt>(C)) {
    const APInt &V = CI->getValue();
    H.u64(V.getNumWords());
    for (unsigned I = 0; I < V.getNumWords(); ++I)
      H.u64(V.getRawData()[I]);
    return;
  }
  if (const auto *CF = dyn_cast<ConstantFP>(C)) {
    const APInt &Bits = CF->getValueAPF().bitcastToAPInt();
    H.u64(Bits.getNumWords());
    for (unsigned I = 0; I < Bits.getNumWords(); ++I)
      H.u64(Bits.getRawData()[I]);
    return;
  }
  if (const auto *Seq = dyn_cast<ConstantDataSequential>(C)) {
    H.u64(Seq->getNumElements());
    for (unsigned I = 0; I < Seq->getNumElements(); ++I)
      hashConstant(H, Seq->getElementAsConstant(I));
    return;
  }

  // ConstantAggregate / ConstantExpr / BlockAddress / DSOLocalEquivalent:
  // recurse over operands (covers GlobalOpt-promoted initializer trees).
  H.u64(C->getNumOperands());
  for (const Use &Op : C->operands())
    hashConstant(H, dyn_cast<Constant>(Op));
}

} // namespace

DedupFingerprint computeModuleFingerprint(const Module &M) {
  DedupFingerprint Out;
  Out.fp1 = StructuralHash(M, /*DetailedHash=*/true);

  FNV64 H;
  uint32_t NumFuncs = 0;
  uint32_t NumInsts = 0;

  // Defined globals: identity + initializer. This closes the StructuralHash
  // blind spot where a GlobalOpt promotion bakes a specialized constant into
  // a promoted initializer while leaving every instruction identical.
  for (const GlobalVariable &GV : M.globals()) {
    if (GV.isDeclaration())
      continue;
    H.str(GV.getName());
    H.u64(static_cast<uint64_t>(GV.getLinkage()));
    H.u64(GV.isConstant() ? 1 : 0);
    hashConstant(H, GV.getInitializer());
  }

  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    ++NumFuncs;
    for (const BasicBlock &BB : F)
      NumInsts += static_cast<uint32_t>(BB.size());
  }

  // Fold the counts into fp2 as well so irUnits cannot diverge from the
  // hashed content unnoticed.
  H.u64(NumFuncs);
  H.u64(NumInsts);
  Out.fp2 = H.finish();
  Out.irUnits = (static_cast<uint64_t>(NumFuncs) << 32) | NumInsts;
  return Out;
}

void *EJitDedupIndex::find(uint32_t funcIndex, const DedupFingerprint &FP) {
  size_t Start = (FP.fp1 ^ (static_cast<uint64_t>(funcIndex) * 0x9e3779b97f4a7c15ULL)) %
                 kCapacity;
  for (size_t Probe = 0; Probe < kCapacity; ++Probe) {
    const Slot &S = Slots_[(Start + Probe) % kCapacity];
    if (!S.used)
      return nullptr; // linear probe: an empty slot ends the run
    if (S.funcIndex == funcIndex && S.FP == FP)
      return S.fnPtr;
  }
  return nullptr;
}

bool EJitDedupIndex::insert(uint32_t funcIndex, const DedupFingerprint &FP,
                            void *fnPtr) {
  size_t Start = (FP.fp1 ^ (static_cast<uint64_t>(funcIndex) * 0x9e3779b97f4a7c15ULL)) %
                 kCapacity;
  for (size_t Probe = 0; Probe < kCapacity; ++Probe) {
    Slot &S = Slots_[(Start + Probe) % kCapacity];
    if (!S.used) {
      S.used = true;
      S.funcIndex = funcIndex;
      S.FP = FP;
      S.fnPtr = fnPtr;
      ++Stats_.entries;
      ++Stats_.inserts;
      return true;
    }
    if (S.funcIndex == funcIndex && S.FP == FP) {
      S.fnPtr = fnPtr; // same fingerprint recompiled: refresh the pointer
      ++Stats_.inserts;
      return true;
    }
  }
  Stats_.full = true;
  return false;
}

void EJitDedupIndex::clear() {
  for (Slot &S : Slots_) {
    S.used = false;
    S.fnPtr = nullptr;
    S.FP = DedupFingerprint();
  }
  Stats_ = Stats{};
}

} // namespace ejit
} // namespace llvm
