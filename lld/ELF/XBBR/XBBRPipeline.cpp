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

#include "llvm/ADT/SmallVector.h"

using namespace llvm;

namespace lld::elf::xbbr {

// Forward declarations of per-stage entry points (defined in their
// respective .cpp files — we forward-declare here to keep the pipeline
// as a thin orchestrator).

std::vector<FunctionCluster> clusterFunctions(const XBBRGraph &graph,
                                              XBBRClusterAlgo algo);

namespace {
// Maximum BBs per cluster before splitting into sub-clusters. Aligned with
// ExtTSP's MaxChainSize (512) — with 2× headroom so ExtTSP has enough
// candidates to find profitable merges without hitting the O(N²) wall.
// PLAN §4.3 Stage 2: "max_bbs_per_cluster / max_bytes_per_cluster thresholds"
// and "sub-cluster splitting by hot sub-paths".
constexpr uint32_t MAX_BBS_PER_SUBCLUSTER = 1024;

/// Count the total number of BBs spanned by a cluster's member functions.
static uint32_t countBBsInCluster(const FunctionCluster &cl,
                                  const XBBRGraph &graph) {
  uint32_t total = 0;
  for (FuncId F : cl.Members)
    total += graph.funcs()[F].NumNodes;
  return total;
}

/// Split a single oversized cluster into multiple sub-clusters, packing
/// functions greedily by density so each sub-cluster stays within the BB
/// limit. Functions within a sub-cluster keep their original density order;
/// cross-sub-cluster ordering is by the density of the first (densest)
/// function in each sub-cluster. The existing cluster Id and density are
/// inherited; new sub-cluster Ids are assigned sequentially.
static std::vector<FunctionCluster>
splitCluster(const FunctionCluster &cl, const XBBRGraph &graph) {
  uint32_t totalBBs = countBBsInCluster(cl, graph);
  if (totalBBs <= MAX_BBS_PER_SUBCLUSTER)
    return {cl}; // no split needed

  // Sort members by per-function density descending so the hottest functions
  // anchor the first sub-clusters.
  SmallVector<FuncId, 32> members(cl.Members.begin(), cl.Members.end());
  llvm::sort(members, [&](FuncId A, FuncId B) {
    auto fnA = graph.funcs()[A];
    auto fnB = graph.funcs()[B];
    double dA = fnA.NumNodes ? static_cast<double>(fnA.EntryCount) / fnA.NumNodes
                             : 0.0;
    double dB = fnB.NumNodes ? static_cast<double>(fnB.EntryCount) / fnB.NumNodes
                             : 0.0;
    return dA > dB;
  });

  std::vector<FunctionCluster> subClusters;
  uint32_t currentBBs = 0;
  FunctionCluster cur;
  for (FuncId F : members) {
    uint32_t n = graph.funcs()[F].NumNodes;
    // If adding this function would push the sub-cluster over the limit and
    // the current sub-cluster already has content, start a new one. A single
    // function whose BB count exceeds the limit (very rare — 1K+ BB func)
    // gets its own sub-cluster anyway.
    if (currentBBs > 0 && currentBBs + n > MAX_BBS_PER_SUBCLUSTER) {
      cur.TotalSize = cur.TotalWeight = 0;
      for (FuncId M : cur.Members) {
        auto fn = graph.funcs()[M];
        auto nr = fn.nodes(graph.nodes());
        for (const XBBRNode &N : nr)
          cur.TotalSize += N.Size;
        cur.TotalWeight += fn.EntryCount;
      }
      cur.Density = cur.TotalSize ? static_cast<double>(cur.TotalWeight) /
                                        cur.TotalSize
                                  : 0.0;
      subClusters.push_back(std::move(cur));
      cur = FunctionCluster{};
      currentBBs = 0;
    }
    cur.Members.push_back(F);
    currentBBs += n;
  }
  // Flush the last sub-cluster.
  if (!cur.Members.empty()) {
    cur.TotalSize = cur.TotalWeight = 0;
    for (FuncId M : cur.Members) {
      auto fn = graph.funcs()[M];
      auto nr = fn.nodes(graph.nodes());
      for (const XBBRNode &N : nr)
        cur.TotalSize += N.Size;
      cur.TotalWeight += fn.EntryCount;
    }
    cur.Density = cur.TotalSize ? static_cast<double>(cur.TotalWeight) /
                                      cur.TotalSize
                                : 0.0;
    subClusters.push_back(std::move(cur));
  }

  return subClusters;
}
} // namespace

void runXBBRPipeline(Ctx &ctx, XBBRGraph &graph) {
  // Persist the layout result on Ctx so the post-thunk-loop VA backfill
  // (Phase 3) and physical emitter can consume Stages 1–4 output after
  // addresses are assigned. The rest of this function treats `result` as a
  // local reference for readability.
  ctx.xbbrLayoutResult = std::make_unique<XBBRLayoutResult>();
  XBBRLayoutResult &result = *ctx.xbbrLayoutResult;

  // Safety: hfsort+ density-merge in clusterFunctions does not scale past
  // ~50K candidate functions (the chain-walking merge loop has quadratic
  // worst-case behaviour). For very large binaries (full LLVM: 150K+ funcs)
  // degrade to function-level mode — the CGProfile function order is used,
  // and per-BB sections are co-located with their function via the
  // buildSectionOrder extension (Writer.cpp). This trades cross-function
  // BB migration for correctness.
  static constexpr uint32_t MAX_SAFE_FUNCS = 60000;
  if (graph.funcs().size() > MAX_SAFE_FUNCS) {
    if (ctx.arg.xbbrStats)
      errs() << "xbbr-stage1: too many functions (" << graph.funcs().size()
             << " > " << MAX_SAFE_FUNCS
             << "), degrading to function-level mode\n";
    result.Degraded = true;
    return;
  }

  // Stage 1: function clustering (hfsort+/C³).
  std::vector<FunctionCluster> rawClusters =
      clusterFunctions(graph, ctx.arg.xbbrClusterAlgo);

  if (rawClusters.empty())
    return;

  // Split oversized clusters to keep ExtTSP within O(N²) bounds (PLAN §4.3
  // Stage 2: max_bbs_per_cluster threshold + sub-cluster partitioning).
  result.Clusters.reserve(rawClusters.size());
  for (const FunctionCluster &cl : rawClusters) {
    std::vector<FunctionCluster> sub = splitCluster(cl, graph);
    for (auto &s : sub) {
      s.Id = static_cast<uint32_t>(result.Clusters.size());
      result.Clusters.push_back(std::move(s));
    }
  }

  if (ctx.arg.xbbrStats && result.Clusters.size() != rawClusters.size()) {
    uint32_t totalBBs = 0;
    for (const auto &cl : result.Clusters)
      totalBBs += countBBsInCluster(cl, graph);
    errs() << "xbbr-stage1: clusters=" << rawClusters.size()
           << " subclusters=" << result.Clusters.size()
           << " total-bbs=" << totalBBs << "\n";
  }

  // Stage 2: per-cluster ExtTSP BB layout.
  auto strategy = createBBLayoutStrategy(ctx.arg.xbbrLayoutAlgo,
                                         ctx.arg.xbbrMode);
  CostWeights cw;
  cw.Icache = ctx.arg.xbbrWeightIcache;
  cw.Itlb = ctx.arg.xbbrWeightItlb;
  cw.Btb = ctx.arg.xbbrWeightBtb;
  cw.Size = ctx.arg.xbbrWeightSize;

  double totalCostBefore = 0.0, totalCostAfter = 0.0;

  size_t numLayoutBBs = 0;
  for (const FunctionCluster &cl : result.Clusters)
    numLayoutBBs += countBBsInCluster(cl, graph);
  if (ctx.arg.xbbrStats)
    errs() << "xbbr-stage2-begin: subclusters=" << result.Clusters.size()
           << " total-bbs=" << numLayoutBBs << "\n";

  unsigned processed = 0;
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
    ++processed;
    if (ctx.arg.xbbrStats && (processed % 100 == 0))
      errs() << "xbbr-stage2: processed " << processed << "/"
             << result.Clusters.size() << " subclusters\n";
  }

  if (ctx.arg.xbbrStats)
    errs() << "xbbr-stage2-done: costBefore="
           << static_cast<uint64_t>(totalCostBefore)
           << " costAfter=" << static_cast<uint64_t>(totalCostAfter) << "\n";

  // Stage 4: constraint fallback. If this returns false, the layout
  // cannot satisfy constraints and fallback=none is set — fatal.
  if (!runConstraintSolver(ctx, graph, result))
    return;

  if (ctx.arg.xbbrStats)
    errs() << "xbbr-stage4-done\n";

  // Stage 5: build decision-map entries + BBFragments.
  runSectionEmitter(ctx, graph, result);

  if (ctx.arg.xbbrStats)
    errs() << "xbbr-stage5-done\n";

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
