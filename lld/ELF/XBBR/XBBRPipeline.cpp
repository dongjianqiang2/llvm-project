//===- XBBRPipeline.cpp - XBBR pipeline orchestrator ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "XBBRPipeline.h"
#include "BBLayoutStrategy.h"
#include "Config.h"
#include "ConstraintSolver.h"
#include "CostFunction.h"
#include "DWARFRewriter.h"
#include "SectionEmitter.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"

using namespace llvm;

namespace lld::elf::xbbr {

// Forward declarations of per-stage entry points (defined in their
// respective .cpp files — we forward-declare here to keep the pipeline
// as a thin orchestrator).

std::vector<FunctionCluster> clusterFunctions(const XBBRGraph &graph,
                                              XBBRClusterAlgo algo);

void runXBBRPipeline(Ctx &ctx, XBBRGraph &graph) {
  XBBRLayoutResult result;

  // Stage 1: function clustering (hfsort+/C³).
  result.Clusters =
      clusterFunctions(graph, ctx.arg.xbbrClusterAlgo);

  if (result.Clusters.empty())
    return;

  // Stage 2: per-cluster ExtTSP BB layout.
  auto strategy = createBBLayoutStrategy(ctx.arg.xbbrLayoutAlgo,
                                         ctx.arg.xbbrMode);
  CostWeights cw;
  cw.Icache = ctx.arg.xbbrWeightIcache;
  cw.Itlb = ctx.arg.xbbrWeightItlb;
  cw.Btb = ctx.arg.xbbrWeightBtb;
  cw.Size = ctx.arg.xbbrWeightSize;

  double totalCostBefore = 0.0, totalCostAfter = 0.0;

  for (const FunctionCluster &cl : result.Clusters) {
    std::vector<uint32_t> order = strategy->run(graph, cl);
    totalCostBefore +=
        computeLayoutCost(graph, order, cw, ctx.arg.xbbrMaxAlign);
    // Stage 3: local-search refinement.
    order = localSearchRefine(graph, std::move(order), cw,
                              ctx.arg.xbbrMaxAlign, /*maxIterations=*/100);
    totalCostAfter +=
        computeLayoutCost(graph, order, cw, ctx.arg.xbbrMaxAlign);
    result.ClusterBBOrders.push_back(std::move(order));
  }

  // Stage 4: constraint fallback. If this returns false, the layout
  // cannot satisfy constraints and fallback=none is set — fatal.
  if (!runConstraintSolver(ctx, graph, result))
    return;

  // Stage 5: build decision-map entries + BBFragments.
  runSectionEmitter(ctx, graph, result);

  // Rewrite DWARF/CFI for migrated BBs. Only a stub today; the real
  // pass slots in once physical emission assigns final VAs.
  rewriteDWARF(ctx);

  if (ctx.arg.xbbrStats) {
    uint32_t totalBBs = 0;
    for (const auto &o : result.ClusterBBOrders)
      totalBBs += o.size();
    errs() << "xbbr-pipeline: clusters=" << result.Clusters.size()
           << " layoutBBs=" << totalBBs
           << " strategy=" << strategy->name()
           << " mode=" << (ctx.arg.xbbrMode == XBBRMode::Full ? "full" : "partial")
           << " costBefore=" << static_cast<uint64_t>(totalCostBefore)
           << " costAfter=" << static_cast<uint64_t>(totalCostAfter)
           << " placed=" << result.Placements.size() << "\n";
  }
}

} // namespace lld::elf::xbbr
