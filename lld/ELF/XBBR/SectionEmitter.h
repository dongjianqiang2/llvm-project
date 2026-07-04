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

/// Phase 3: patch the decision-map entries' OrigFuncAddr/NewAddress with the
/// real linked VAs. Called after finalizeAddressDependentContent (and after
/// optimizeBasicBlockJumps) so outSecOff/addr are final. The entry COUNT is
/// unchanged (set during runSectionEmitter), so no section re-finalization is
/// needed — writeTo reads the patched entries at write time.
void backfillDecisionMapVAs(Ctx &ctx);

/// P1-1: rename each per-BB InputSection by its hot/cold classification so
/// that lld's orphan grouping (with -z keep-text-section-prefix, which the
/// Driver implies for partial/full) routes hot migratable BBs to .text.hot
/// and cold BBs (full mode) to .text.unlikely. Entry/anchor BBs and, in
/// partial mode, cold BBs (SPEC §4: cold stays at its original function
/// position) keep their .text.<fn> name. `graph` must be freshly built (Stage
/// 0); this is called from the Driver before orphan routing on a temporary
/// pre-ICF graph — the real layout graph is rebuilt post-ICF in buildSectionOrder.
void renameSectionsForHotColdSplit(Ctx &ctx, XBBRGraph &graph);

/// P1-3 safety net: after markRangeAnchors has computed CondInvolved /
/// CondSafeToMigrate, revert any non-CondSafeToMigrate BB sections from
/// .text.hot.* / .text.unlikely.* back to .text.* so unthunkable cond-branch
/// partners are never split across output sections. This catches cases the
/// Driver-side HasCondBranch check (renameSectionsForHotColdSplit) misses —
/// HasCondBranch scans raw ELF reloc data, but some cond-branch patterns (e.g.
/// branches in non-entry BBs of merged sections) may not be detected until the
/// full relocation scan.
///
/// Must run after graph->build() (which calls markRangeAnchors) and before
/// runXBBRPipeline (which consumes CondSafeToMigrate for migration decisions).
void ensureCondBranchesCoLocated(Ctx &ctx, XBBRGraph &graph);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_SECTIONEMITTER_H
