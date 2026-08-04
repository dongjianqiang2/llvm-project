//===-- EJitLinkOptimizationPlugin.cpp - EJIT JITLink optimizations -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitLinkOptimizationPlugin.h"

#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/TargetParser/Triple.h"

#include <limits>

using namespace llvm;
using namespace llvm::ejit;
using namespace llvm::jitlink;

namespace {

static bool isStubSymbol(const Symbol &S) {
  return S.isDefined() && S.getOffset() == 0 &&
         S.getSize() == sizeof(aarch64::PointerJumpStubContent) &&
         S.getBlock().getSize() == sizeof(aarch64::PointerJumpStubContent) &&
         S.getBlock().getSection().getName() ==
             aarch64::PLTTableManager::getSectionName();
}

// Match the standard JITLink AArch64 pointer-jump chain:
//   branch -> stub --Page21/PageOffset12--> GOT --Pointer64--> destination.
static Symbol *getPointerJumpStubDestination(Symbol &Stub) {
  if (!isStubSymbol(Stub))
    return nullptr;

  Symbol *GOTEntry = nullptr;
  bool SawPage21 = false;
  bool SawPageOffset12 = false;
  for (Edge &E : Stub.getBlock().edges()) {
    if (E.getKind() == aarch64::Page21) {
      if (SawPage21 || E.getOffset() != 0 || E.getAddend() != 0)
        return nullptr;
      SawPage21 = true;
    } else if (E.getKind() == aarch64::PageOffset12) {
      if (SawPageOffset12 || E.getOffset() != 4 || E.getAddend() != 0)
        return nullptr;
      SawPageOffset12 = true;
    } else {
      continue;
    }
    if (GOTEntry && GOTEntry != &E.getTarget())
      return nullptr;
    GOTEntry = &E.getTarget();
  }
  if (!SawPage21 || !SawPageOffset12 || !GOTEntry || !GOTEntry->isDefined() ||
      GOTEntry->getOffset() != 0 || GOTEntry->getSize() != 8 ||
      GOTEntry->getBlock().getSize() != 8 ||
      GOTEntry->getBlock().getSection().getName() !=
          aarch64::GOTTableManager::getSectionName())
    return nullptr;

  Symbol *Destination = nullptr;
  for (Edge &E : GOTEntry->getBlock().edges()) {
    if (E.getKind() != aarch64::Pointer64)
      continue;
    if (Destination || E.getOffset() != 0 || E.getAddend() != 0)
      return nullptr;
    Destination = &E.getTarget();
  }
  return Destination;
}

static bool isDirectBranchReachable(uint64_t FixupAddr, uint64_t TargetAddr,
                                    int64_t Addend) {
  constexpr uint64_t MaxInt64 =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (FixupAddr > MaxInt64 || TargetAddr > MaxInt64)
    return false;

  int64_t Delta = 0;
  if (SubOverflow(static_cast<int64_t>(TargetAddr),
                  static_cast<int64_t>(FixupAddr), Delta) ||
      AddOverflow(Delta, Addend, Delta))
    return false;

  // Branch26PCRel stores a signed 26-bit count of four-byte instructions:
  // [-2^27, 2^27 - 4] bytes, with four-byte alignment.
  return (Delta & 3) == 0 && isInt<28>(Delta);
}

} // namespace

Error llvm::ejit::relaxAArch64BranchStubs(LinkGraph &G) {
  const Triple &TT = G.getTargetTriple();
  if (TT.getArch() != Triple::aarch64 && TT.getArch() != Triple::aarch64_be)
    return Error::success();

  // Diagnostic counters (INFO): categorize why each stubbed Branch26PCRel was
  // or was not relaxed, so the "stubbed (0 exceed +-128MB)" audit line can be
  // reconciled with what this pass actually did.
  unsigned total = 0, stubbed = 0, chainMismatch = 0, unresolved = 0,
           outOfRange = 0, relaxed = 0;

  for (Section &Sec : G.sections()) {
    for (Block *B : Sec.blocks()) {
      for (Edge &E : B->edges()) {
        if (E.getKind() != aarch64::Branch26PCRel)
          continue;
        ++total;

        Symbol &Tgt = E.getTarget();
        // Loose stub check (matches EJitLinkDiagPlugin's isStubSymbol): a
        // defined symbol whose block is in $__STUBS. This is the population the
        // diag plugin counts as "stubbed"; the sub-counters below then pin why
        // each was or was not relaxed.
        if (!Tgt.isDefined() ||
            Tgt.getBlock().getSection().getName() != "$__STUBS")
          continue; // direct target, not a stub
        ++stubbed;

        Symbol *Destination = getPointerJumpStubDestination(Tgt);
        if (!Destination) {
          // Stub is non-standard: strict isStubSymbol (size/offset) or the
          // Page21/PageOffset12/Pointer64 chain (addends, GOT size) did not
          // match. The diag plugin's loose followStubToRealTarget may still
          // resolve these, so they appear as "stubbed (within +-128MB)" in the
          // audit without being relaxable.
          ++chainMismatch;
          continue;
        }
        uint64_t FixupAddr = B->getFixupAddress(E).getValue();
        uint64_t TargetAddr = Destination->getAddress().getValue();
        if (TargetAddr == 0) {
          // Genuinely unresolved (e.g. a weakly-referenced external whose
          // lookup found no definition). NOTE: JITLink's applyLookupResult
          // resolves non-weak externals before PreFixup but leaves
          // isExternal() true (it sets the address without converting to
          // isAbsolute), so we must gate on the address, NOT isExternal() -
          // otherwise every resolved external stub is wrongly skipped.
          ++unresolved;
          continue;
        }
        if (!isDirectBranchReachable(FixupAddr, TargetAddr, E.getAddend())) {
          ++outOfRange;
          continue;
        }
        E.setTarget(*Destination);
        ++relaxed;
      }
    }
  }
  EJIT_DIAG("relaxAArch64BranchStubs: graph=%s Branch26PCRel: %u total, "
            "%u stubbed (chain-mismatch=%u unresolved=%u out-of-range=%u), "
            "%u relaxed",
            G.getName().c_str(), total, stubbed, chainMismatch, unresolved,
            outOfRange, relaxed);
  return Error::success();
}

void EJitLinkOptimizationPlugin::modifyPassConfig(
    orc::MaterializationResponsibility &MR, LinkGraph &G,
    PassConfiguration &Config) {
  (void)MR;
  const Triple &TT = G.getTargetTriple();
  if (TT.getArch() != Triple::aarch64 && TT.getArch() != Triple::aarch64_be)
    return;

  // PreFixup runs after allocation and external-symbol lookup, so both the
  // call site and final destination addresses are known. Retargeting here
  // leaves the already-allocated stub/GOT layout intact while allowing the
  // normal Branch26PCRel fixup to emit a direct B/BL displacement.
  Config.PreFixupPasses.push_back(relaxAArch64BranchStubs);
}
