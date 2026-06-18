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
#include "SyntheticSections.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"
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
    XBBRDecisionEntry e;
    e.BBIndex = node.BB;
    e.NewAddress = frag.FinalVA;
    e.ClusterId = 0; // simplified for now; multi-cluster routing later
    const bool effectiveAnchor =
        node.isAnchor() || (partialMode && node.isCold());
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

} // namespace lld::elf::xbbr
