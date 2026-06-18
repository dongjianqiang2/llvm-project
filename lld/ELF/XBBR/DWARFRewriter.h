//===- DWARFRewriter.h - XBBR DWARF/CFI rewrite header ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// When XBBR migrates BBs across function boundaries, the owning function's
// code becomes non-contiguous. This pass rewrites DWARF debug info so that
// gdb/perf/addr2line continue to work correctly (PLAN §5.1):
//
//   - DW_TAG_subprogram: low_pc/high_pc → DW_AT_ranges (multi-segment)
//   - .debug_line: DW_LNE_set_address at each BB segment start
//   - .debug_aranges/.debug_ranges: multi-segment lists
//
// M4 scope: header + skeleton. Full implementation in M5 after physical
// BB-level section emission is complete.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_DWARFREWRITER_H
#define LLD_ELF_XBBR_DWARFREWRITER_H

namespace lld::elf {
struct Ctx;
} // namespace lld::elf

namespace lld::elf::xbbr {

/// Rewrite DWARF debug info for functions whose BBs were migrated by XBBR.
/// Called after section addresses are assigned (post-assignOffsets in M5).
void rewriteDWARF(Ctx &ctx);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_DWARFREWRITER_H
