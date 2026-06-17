//===-- XBBRMetadataEmitter.cpp - XBBR metadata emitter pass --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// XBBRMetadataEmitter runs after MachineBlockPlacement and computes, for
// each MachineBasicBlock, the XBBR blacklist/attribute bits (SPEC §5.3,
// PLAN §3.4 / §9.3). The bytes are stashed in a per-MachineFunction
// side-table and the actual `.llvm_xbbr_attr` section is emitted by
// AsmPrinter::emitXBBRAttrSection (mirroring BB_ADDR_MAP).
//
// Detection is faithful to the PLAN §3.4 review correction:
//   * `is_musttail` uses IR-level `BasicBlock::getTerminatingMustTailCall()`
//     and `CallBase::isMustTailCall()`, NOT `MachineInstr::isReturn()`.
//   * `is_landing_pad` / `is_indirectbr_target` are sourced from the same
//     APIs that drive `BBEntry::Metadata::IsEHPad` / `HasIndirectBranch`,
//     so a Stage-0 consistency check between the two sections is trivial.
//
// The pass does not modify the MIR.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/XBBRMetadata.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ManagedStatic.h"

#include <mutex>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "xbbr-metadata-emitter"

/// Testing flag enabling XBBR metadata emission from llc. clang instead sets
/// TargetOptions::XBBR via -fbb-cross-reorder= (wired up in M1-T06).
cl::opt<bool> llvm::EnableXBBR(
    "enable-xbbr", cl::Hidden, cl::init(false),
    cl::desc("Enable XBBR metadata emission (.llvm_xbbr_attr) for testing"));

namespace {

// Per-MachineFunction attribute bytes, populated by the pass and consumed
// by AsmPrinter via getXBBRAttrs(). Plain DenseMap rather than a hot-header
// MachineFunction field — XBBR is opt-in, so the indirection is fine and
// keeps MachineFunction.h ABI quiet.
//
// Single-threaded LLVM CodeGen makes locking unnecessary in normal usage,
// but llc-tests and parallel ThinLTO backends could in principle clash.
// Use a small mutex for safety; contention is irrelevant for opt-in XBBR.
struct XBBRAttrTable {
  DenseMap<const MachineFunction *, std::vector<uint8_t>> Map;
  std::mutex M;
};

XBBRAttrTable &getAttrTable() {
  static XBBRAttrTable T;
  return T;
}

/// Walk MF.getFunction() once and pre-compute per-IR-BasicBlock blacklist
/// bits derived from the IR layer (musttail, setjmp, inline-asm-with-label,
/// blockaddress). Returning a DenseMap<BasicBlock*, uint8_t> lets the MIR
/// loop look up its parent IR block in O(1).
DenseMap<const BasicBlock *, uint8_t>
computeIRAttrs(const MachineFunction &MF) {
  DenseMap<const BasicBlock *, uint8_t> Attrs;
  const Function &F = MF.getFunction();
  for (const BasicBlock &BB : F) {
    uint8_t Bits = 0;
    if (BB.hasAddressTaken())
      Bits |= xbbr::IsIndirectBrTarget;
    if (BB.getTerminatingMustTailCall())
      Bits |= xbbr::IsMustTail;
    for (const Instruction &I : BB) {
      if (const auto *II = dyn_cast<IntrinsicInst>(&I)) {
        Intrinsic::ID ID = II->getIntrinsicID();
        if (ID == Intrinsic::eh_sjlj_setjmp ||
            ID == Intrinsic::eh_sjlj_longjmp) {
          Bits |= xbbr::HasSetjmp;
          continue;
        }
      }
      if (const auto *CB = dyn_cast<CallBase>(&I)) {
        // setjmp / vfork / similar — `returns_twice` lives on the call's
        // attributes (CallInst::canReturnTwice() is just a wrapper).
        if (CB->hasFnAttr(Attribute::ReturnsTwice))
          Bits |= xbbr::HasSetjmp;
        if (CB->isMustTailCall())
          Bits |= xbbr::IsMustTail;
        // Inline asm with section/label-like directives is conservatively
        // blacklisted to keep PLAN §5.3 invariants. Cheap heuristic on the
        // asm text — XBBR can only get less aggressive from here.
        if (const auto *IA = dyn_cast<InlineAsm>(CB->getCalledOperand())) {
          StringRef S = IA->getAsmString();
          if (S.contains(".section") || S.contains(".pushsection") ||
              S.contains(".popsection"))
            Bits |= xbbr::HasInlineAsmLabel;
        }
      }
    }
    if (Bits)
      Attrs[&BB] = Bits;
  }
  return Attrs;
}

class XBBRMetadataEmitter : public MachineFunctionPass {
public:
  static char ID;

  XBBRMetadataEmitter() : MachineFunctionPass(ID) {
    initializeXBBRMetadataEmitterPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "XBBR Metadata Emitter"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (!EnableXBBR)
      return false;
    if (skipFunction(MF.getFunction()))
      return false;

    LLVM_DEBUG(dbgs() << "XBBRMetadataEmitter running on " << MF.getName()
                      << " (" << MF.size() << " MBBs)\n");

    DenseMap<const BasicBlock *, uint8_t> IRAttrs = computeIRAttrs(MF);

    std::vector<uint8_t> Attrs;
    Attrs.reserve(MF.size());
    for (const MachineBasicBlock &MBB : MF) {
      uint8_t Bits = 0;
      if (MBB.isEntryBlock())
        Bits |= xbbr::IsEntry;
      if (MBB.isEHPad())
        Bits |= xbbr::IsLandingPad;
      if (MBB.hasAddressTaken() || MBB.isInlineAsmBrIndirectTarget())
        Bits |= xbbr::IsIndirectBrTarget;
      if (const BasicBlock *BB = MBB.getBasicBlock()) {
        auto It = IRAttrs.find(BB);
        if (It != IRAttrs.end())
          Bits |= It->second;
      }
      // bit6=user_blacklisted and bit7=cold are populated by later M1 tasks
      // (M1-T06 user blacklist / MFS sync). Keep the bytes self-consistent
      // by leaving them zero for now.
      Attrs.push_back(Bits);
    }

    auto &T = getAttrTable();
    std::lock_guard<std::mutex> Lock(T.M);
    T.Map[&MF] = std::move(Attrs);
    return false;
  }
};

} // end anonymous namespace

char XBBRMetadataEmitter::ID = 0;
INITIALIZE_PASS(XBBRMetadataEmitter, "xbbr-metadata-emitter",
                "XBBR Metadata Emitter", false, false)

MachineFunctionPass *llvm::createXBBRMetadataEmitterPass() {
  return new XBBRMetadataEmitter();
}

ArrayRef<uint8_t> llvm::getXBBRAttrs(const MachineFunction &MF) {
  auto &T = getAttrTable();
  std::lock_guard<std::mutex> Lock(T.M);
  auto It = T.Map.find(&MF);
  if (It == T.Map.end())
    return {};
  return It->second;
}
