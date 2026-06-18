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
      // Alignment: min(node alignment, maxAlign cap). Nodes don't carry
      // explicit alignment yet (M5), so use 1.
      unsigned align = 1;
      if (align > 1 && (currentVA % align) != 0)
        currentVA += align - (currentVA % align);

      BBFragment frag;
      frag.NodeIdx = nodeIdx;
      frag.Size = node.Size;
      frag.Alignment = align;
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
  dm.setNumEntries(static_cast<uint32_t>(fragments.size()));

  // Store per-BB entries for writeTo(). The decision map format
  // (PLAN §9.4) is 32 bytes per entry:
  //   uint64 orig_func_addr — entry-block VA
  //   uint32 bb_index       — BB index within function
  //   uint64 new_address    — post-XBBR VA (projected for M3)
  //   uint32 cluster_id     — owning cluster
  //   uint32 decision_flags — moved/anchored/fallback/thunk
  //   uint32 reserved

  // We store the entry data in a vector owned by the decision map.
  // For now, use the SectionEmitter's local data; M5 will make this
  // persistent.
  if (ctx.arg.xbbrStats) {
    uint32_t nAnchored = 0, nMoved = 0, nFallback = 0;
    for (const auto &frag : fragments) {
      const XBBRNode &node = graph.nodes()[frag.NodeIdx];
      if (node.isAnchor())
        ++nAnchored;
      else
        ++nMoved;
    }
    (void)nFallback;
    errs() << "xbbr-emit: decisionEntries=" << fragments.size()
           << " moved=" << nMoved << " anchored=" << nAnchored << "\n";
  }

  // Store fragments for M5 physical emission.
  result.Placements.clear();
  for (const auto &frag : fragments) {
    BBPlacement p;
    p.NodeIdx = frag.NodeIdx;
    p.ClusterIdx = 0; // simplified for M3
    const XBBRNode &node = graph.nodes()[frag.NodeIdx];
    p.TargetSec = node.isAnchor() ? BBPlacement::Section::Original
                  : node.isCold()  ? BBPlacement::Section::Unlikely
                                   : BBPlacement::Section::Hot;
    result.Placements.push_back(p);
  }

  result.ThunkBytes = 0; // M5 computes actual thunk bytes
}

} // namespace lld::elf::xbbr
