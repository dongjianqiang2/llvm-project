//===- DWARFRewriter.cpp - XBBR DWARF/CFI rewrite ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stub — full DWARF/CFI rewriting will be implemented in a follow-up
// patch once physical BB-level section emission is in place.  The
// skeleton establishes the interface and integration point so that the
// linker pipeline never silently drops this step.
//
// PLAN §5.1 describes the required transformations:
//   - DW_TAG_subprogram low_pc/high_pc → DW_AT_ranges
//   - .debug_line set_address per BB segment
//   - .debug_aranges/.debug_ranges multi-segment expansion
//
//===----------------------------------------------------------------------===//

#include "DWARFRewriter.h"
#include "Config.h"
#include "lld/Common/ErrorHandler.h"

using namespace llvm;

namespace lld::elf::xbbr {

void rewriteDWARF(Ctx &ctx) {
  // Stub. The real implementation will:
  //   * convert DW_TAG_subprogram low_pc/high_pc to DW_AT_ranges (PLAN §5.1)
  //   * emit DW_LNE_set_address per BB segment in .debug_line
  //   * expand .debug_aranges/.debug_ranges to multi-segment lists
  //   * split FDEs and rebuild .eh_frame_hdr (PLAN §5.3)
  //
  // Calling here while no BBs are physically migrated is a no-op; the
  // diagnostic under --bb-cross-reorder-stats confirms the pipeline is
  // wired correctly so the real pass can drop in without further plumbing.
  if (ctx.arg.xbbrStats)
    Warn(ctx) << "XBBR: DWARF rewrite stub invoked (not yet implemented)";
}

} // namespace lld::elf::xbbr
