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
// PLAN §3.4 / §9.3). The 16-bit attr words are stashed in a
// per-MachineFunction side-table and the actual `.llvm_xbbr_attr`
// section is emitted by AsmPrinter::emitXBBRAttrSection (mirroring
// BB_ADDR_MAP).
//
// Detection notes (in order of bit number, see XBBRMetadata.h):
//
//   * `is_landing_pad` / `is_indirectbr_target` are sourced from the same
//     APIs that drive `BBEntry::Metadata::IsEHPad` / `HasIndirectBranch`,
//     so a Stage-0 consistency check between the two sections is trivial.
//
//   * `has_setjmp` recognizes:
//       (a) calls to functions with `returns_twice` attribute — covers
//           glibc setjmp / sigsetjmp / __sigsetjmp;
//       (b) calls whose callee name matches the longjmp family —
//           glibc longjmp does NOT have an IR-level marker (it has only
//           `noreturn`, which would over-match abort/exit/__cxa_throw);
//           name match is the only sound way to recognize it.
//
//   * `is_musttail` uses IR-level `BasicBlock::getTerminatingMustTailCall()`
//     and `CallBase::isMustTailCall()`, NOT `MachineInstr::isReturn()`
//     (PLAN §3.4 review correction — review #7).
//
//   * `is_no_return_tail` flags the SPEC §5.3 item 7 case — a BB that
//     ends with a `noreturn` callsite and has no successors. Anchoring
//     such a BB to its parent function aids backtrace fidelity. The
//     check is *both* "callsite is noreturn" *and* "MBB has no
//     successors", so we never over-match every abort()-call site.
//
// The pass does not modify the MIR.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/XBBRMetadata.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <mutex>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "xbbr-metadata-emitter"

/// Testing flag enabling XBBR metadata emission from llc. clang instead sets
/// TargetOptions::XBBR via -fbb-cross-reorder= (wired up in M1-T06).
cl::opt<bool> llvm::EnableXBBR(
    "enable-xbbr", cl::Hidden, cl::init(false),
    cl::desc("Enable XBBR metadata emission (.llvm_xbbr_attr) for testing"));

/// Path to a newline-separated function blacklist (M1-T06,
/// SPEC §6.1 -fbb-cross-reorder-blacklist=). One symbol name per line;
/// `#` starts a comment; blank lines are ignored. Functions in the
/// blacklist get UserBlacklisted on every BB except the entry.
static cl::opt<std::string> XBBRBlacklistFile(
    "xbbr-blacklist", cl::Hidden, cl::init(""),
    cl::desc("Path to XBBR per-function blacklist (one name per line)"));

/// If set, XBBRMetadataEmitter prints a one-line summary per function
/// to stderr after computing attrs (SPEC §6.1 -fbb-cross-reorder-stats).
static cl::opt<bool> XBBRStats(
    "xbbr-stats", cl::Hidden, cl::init(false),
    cl::desc("Print XBBR per-function attr statistics to stderr"));

namespace {

// Per-MachineFunction attribute words (16-bit) populated by the pass and
// consumed by AsmPrinter via getXBBRAttrs(). Plain DenseMap rather than a
// hot-header MachineFunction field — XBBR is opt-in, so the indirection
// is fine and keeps MachineFunction.h ABI quiet.
//
// Single-threaded LLVM CodeGen makes locking unnecessary in normal usage,
// but llc-tests and parallel ThinLTO backends could in principle clash.
// Use a small mutex for safety; contention is irrelevant for opt-in XBBR.
struct XBBRAttrTable {
  DenseMap<const MachineFunction *, std::vector<uint16_t>> Map;
  std::mutex M;
};

XBBRAttrTable &getAttrTable() {
  static XBBRAttrTable T;
  return T;
}

/// Lazy-loaded set of blacklisted function names from -xbbr-blacklist=.
struct XBBRBlacklist {
  DenseSet<StringRef> Names;            ///< Refers into Storage's strings
  std::vector<std::string> Storage;     ///< Owns the name buffers
  bool Loaded = false;
  std::mutex M;
};

XBBRBlacklist &getBlacklist() {
  static XBBRBlacklist BL;
  return BL;
}

void ensureBlacklistLoaded() {
  auto &BL = getBlacklist();
  std::lock_guard<std::mutex> Lock(BL.M);
  if (BL.Loaded)
    return;
  BL.Loaded = true;
  if (XBBRBlacklistFile.empty())
    return;
  auto Buf = MemoryBuffer::getFile(XBBRBlacklistFile);
  if (!Buf) {
    errs() << "warning: -xbbr-blacklist=" << XBBRBlacklistFile
           << ": " << Buf.getError().message() << "\n";
    return;
  }
  StringRef Body = (*Buf)->getBuffer();
  while (!Body.empty()) {
    auto [Line, Rest] = Body.split('\n');
    Body = Rest;
    Line = Line.trim();
    if (Line.empty() || Line.starts_with("#"))
      continue;
    BL.Storage.emplace_back(Line.str());
    BL.Names.insert(StringRef(BL.Storage.back()));
  }
}

bool functionIsUserBlacklisted(const Function &F) {
  ensureBlacklistLoaded();
  auto &BL = getBlacklist();
  std::lock_guard<std::mutex> Lock(BL.M);
  return BL.Names.contains(F.getName());
}

/// Return true if `Name` looks like a member of the longjmp family —
/// glibc longjmp / _longjmp / siglongjmp, plus the rarely-seen __longjmp.
/// This is a name-based check because LLVM has no IR-level marker for
/// these (`noreturn` is too broad — it also covers abort/exit/__cxa_throw).
bool isLongjmpName(StringRef Name) {
  return Name == "longjmp" || Name == "_longjmp" || Name == "siglongjmp" ||
         Name == "__longjmp" || Name == "_siglongjmp";
}

/// Walk MF.getFunction() once and pre-compute per-IR-BasicBlock blacklist
/// bits derived from the IR layer (musttail, setjmp/longjmp,
/// inline-asm-with-section, blockaddress). Returning a
/// DenseMap<BasicBlock*, uint16_t> lets the MIR loop look up its parent
/// IR block in O(1).
DenseMap<const BasicBlock *, uint16_t>
computeIRAttrs(const MachineFunction &MF) {
  DenseMap<const BasicBlock *, uint16_t> Attrs;
  const Function &F = MF.getFunction();
  for (const BasicBlock &BB : F) {
    uint16_t Bits = 0;
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
        // (a) returns_twice covers setjmp / sigsetjmp / vfork.
        if (CB->hasFnAttr(Attribute::ReturnsTwice))
          Bits |= xbbr::HasSetjmp;
        // (b) longjmp family must be matched by callee name — see file
        //     header comment for why `noreturn` is not the right signal.
        if (const Function *Callee = CB->getCalledFunction())
          if (isLongjmpName(Callee->getName()))
            Bits |= xbbr::HasSetjmp;
        if (CB->isMustTailCall())
          Bits |= xbbr::IsMustTail;
        // Inline asm whose body emits .section/.pushsection/.popsection
        // changes the active section under XBBR's nose; conservatively
        // blacklist. Inline-asm-goto with indirect targets is handled
        // separately via MBB.isInlineAsmBrIndirectTarget() below.
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

    DenseMap<const BasicBlock *, uint16_t> IRAttrs = computeIRAttrs(MF);
    const bool UserBL = functionIsUserBlacklisted(MF.getFunction());

    std::vector<uint16_t> Attrs;
    Attrs.reserve(MF.size());
    for (const MachineBasicBlock &MBB : MF) {
      uint16_t Bits = 0;
      if (MBB.isEntryBlock())
        Bits |= xbbr::IsEntry;
      if (MBB.isEHPad())
        Bits |= xbbr::IsLandingPad;
      // The OR-with-IR below is a deliberate belt-and-braces — MBB
      // inherits hasAddressTaken from its parent IR BB, so for any
      // MBB whose BB is address-taken the two checks agree. Keeping
      // both lets us catch (a) MIR-only address takes (e.g. jump
      // tables synthesized late) and (b) MBBs with no parent BB
      // (computeIRAttrs skips those entirely).
      if (MBB.hasAddressTaken() || MBB.isInlineAsmBrIndirectTarget())
        Bits |= xbbr::IsIndirectBrTarget;
      if (const BasicBlock *BB = MBB.getBasicBlock()) {
        auto It = IRAttrs.find(BB);
        if (It != IRAttrs.end())
          Bits |= It->second;
      }
      // SPEC §5.3 item 7: a BB whose terminator is a noreturn callsite
      // and which has no successors. Detect at IR level via the parent
      // BB's terminator; the MIR-level "no successors" predicate is
      // what makes this narrow enough not to over-match every abort()
      // call site (only the actual tail blocks qualify).
      //
      // Exclude landing pads: LLVM's EH preparation rewrites `resume`
      // into `call _Unwind_Resume(); unreachable`, which superficially
      // matches our pattern. Those blocks are already flagged
      // IsLandingPad for the same anchoring reason; double-flagging
      // them as IsNoReturnTail just confuses Stage 0 diagnostics.
      if (MBB.succ_empty() && !MBB.isEHPad()) {
        if (const BasicBlock *BB = MBB.getBasicBlock()) {
          if (const Instruction *Term = BB->getTerminator()) {
            if (isa<UnreachableInst>(Term) && Term != &BB->front()) {
              if (const auto *CB =
                      dyn_cast<CallBase>(Term->getPrevNode())) {
                if (CB->hasFnAttr(Attribute::NoReturn) ||
                    (CB->getCalledFunction() &&
                     CB->getCalledFunction()->hasFnAttribute(
                         Attribute::NoReturn)))
                  Bits |= xbbr::IsNoReturnTail;
              }
            }
          }
        }
      }
      if (UserBL && !MBB.isEntryBlock()) {
        // Entry BB anchors regardless (function symbol = entry address).
        // Marking only non-entry BBs makes the blacklist semantically
        // "this function's BBs may not migrate cross-function".
        Bits |= xbbr::UserBlacklisted;
      }
      // bit7=cold sync with MachineFunctionSplitter is deferred to M3
      // (lld is the only consumer of cold-threshold; SPEC §6.1's
      // -fbb-cross-reorder-cold-threshold= is wired then).
      Attrs.push_back(Bits);
    }

    if (XBBRStats) {
      uint32_t NumAnchors = 0, NumMustTail = 0, NumLandingPad = 0,
               NumNoRetTail = 0, NumIndirectBr = 0, NumSetjmp = 0,
               NumUserBL = 0;
      for (uint16_t W : Attrs) {
        if (W & (xbbr::IsEntry | xbbr::IsLandingPad |
                 xbbr::IsIndirectBrTarget | xbbr::HasSetjmp |
                 xbbr::HasInlineAsmLabel | xbbr::IsMustTail |
                 xbbr::UserBlacklisted | xbbr::IsNoReturnTail))
          ++NumAnchors;
        if (W & xbbr::IsMustTail) ++NumMustTail;
        if (W & xbbr::IsLandingPad) ++NumLandingPad;
        if (W & xbbr::IsNoReturnTail) ++NumNoRetTail;
        if (W & xbbr::IsIndirectBrTarget) ++NumIndirectBr;
        if (W & xbbr::HasSetjmp) ++NumSetjmp;
        if (W & xbbr::UserBlacklisted) ++NumUserBL;
      }
      errs() << "xbbr-stats: " << MF.getName() << " bbs=" << Attrs.size()
             << " anchors=" << NumAnchors
             << " musttail=" << NumMustTail
             << " landingpad=" << NumLandingPad
             << " noreturntail=" << NumNoRetTail
             << " indirectbr=" << NumIndirectBr
             << " setjmp=" << NumSetjmp
             << " userbl=" << NumUserBL << "\n";
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

ArrayRef<uint16_t> llvm::getXBBRAttrs(const MachineFunction &MF) {
  auto &T = getAttrTable();
  std::lock_guard<std::mutex> Lock(T.M);
  auto It = T.Map.find(&MF);
  if (It == T.Map.end())
    return {};
  return It->second;
}
