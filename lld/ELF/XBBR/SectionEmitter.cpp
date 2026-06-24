//===- SectionEmitter.cpp - XBBR Stage 5: section emission --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stage 5 of the XBBR linker pipeline (PLAN §4.3): builds per-BB
// decision-map entries and BBFragment objects from the pipeline result.
//
// Current scope: the decision map is fully populated with BB-level
// entries. Physical BB-level section emission (actually moving BBs into
// .text.hot / .text.unlikely) is out of scope here and runs in a
// follow-up patch; until then the .text layout still uses the existing
// function-level hfsort+ order.
//
//===----------------------------------------------------------------------===//

#include "SectionEmitter.h"
#include "Config.h"
#include "InputSection.h"
#include "LinkerScript.h"
#include "OutputSections.h"
#include "Relocations.h"
#include "SyntheticSections.h"
#include "Symbols.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/XBBRDecisionMap.h"


using namespace llvm;

namespace lld::elf::xbbr {

namespace {

/// Compute the projected final VA for each BB in the layout order.
std::vector<BBFragment> buildFragments(const XBBRGraph &graph,
                                       const XBBRLayoutResult &result,
                                       unsigned maxAlign) {
  std::vector<BBFragment> fragments;
  ArrayRef<XBBRNode> allNodes = graph.nodes();
  uint64_t currentVA = 0; // relative offset, to be patched after assignOffsets

  for (const auto &order : result.ClusterBBOrders) {
    for (uint32_t nodeIdx : order) {
      const XBBRNode &node = allNodes[nodeIdx];
      // Per-BB alignment will eventually come from BBAddrMap
      // BBEntry::Alignment; for now we always use 1 (no padding).

      BBFragment frag;
      frag.NodeIdx = nodeIdx;
      frag.Size = node.Size;
      frag.Alignment = 1;
      frag.FinalVA = currentVA;

      // Map node back to its InputSection + offset.
      InputSectionBase *sec = graph.funcSection(node.Func);
      if (sec) {
        // BB offset within the function = sum of preceding BB sizes.
        uint32_t offset = 0;
        auto fn = graph.funcs()[node.Func];
        for (uint32_t I = fn.FirstNode; I < nodeIdx; ++I)
          offset += allNodes[I].Size;
        frag.Offset = offset;
        frag.Section = sec;
      }

      fragments.push_back(frag);
      currentVA += node.Size;
    }
  }
  return fragments;
}

} // namespace

/// P0-2: sum the byte sizes of every ThunkSection lld emitted. ThunkSections
/// are synthetic sections named ".text.thunk" (ThunkSection ctor,
/// SyntheticSections.cpp) inserted into InputSectionDescriptions by the
/// finalizeAddressDependentContent thunk loop. By the time the tail-end recheck
/// runs that loop has converged, so these sizes are the real, final thunk
/// overhead — the ground truth Stage 4's projected estimate is compared
/// against (PLAN §4.3 Stage 5 "末梢复核").
static uint64_t computeRealThunkBytes(Ctx &ctx) {
  uint64_t total = 0;
  for (OutputSection *os : ctx.outputSections)
    for (SectionCommand *cmd : os->commands) {
      auto *isd = dyn_cast<InputSectionDescription>(cmd);
      if (!isd)
        continue;
      for (InputSection *s : isd->sections)
        // name check distinguishes ThunkSections from other synthetic sections
        // (.got/.plt/…); dyn_cast<ThunkSection> is non-null for any
        // SyntheticSection (Relocations.cpp:1978 FIXME), so the name is the
        // reliable discriminator.
        if (isa<SyntheticSection>(s) && s->name == ".text.thunk")
          total += s->getSize();
    }
  return total;
}

/// #2 tail-end guard: with real (post-layout) addresses, verify every
/// CondInvolved BB's unthunkable conditional/test branch is within ISA range.
/// These relocs (AArch64 CONDBR19/TSTBR14; ARM Thumb-1 THM_JUMP8/THM_JUMP11)
/// CANNOT be thunked by lld, so an overflow is a hard link error at write time
/// — except Stage 4's projected model can miss the case where a pinned BB's
/// real .text.hot mid-point placement (a rename-race artifact, see
/// backfillDecisionMapVAs) separates a cond pair. Catch it here as a fatal
/// error instead of letting lld's relocate() report it later with no XBBR
/// context. Returns the per-arch cond range, or 0 if the arch/reloc isn't
/// range-checked here.
static uint64_t condRangeForReloc(uint16_t emachine, uint32_t type) {
  switch (emachine) {
  case ELF::EM_AARCH64:
    if (type == ELF::R_AARCH64_TSTBR14)
      return 0x8000; // ±32 KiB
    if (type == ELF::R_AARCH64_CONDBR19)
      return 0x100000; // ±1 MiB
    return 0;
  case ELF::EM_ARM:
    // Thumb-1 unthunkable conditionals (markRangeAnchors pins these).
    if (type == ELF::R_ARM_THM_JUMP8)
      return 0x80; // ±256 B
    if (type == ELF::R_ARM_THM_JUMP11)
      return 0x800; // ±2 KiB
    return 0;
  default:
    return 0;
  }
}

static void verifyCondRangesFinal(Ctx &ctx, XBBRGraph &graph) {
  for (const XBBRNode &node : graph.nodes()) {
    if (!node.CondInvolved)
      continue;
    InputSectionBase *src = node.BBSection;
    if (!src)
      continue;
    for (const Relocation &r : src->relocs()) {
      uint64_t range = condRangeForReloc(ctx.arg.emachine, r.type);
      if (range == 0)
        continue;
      // Real branch-site and target VAs (final addresses).
      uint64_t branchSite = src->getVA(r.offset);
      auto *d = dyn_cast<Defined>(r.sym);
      if (!d)
        continue; // undefined/external — lld reports its own error
      uint64_t target = d->getVA(ctx, r.addend);
      uint64_t dist = branchSite >= target ? branchSite - target
                                          : target - branchSite;
      if (dist > range) {
        ErrAlways(ctx)
            << "XBBR: unthunkable conditional branch out of range after "
               "layout (func=" << node.Func << " bb=" << node.BB
            << " reloc=0x" << llvm::format_hex_no_prefix(r.type, 0)
            << " dist=" << dist << " range=" << range
            << "). Stage 4 failed to pin this cond pair; rerun with "
               "--bb-cross-reorder-fallback=conservative or disable "
               "--bb-cross-reorder. Aborting to avoid a runtime SIGILL.";
        return;
      }
    }
  }
}

void runSectionEmitter(Ctx &ctx, XBBRGraph &graph,
                       XBBRLayoutResult &result) {
  if (!ctx.arg.xbbrEmitDecisionMap)
    return;

  // Build BB-level fragments.
  auto fragments = buildFragments(graph, result, ctx.arg.xbbrMaxAlign);

  // Populate the decision-map section with BB-level entries.
  Partition *mainPart = ctx.mainPart;
  if (!mainPart || !mainPart->xbbrDecisionMap)
    return;

  XBBRDecisionMapSection &dm = *mainPart->xbbrDecisionMap;
  dm.setDegraded(result.Degraded);

  // Build per-BB decision entries (PLAN §9.4, 32-byte stride).
  std::vector<XBBRDecisionEntry> entries;
  entries.reserve(fragments.size());
  uint32_t a = 0, m = 0;
  // In partial mode cold BBs are kept with their original function (per
  // SPEC §4 / BBLayout::collectMigratableBBs); they should be reported
  // as `anchored` in the decision map even though their XBBR attr bits
  // don't make them anchors in the SPEC §5.3 sense. Otherwise downstream
  // tools (BOLT, llvm-bbreorder-dump) cannot distinguish cold BBs that
  // *will* migrate (full) from cold BBs that *won't* (partial) — which
  // is the core partial-vs-full differentiation those tools surface.
  const bool partialMode = ctx.arg.xbbrMode == XBBRMode::Partial;
  for (const auto &frag : fragments) {
    const XBBRNode &node = graph.nodes()[frag.NodeIdx];
    const FuncInfo &fn = graph.funcs()[node.Func];
    XBBRDecisionEntry e;
    e.BBIndex = node.BB;
    e.NewAddress = frag.FinalVA;
    e.ClusterId = 0; // simplified for now; multi-cluster routing later
    // Mirrors BBLayout::collectMigratableBBs's anchor decision. P1-3: a hot
    // cond-branch BB that migrated (CondSafeToMigrate, still in the layout, not
    // Stage-4-pinned) is Moved; cond BBs whose component isn't all-hot (and the
    // §5.3/EH-gate/partial-cold anchors) are Anchored. Stage-4-pinned BBs are
    // absent from the layout (no fragment) and thus unlisted (anchored at
    // original).
    const bool effectiveAnchor =
        node.isAnchor() || fn.IsEHGated ||
        (node.CondInvolved && !node.CondSafeToMigrate) ||
        (partialMode && node.isCold());
    if (effectiveAnchor) {
      e.DecisionFlags = llvm::XBBRDecisionMap::EntryFlags::Anchored;
      ++a;
    } else {
      e.DecisionFlags = llvm::XBBRDecisionMap::EntryFlags::Moved;
      ++m;
    }
    // orig_func_addr: placeholder 0. A follow-up patch patches this to
    // the function entry block's real linked VA after assignOffsets has
    // run. (PLAN §9.4: it is the linker-time absolute address, not a
    // node index.)
    e.OrigFuncAddr = 0;
    e.FuncId = node.Func; // internal FuncId for reverse lookup
    entries.push_back(e);
  }
  dm.setEntries(std::move(entries));

  if (ctx.arg.xbbrStats)
    errs() << "xbbr-emit: decisionEntries=" << fragments.size()
           << " moved=" << m << " anchored=" << a << "\n";

  // Build placements for the future physical-emission pass.
  result.Placements.clear();
  for (const auto &frag : fragments) {
    BBPlacement p;
    p.NodeIdx = frag.NodeIdx;
    const XBBRNode &node = graph.nodes()[frag.NodeIdx];
    p.TargetSec = node.isAnchor() ? BBPlacement::Section::Original
                  : node.isCold()  ? BBPlacement::Section::Unlikely
                                   : BBPlacement::Section::Hot;
    result.Placements.push_back(p);
  }
}

void renameSectionsForHotColdSplit(Ctx &ctx, XBBRGraph &graph) {
  if (ctx.arg.xbbrMode < XBBRMode::Partial)
    return;
  const bool partialMode = ctx.arg.xbbrMode == XBBRMode::Partial;
  for (const XBBRNode &node : graph.nodes()) {
    InputSectionBase *sec = node.BBSection;
    if (!sec)
      continue;
    // Classification mirrors runSectionEmitter's BBPlacement logic, but is
    // mode-aware for cold: SPEC §4 — in partial, cold BBs stay at their
    // original function position (Original); only full migrates them to
    // .text.unlikely. Entry/anchor BBs are always Original.
    //
    // NOTE: this runs in the Driver, before scanRelocations (Writer) parses
    // section relocs, so node.CondInvolved/CondSafeToMigrate are NOT computed
    // yet here (markRangeAnchors finds no relocs). We therefore cannot make
    // this routing cond-pair-aware the way collectMigratableBBs does. Two
    // reloc-independent guards keep unthunkable cond pairs co-located (same
    // output section) so they never overflow:
    //  - HasCondBranch: detected from raw reloc TYPES at graph-build time
    //    (collectFromFile). A function with any unthunkable cond branch keeps
    //    ALL its BBs in their compiler-assigned section (Original) — never
    //    routed to .text.hot/.text.unlikely — so the cond pair stays together.
    //    This is over-conservative (an all-hot cond component that P1-3 would
    //    let migrate to .text.hot stays in .text instead), traded for
    //    correctness: the projection can't see output-section routing, so a
    //    split pair would slip past Stage 4 and fatal at verifyCondRangesFinal.
    //  - the section-name guard below: never re-route a section the compiler
    //    already hot/cold-split (.text.hot.* / .text.unlikely.*), which would
    //    double-prefix it (.text.hot.unlikely.*) and re-split the function.
    BBPlacement::Section target;
    if (graph.funcs()[node.Func].HasCondBranch)
      target = BBPlacement::Section::Original;
    else if (node.isAnchor())
      target = BBPlacement::Section::Original;
    else if (node.isCold() && !partialMode)
      target = BBPlacement::Section::Unlikely;
    else if (node.isCold())
      target = BBPlacement::Section::Original; // partial: cold stays
    else
      target = BBPlacement::Section::Hot;
    if (target != BBPlacement::Section::Hot &&
        target != BBPlacement::Section::Unlikely)
      continue;
    // sec->name is ".text.<rest>" under -fbasic-block-sections=all; rewrite to
    // ".text.hot.<rest>" / ".text.unlikely.<rest>" so getOutputSectionName
    // (with -z keep-text-section-prefix) routes it to .text.hot / .text.unlikely.
    StringRef nm = sec->name;
    if (!nm.starts_with(".text."))
      continue; // not a per-BB text section; leave untouched
    // Respect the compiler's own hot/cold split: a section already named
    // .text.hot.<rest> / .text.unlikely.<rest> (MachineFunctionSplitter, under
    // -fbasic-block-sections=all) is part of a function the compiler already
    // placed. Re-prefixing would double-name it (.text.hot.unlikely.<rest>) and
    // re-route a hot BB out of a compiler-split function, splitting its
    // unthunkable cond pairs across output sections. Leave it where the
    // compiler put it.
    if (nm.starts_with(".text.hot.") || nm.starts_with(".text.unlikely."))
      continue;
    StringRef rest = nm.substr(strlen(".text."));
    StringRef prefix = target == BBPlacement::Section::Hot ? ".text.hot."
                                                           : ".text.unlikely.";
    sec->name = ctx.saver.save((Twine(prefix) + rest).str());
  }
}

/// P0 task #2: returns true if any relocation in `sec` targets a linker-emitted
/// range-extension thunk. lld rewrites an over-range branch's `reloc.sym` to
/// the thunk's own Defined symbol (Relocations.cpp: `rel.sym =
/// t->getThunkTargetSym()`), and that Defined's section is a ThunkSection — a
/// SyntheticSection whose name is ".text.thunk" (ThunkSection ctor). Only
/// migration can make a branch over-range, so callers consult this only for
/// Moved BBs; a set Thunk flag means "this BB's migration required a linker
/// thunk". `dyn_cast<ThunkSection>` is NOT a reliable discriminator (it is
/// non-null for any SyntheticSection, Relocations.cpp:1978 FIXME), so the name
/// check is the reliable test — same approach as `computeRealThunkBytes`.
static bool bbRelocTargetsThunk(InputSectionBase *sec) {
  if (!sec)
    return false;
  for (const Relocation &r : sec->relocs()) {
    auto *d = dyn_cast<Defined>(r.sym);
    if (!d || !d->section)
      continue;
    if (isa<SyntheticSection>(d->section) && d->section->name == ".text.thunk")
      return true;
  }
  return false;
}

void backfillDecisionMapVAs(Ctx &ctx) {
  if (!ctx.xbbrGraph || !ctx.xbbrLayoutResult)
    return;

  // P0-2 tail-end recheck (PLAN §4.3 Stage 5 "末梢复核"): now that
  // finalizeAddressDependentContent has converged and optimizeBasicBlockJumps
  // has run, measure the REAL thunk overhead from the emitted ThunkSections
  // and compare against Stage 4's projected estimate. This runs regardless of
  // whether the decision map is emitted — the budget check is independent.
  XBBRLayoutResult &result = *ctx.xbbrLayoutResult;
  result.ThunkBytes = computeRealThunkBytes(ctx);
  // ActualCost captures the verified real size-overhead cost component
  // (w_size × thunk bytes); a full icache/itlb/btb recompute against final
  // addresses is future work.
  result.ActualCost =
      static_cast<double>(result.ThunkBytes) * ctx.arg.xbbrWeightSize;
  const bool overrun =
      ctx.arg.xbbrMaxThunkBytes != 0 &&
      result.ThunkBytes > ctx.arg.xbbrMaxThunkBytes;
  if (overrun) {
    // Addresses are final by now, so we cannot revert BBs here — the
    // pre-emit pinning (P0-1) is the revert mechanism. This warning surfaces
    // cases Stage 4's projected estimate under-counted (e.g. distance created
    // by non-BB filler it couldn't see) so the user can tighten the budget or
    // use --bb-cross-reorder-fallback=conservative to catch it pre-emit.
    Warn(ctx) << "XBBR tail-end recheck: real thunk bytes ("
              << result.ThunkBytes
              << ") exceed --bb-cross-reorder-max-thunk-bytes ("
              << ctx.arg.xbbrMaxThunkBytes << "); Stage 4 projected "
              << result.EstimatedThunkBytes
              << ". Layout already emitted — rerun with a larger budget or "
                 "--bb-cross-reorder-fallback=conservative.";
  }
  if (ctx.arg.xbbrStats)
    errs() << "xbbr-stage5: realThunkBytes=" << result.ThunkBytes
           << " estThunkBytes=" << result.EstimatedThunkBytes
           << " overrun=" << (overrun ? 1 : 0) << "\n";

  // #2 tail-end cond-overflow guard: Stage 4's pin reverts a BB's section name
  // to .text, but that rename races orphan routing (Driver, pre-Writer) — a
  // pinned CondInvolved BB can stay in .text.hot at the sortISDBySectionOrder
  // mid-point, and collectCondPins skips cond edges whose endpoint is off the
  // projected map, so an unthunkable B.cond/TBZ overflow could slip through.
  // Addresses are final now, so verify each CondInvolved BB's unthunkable cond
  // reloc is in range against REAL VAs. A failure here is a Stage 4/Stage 5
  // ordering bug surfacing as a fatal link error (caught), not a silent
  // runtime SIGILL / write-time hard error.
  verifyCondRangesFinal(ctx, *ctx.xbbrGraph);

  Partition *mainPart = ctx.mainPart;
  if (!mainPart || !mainPart->xbbrDecisionMap)
    return;
  XBBRGraph &graph = *ctx.xbbrGraph;
  XBBRDecisionMapSection &dm = *mainPart->xbbrDecisionMap;
  // Real VAs are stable now (finalizeAddressDependentContent converged).
  // Patch each entry: NewAddress = the BB's per-section final VA; OrigFuncAddr
  // = the function entry section's VA (ABI §5.1: function symbol = entry BB).
  for (XBBRDecisionEntry &e : dm.entries()) {
    std::optional<uint32_t> nodeIdx = graph.findNode(e.FuncId, e.BBIndex);
    if (!nodeIdx)
      continue;
    const XBBRNode &node = graph.nodes()[*nodeIdx];
    if (node.BBSection)
      e.NewAddress = node.BBSection->getVA();
    if (InputSectionBase *entrySec = graph.funcSection(node.Func))
      e.OrigFuncAddr = entrySec->getVA();
    // P0 task #2: Thunk flag. lld rewrites an over-range branch's reloc.sym
    // to the thunk's Defined (section ".text.thunk"); a Moved BB whose branch
    // needed a range-extension thunk carries the Thunk flag so consumers
    // (BOLT, llvm-bbreorder-dump) can see that migration induced thunking.
    // Anchored BBs do not migrate, so their branches cannot be made
    // over-range by XBBR and are not flagged. The Thunk flag is added to
    // (not replacing) Moved, so the entry reads as moved+thunk.
    if ((e.DecisionFlags & llvm::XBBRDecisionMap::EntryFlags::Moved) &&
        bbRelocTargetsThunk(node.BBSection))
      e.DecisionFlags |= llvm::XBBRDecisionMap::EntryFlags::Thunk;
  }
}

} // namespace lld::elf::xbbr
