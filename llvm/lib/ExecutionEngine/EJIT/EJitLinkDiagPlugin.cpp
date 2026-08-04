//===-- EJitLinkDiagPlugin.cpp - JITLink branch-reloc diagnostic plugin ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitLinkDiagPlugin.h"

#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"

#include <string>

using namespace llvm;
using namespace llvm::ejit;
using namespace llvm::jitlink;

namespace {
#ifdef EJIT_DIAG_ENABLE

// AArch64 unconditional/conditional branch-immediate range: the signed 26-bit
// (B/BL) / 19-bit (B.cond) / 14-bit (TBZ/TBNZ) displacement is in 4-byte
// words, so the byte range of a direct B/BL is +-128MB. A target whose
// PC-relative distance exceeds this cannot be reached by a direct BL and must
// be bridged through a stub.
static constexpr int64_t AArch64DirectBranchLimit = (1ll << 27); // 128 MiB

static std::string symName(const Symbol &S) {
  if (S.hasName())
    return (*S.getName()).str();
  return "<anon>";
}

// AArch64 instructions are always little-endian encoded, even in a big-endian
// data (aarch64_be) build, so read the 4 fixup bytes as little-endian. Returns
// 0 if the offset is out of bounds (treated as an unknown opcode by callers).
static uint32_t readInstrLE(const Block &B, Edge::OffsetT Off) {
  if (B.isZeroFill())
    return 0;
  ArrayRef<char> C = B.getContent();
  if (Off + 4 > C.size())
    return 0;
  return *reinterpret_cast<const support::ulittle32_t *>(C.data() + Off);
}

// Human-readable label for a branch-class relocation, derived from the edge
// kind plus the fixed-up opcode so B vs BL (and TBZ vs TBNZ) are distinguished.
static const char *branchLabel(Edge::Kind K, const Block &B,
                               Edge::OffsetT Off) {
  uint32_t Instr = readInstrLE(B, Off);
  switch (K) {
  case aarch64::Branch26PCRel:
    // B (0x14000000) / BL (0x94000000): bits [30:26] == 00101; bit 31 = L.
    if ((Instr & 0x7C000000u) == 0x14000000u)
      return (Instr & 0x80000000u) ? "bl" : "b";
    return "bl/b";
  case aarch64::TestAndBranch14PCRel:
    // TBZ (0x36000000) / TBNZ (0x37000000): bits [30:25] == 011011;
    // bit 24 = op (0 = TBZ, 1 = TBNZ).
    if ((Instr & 0x7E000000u) == 0x36000000u)
      return (Instr & 0x01000000u) ? "tbnz" : "tbz";
    return "tbz/tbnz";
  case aarch64::CondBranch19PCRel:
    // B.cond (0x54000000); the condition lives in bits [3:0].
    return "b.cond";
  default:
    return "branch";
  }
}

static bool isBranchKind(Edge::Kind K) {
  return K == aarch64::Branch26PCRel ||
         K == aarch64::TestAndBranch14PCRel ||
         K == aarch64::CondBranch19PCRel;
}

static bool isStubSymbol(const Symbol &S) {
  return S.isDefined() && S.getBlock().getSection().getName() == "$__STUBS";
}

// Follow a $__STUBS PointerJumpStub to the real target it ultimately reaches:
//   stub block --Page21/PageOffset12--> $__GOT entry --Pointer64--> real target
// Returns nullptr if the chain does not match the expected stub layout.
static const Symbol *followStubToRealTarget(const Symbol &StubSym) {
  const Block &StubB = StubSym.getBlock();
  const Symbol *GotSym = nullptr;
  for (const Edge &E : StubB.edges())
    if (E.getKind() == aarch64::Page21 ||
        E.getKind() == aarch64::PageOffset12) {
      GotSym = &E.getTarget();
      break;
    }
  if (!GotSym || !GotSym->isDefined())
    return nullptr;
  const Block &GotB = GotSym->getBlock();
  for (const Edge &E : GotB.edges())
    if (E.getKind() == aarch64::Pointer64)
      return &E.getTarget();
  return nullptr;
}

static int64_t absVal(int64_t V) { return V < 0 ? -V : V; }

// PostFixup pass body. INFO prints only the final summary; VERBOSE also prints
// the header and every relocation. Never fails the link: all output is
// advisory.
static Error reportLinkGraph(LinkGraph &G) {
  unsigned stubbed = 0, direct = 0, outOfRange = 0;
  bool headerPrinted = false;

  for (Section &Sec : G.sections()) {
    for (Block *B : Sec.blocks()) {
      for (Edge &E : B->edges()) {
        if (!isBranchKind(E.getKind()))
          continue;

        if (!headerPrinted) {
          headerPrinted = true;
          EJIT_DIAG_VERBOSE(
              "linkdiag: graph=%s triple=%s -- branch relocation audit",
              G.getName().c_str(), G.getTargetTriple().getTriple().c_str());
        }

        const char *Label = branchLabel(E.getKind(), *B, E.getOffset());
        uint64_t FixupAddr = B->getFixupAddress(E).getValue();
        Symbol &Target = E.getTarget();

        if (isStubSymbol(Target)) {
          ++stubbed;
          uint64_t StubAddr = Target.getAddress().getValue();
          const Symbol *Real = followStubToRealTarget(Target);
          uint64_t RealAddr = Real ? Real->getAddress().getValue() : 0;
          int64_t DirectDist =
              Real ? (int64_t)RealAddr - (int64_t)FixupAddr : 0;
          bool exceeds = Real && absVal(DirectDist) > AArch64DirectBranchLimit;
          if (exceeds)
            ++outOfRange;
          EJIT_DIAG(
              "  [STUBBED] %s @0x%llx -> stub@0x%llx "
              "(ADRP x16; LDR x16,[x16]; BR x16) -> $__GOT -> %s @0x%llx"
              " | direct dist=%lld (%s +-128MB)",
              Label, (unsigned long long)FixupAddr,
              (unsigned long long)StubAddr,
              Real ? symName(*Real).c_str() : "<?>", (unsigned long long)RealAddr,
              (long long)DirectDist, exceeds ? "EXCEEDS" : "within");
        } else {
          ++direct;
          uint64_t TargetAddr = Target.getAddress().getValue();
          int64_t Disp = (int64_t)TargetAddr - (int64_t)FixupAddr;
          EJIT_DIAG_VERBOSE("  [direct ] %s @0x%llx -> %s @0x%llx | dist=%lld",
                            Label, (unsigned long long)FixupAddr,
                            symName(Target).c_str(),
                            (unsigned long long)TargetAddr, (long long)Disp);
        }
      }
    }
  }

  if (headerPrinted)
    EJIT_DIAG(
        "linkdiag: graph=%s summary: %u stubbed (%u exceed +-128MB), %u direct",
        G.getName().c_str(), stubbed, outOfRange, direct);

  return Error::success();
}

#endif // EJIT_DIAG_ENABLE
} // namespace

void EJitLinkDiagPlugin::modifyPassConfig(orc::MaterializationResponsibility &MR,
                                          LinkGraph &G,
                                          PassConfiguration &Config) {
#ifdef EJIT_DIAG_ENABLE
  // Avoid constructing the pass when diagnostics are disabled. INFO emits one
  // summary per graph; relocation-level detail remains VERBOSE-only.
  if (gEJitDiagLevel < EJIT_LOG_LVL_INFO)
    return;

  // Branch-relocation classification is AArch64-specific.
  const Triple &TT = G.getTargetTriple();
  if (TT.getArch() != Triple::aarch64 && TT.getArch() != Triple::aarch64_be)
    return;

  Config.PostFixupPasses.push_back(
      [](LinkGraph &G) -> Error { return reportLinkGraph(G); });
#else
  (void)MR;
  (void)G;
  (void)Config;
#endif
}
