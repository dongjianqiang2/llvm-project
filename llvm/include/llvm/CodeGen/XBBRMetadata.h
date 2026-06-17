//===-- llvm/CodeGen/XBBRMetadata.h - XBBR metadata emission ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This declares the XBBRMetadataEmitter pass interface and the per-BB
// attribute byte format consumed by AsmPrinter to emit `.llvm_xbbr_attr`
// (PLAN §9.3, SPEC §5.3 blacklist conditions). Enabled by
// `-fbb-cross-reorder=partial|full` (clang) or `-enable-xbbr` (llc testing).
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
/// Stored one byte per MBB in the order MachineFunction iterates. Bits are
/// independent (e.g., the entry block is also blacklisted from migration),
/// so the value is a bitmask, not a tag.
namespace xbbr {
enum AttrBit : uint8_t {
  IsEntry           = 1u << 0, ///< Function entry block (anchored).
  IsLandingPad      = 1u << 1, ///< EH landing pad (mirrors BBEntry::IsEHPad).
  IsIndirectBrTarget= 1u << 2, ///< blockaddress / callbr / indirectbr target.
  HasSetjmp         = 1u << 3, ///< Calls setjmp / canReturnTwice.
  HasInlineAsmLabel = 1u << 4, ///< Inline asm with section/label (M1: 0).
  IsMustTail        = 1u << 5, ///< Block ends with a musttail call.
  UserBlacklisted   = 1u << 6, ///< Listed in -fbb-cross-reorder-blacklist (M1-T06).
  IsCold            = 1u << 7, ///< Synced with MachineFunctionSplitter (M1-T06).
};
} // namespace xbbr

/// Returns the per-MBB XBBR attribute bytes computed by XBBRMetadataEmitter
/// for a given MachineFunction, in MachineFunction iteration order. Returns
/// an empty ArrayRef if -enable-xbbr was not in effect for this MF or the
/// pass did not run yet.
ArrayRef<uint8_t> getXBBRAttrs(const MachineFunction &MF);

/// createXBBRMetadataEmitterPass - Compute XBBR per-BB metadata after
/// MachineBlockPlacement; the actual section bytes are written by
/// AsmPrinter::emitXBBRAttrSection (mirrors BB_ADDR_MAP).
MachineFunctionPass *createXBBRMetadataEmitterPass();

} // namespace llvm

#endif // LLVM_CODEGEN_XBBRMETADATA_H
