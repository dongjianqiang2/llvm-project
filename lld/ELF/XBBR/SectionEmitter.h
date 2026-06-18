//===- SectionEmitter.h - XBBR Stage 5: emission header ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_SECTIONEMITTER_H
#define LLD_ELF_XBBR_SECTIONEMITTER_H

namespace lld::elf {
struct Ctx;
namespace xbbr {
class XBBRGraph;
struct XBBRLayoutResult;
} // namespace xbbr
} // namespace lld::elf

namespace lld::elf::xbbr {

/// Stage 5 entry point. Populates the decision-map section with BB-level
/// entries from the layout result, and creates BBFragment objects for
/// physical section emission.
///
/// Current scope: the decision map is fully populated with per-BB
/// entries (func_addr, bb_index, new_address, cluster_id, flags).
/// Physical BB-level emission is out of scope here — the .text layout
/// still uses the existing hfsort+ order while the emitter records the
/// intended BB layout in the decision map for downstream tools.
void runSectionEmitter(Ctx &ctx, XBBRGraph &graph,
                       XBBRLayoutResult &result);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_SECTIONEMITTER_H
