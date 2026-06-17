//===-- llvm/CodeGen/XBBRMetadata.h - XBBR metadata emission ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This declares the XBBRMetadataEmitter pass interface and the per-BB
// attribute word format consumed by AsmPrinter to emit `.llvm_xbbr_attr`
// (PLAN §9.3, SPEC §5.3 blacklist conditions). Enabled by
// `-fbb-cross-reorder=partial|full` (clang) or `-enable-xbbr` (llc testing).
//
// Width: 16-bit per BB. The original M1 design used 8 bits (just enough
// for the §5.3 list); a code review surfaced "noreturn-tail" as a
// missing 9th bit (SPEC §5.3 item 7) and the cleanest way forward is
// to widen the on-disk encoding to a u16 little-endian word. This
// leaves headroom for future bits without another format break.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_XBBRMETADATA_H
#define LLVM_CODEGEN_XBBRMETADATA_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CommandLine.h"

#include <cstdint>

namespace llvm {

class MachineFunction;
class MachineFunctionPass;

/// Testing flag that enables XBBR metadata emission from llc (clang instead
/// sets TargetOptions::XBBR via -fbb-cross-reorder=). Referenced by both the
/// pass scheduler (TargetPassConfig), AsmPrinter, MachineFunction, and
/// BasicBlockSections so they stay in sync.
extern cl::opt<bool> EnableXBBR;

/// Per-basic-block XBBR attribute bits (PLAN §9.3 / SPEC §5.3).
/// 16-bit bitmask, stored one little-endian word per MBB in the order
/// MachineFunction iterates. Bits are independent (e.g., the entry
/// block is also blacklisted from migration), so the value is a
/// bitmask, not a tag.
namespace xbbr {
enum AttrBit : uint16_t {
  IsEntry           = 1u << 0, ///< Function entry block (anchored).
  IsLandingPad      = 1u << 1, ///< EH landing pad (mirrors BBEntry::IsEHPad).
  IsIndirectBrTarget= 1u << 2, ///< blockaddress / callbr / indirectbr target.
                                ///< Also covers inline-asm-goto indirect
                                ///< targets via MBB.isInlineAsmBrIndirectTarget()
                                ///< — see HasInlineAsmLabel comment.
  HasSetjmp         = 1u << 3, ///< Calls setjmp/longjmp — recognized by
                                ///< the returns_twice attribute (setjmp side)
                                ///< or by callee name match (longjmp side,
                                ///< since glibc longjmp has no IR-level
                                ///< marker — only `noreturn`, which would
                                ///< over-match abort/exit).
  HasInlineAsmLabel = 1u << 4, ///< Inline asm whose body emits .section
                                ///< / .pushsection / .popsection. Distinct
                                ///< from "inline asm goto with indirect
                                ///< targets", which is already covered by
                                ///< IsIndirectBrTarget.
  IsMustTail        = 1u << 5, ///< Block ends with a musttail call (IR
                                ///< BasicBlock::getTerminatingMustTailCall()
                                ///< / CallBase::isMustTailCall — NOT
                                ///< MachineInstr::isReturn(), see PLAN §3.4
                                ///< review correction).
  UserBlacklisted   = 1u << 6, ///< Listed in -fbb-cross-reorder-blacklist=.
  IsCold            = 1u << 7, ///< Synced with MachineFunctionSplitter
                                ///< (deferred; lld consumer in M3 — flag
                                ///< is currently always 0).
  IsNoReturnTail    = 1u << 8, ///< BB ends with a `noreturn` callsite and
                                ///< has no successors (SPEC §5.3 item 7).
                                ///< Anchoring this aids backtrace fidelity
                                ///< in non-Unwind exception flows.
};
} // namespace xbbr

/// Returns the per-MBB XBBR attribute words (u16, host-endian; the
/// linker reads them as little-endian on disk) computed by
/// XBBRMetadataEmitter for a given MachineFunction, in MachineFunction
/// iteration order. Returns an empty ArrayRef if -enable-xbbr was not
/// in effect for this MF or the pass did not run yet.
ArrayRef<uint16_t> getXBBRAttrs(const MachineFunction &MF);

/// createXBBRMetadataEmitterPass - Compute XBBR per-BB metadata after
/// MachineBlockPlacement; the actual section bytes are written by
/// AsmPrinter::emitXBBRAttrSection (mirrors BB_ADDR_MAP).
MachineFunctionPass *createXBBRMetadataEmitterPass();

} // namespace llvm

#endif // LLVM_CODEGEN_XBBRMETADATA_H
