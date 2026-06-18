//===- DWARFRewriter.cpp - XBBR DWARF/CFI rewrite ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// M4 skeleton — full DWARF/CFI rewriting is implemented in M5 after
// physical BB-level section emission is complete.  The skeleton
// establishes the interface and integration point.
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
  // M4 skeleton — M5 implements the full pass after BBs physically move.
  if (ctx.arg.xbbrStats)
    errs() << "xbbr-m4: DWARF rewrite skeleton (full impl in M5)\n";
}

} // namespace lld::elf::xbbr
