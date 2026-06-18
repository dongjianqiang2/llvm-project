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
// M3 scope: decision map is fully populated with BB-level entries.
// Physical BB-level section emission is deferred to M5; M3 uses the
// existing function-level hfsort+ order for the actual .text layout.
//
//===----------------------------------------------------------------------===//

#include "SectionEmitter.h"
#include "Config.h"
#include "InputSection.h"
#include "SyntheticSections.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"


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
      // Alignment: M5 will read per-BB alignment from BBAddrMap
      // BBEntry::Alignment. For M3, alignment is always 1 (no padding).

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
  // In M2, this section was header-only (num_entries=0). M3 fills the
  // per-BB entries from the pipeline result.
  //
  // The XBBRDecisionMapSection lives in ctx.mainPart->xbbrDecisionMap.
  // We set the entry count and populate the entry data.
  Partition *mainPart = ctx.mainPart;
  if (!mainPart || !mainPart->xbbrDecisionMap)
    return;

  XBBRDecisionMapSection &dm = *mainPart->xbbrDecisionMap;
  dm.setDegraded(result.Degraded);

  // Build per-BB decision entries (PLAN §9.4, 32-byte stride).
  std::vector<XBBRDecisionEntry> entries;
  entries.reserve(fragments.size());
  uint32_t a = 0, m = 0;
  for (const auto &frag : fragments) {
    const XBBRNode &node = graph.nodes()[frag.NodeIdx];
    XBBRDecisionEntry e;
    e.BBIndex = node.BB;
    e.NewAddress = frag.FinalVA;
    e.ClusterId = 0; // simplified for M3
    if (node.isAnchor()) {
      e.DecisionFlags = 2; // anchored
      ++a;
    } else {
      e.DecisionFlags = 1; // moved
      ++m;
    }
    // orig_func_addr: placeholder 0. M5 patches to the function entry
    // block's real linked VA after assignOffsets. (PLAN §9.4: this is
    // the linker-time absolute address, not a node index.)
    e.OrigFuncAddr = 0; // M5 → funcSection(Func)->getVA()
    entries.push_back(e);
  }
  dm.setEntries(std::move(entries));

  if (ctx.arg.xbbrStats)
    errs() << "xbbr-emit: decisionEntries=" << fragments.size()
           << " moved=" << m << " anchored=" << a << "\n";

  // Build placements for M5 physical emission.
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
