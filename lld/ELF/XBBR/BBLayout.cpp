//===- BBLayout.cpp - XBBR Stage 2: ExtTSP BB layout -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "BBLayout.h"
#include "BBLayoutStrategy.h"
#include "Config.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"
#include "llvm/Transforms/Utils/CodeLayout.h"

#include <unordered_map>

using namespace llvm;
using namespace llvm::codelayout;

namespace lld::elf::xbbr {

void collectMigratableBBs(const XBBRGraph &graph,
                          const FunctionCluster &cluster, XBBRMode mode,
                          std::vector<uint32_t> &migratable,
                          std::vector<bool> &isAnchor) {
  ArrayRef<XBBRNode> allNodes = graph.nodes();
  for (FuncId F : cluster.Members) {
    auto fn = graph.funcs()[F];
    // Phase 1b EH gate: an LSDA/landing-pad function may move wholesale (all
    // BBs contiguous — LSDA call_site offsets are entry-relative and survive a
    // contiguous shift) but its individual BBs must NOT migrate cross-function,
    // which would break the call_site ranges. Treat every BB of an EH-gated
    // function as a non-migratable anchor; ExtTSP then leaves the function
    // intact at its cluster slot.
    const bool ehGated = fn.IsEHGated;
    for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I) {
      if (ehGated || allNodes[I].isAnchor()) {
        // isAnchor: SPEC §5.3 blacklist (entry/landing-pad/musttail/...).
        // ehGated: LSDA function — wholesale-only (Phase 1b).
        isAnchor[I] = true;
      } else if (allNodes[I].CondInvolved && !allNodes[I].CondSafeToMigrate) {
        // P1-3: a cond-branch BB whose connected component isn't all-hot
        // (cross-section risk: a partner would land in .text while this BB
        // goes to .text.hot) stays in .text. Covers cold cond BBs (never
        // CondSafeToMigrate) and ARM R_ARM_THM_JUMP11 (unconditionally
        // pinned). Hot cond BBs in an all-hot component may migrate; Stage 4
        // range-checks the rare within-.text.hot over-range case.
        isAnchor[I] = true;
      } else if (mode == XBBRMode::Partial && allNodes[I].isCold()) {
        // In partial mode, cold BBs stay with their original functions
        // (SPEC §4: only hot BBs may migrate cross-function).
        isAnchor[I] = true; // treat as anchor — never migrates
      } else {
        // Includes hot CondInvolved BBs with CondSafeToMigrate (P1-3
        // relaxation): they enter the ExtTSP layout; Stage 4 reverts (pins)
        // any whose B.cond/TBZ becomes over-range within .text.hot.
        migratable.push_back(I);
      }
    }
  }
}

namespace {

class ExtTSPStrategy : public BBLayoutStrategy {
  XBBRMode mode;
public:
  explicit ExtTSPStrategy(XBBRMode m) : mode(m) {}
  const char *name() const override { return "ExtTSP"; }
  std::vector<uint32_t> run(const XBBRGraph &graph,
                            const FunctionCluster &cluster) override;
};

} // namespace

std::vector<uint32_t> ExtTSPStrategy::run(const XBBRGraph &graph,
                                          const FunctionCluster &cluster) {
  ArrayRef<XBBRNode> allNodes = graph.nodes();

  std::vector<uint32_t> migratable;
  std::vector<bool> isAnchor(allNodes.size(), false);
  collectMigratableBBs(graph, cluster, mode, migratable, isAnchor);

  if (migratable.empty()) {
    std::vector<uint32_t> result;
    for (FuncId F : cluster.Members) {
      auto fn = graph.funcs()[F];
      for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I)
        result.push_back(I);
    }
    return result;
  }

  if (migratable.size() <= 2) {
    std::vector<uint32_t> result;
    for (FuncId F : cluster.Members) {
      auto fn = graph.funcs()[F];
      for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I)
        if (isAnchor[I]) result.push_back(I);
    }
    for (uint32_t M : migratable) result.push_back(M);
    return result;
  }

  std::vector<uint32_t> localToGlobal = migratable;
  std::unordered_map<uint32_t, uint32_t> globalToLocal;
  for (uint32_t L = 0; L < localToGlobal.size(); ++L)
    globalToLocal[localToGlobal[L]] = L;

  SmallVector<uint64_t, 128> nodeSizes, nodeCounts;
  SmallVector<EdgeCount, 256> edgeCounts;
  for (uint32_t L = 0; L < localToGlobal.size(); ++L) {
    const XBBRNode &N = allNodes[localToGlobal[L]];
    nodeSizes.push_back(N.Size);
    nodeCounts.push_back(N.GlobalFreq);
  }

  for (const XBBREdge &E : graph.edges()) {
    auto SrcIt = globalToLocal.find(E.SrcNode);
    if (SrcIt == globalToLocal.end()) continue;
    auto DstIt = globalToLocal.find(E.DstNode);
    if (DstIt == globalToLocal.end()) continue;
    edgeCounts.push_back({SrcIt->second, DstIt->second, E.Weight});
  }

  std::vector<uint64_t> extTspOrder =
      computeExtTspLayout(nodeSizes, nodeCounts, edgeCounts);

  std::vector<uint32_t> result;
  size_t cursor = 0;
  for (FuncId F : cluster.Members) {
    auto fn = graph.funcs()[F];
    for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I) {
      if (isAnchor[I]) {
        result.push_back(I);
      } else if (cursor < extTspOrder.size()) {
        uint64_t localIdx = extTspOrder[cursor++];
        if (localIdx < localToGlobal.size())
          result.push_back(localToGlobal[localIdx]);
      }
    }
  }
  while (cursor < extTspOrder.size()) {
    uint64_t localIdx = extTspOrder[cursor++];
    if (localIdx < localToGlobal.size())
      result.push_back(localToGlobal[localIdx]);
  }

  // Defensive size check.
  uint32_t expectedSize = 0;
  for (FuncId F : cluster.Members)
    expectedSize += graph.funcs()[F].NumNodes;
  if (result.size() != expectedSize) {
    result.clear();
    for (FuncId F : cluster.Members) {
      auto fn = graph.funcs()[F];
      for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I)
        result.push_back(I);
    }
  }

  return result;
}

/// Pettis-Hansen chain merge, mode-aware.
class PHStrategy : public BBLayoutStrategy {
  XBBRMode mode;
public:
  explicit PHStrategy(XBBRMode m) : mode(m) {}
  const char *name() const override { return "PH"; }
  std::vector<uint32_t> run(const XBBRGraph &graph,
                            const FunctionCluster &cluster) override;
};

std::vector<uint32_t> PHStrategy::run(const XBBRGraph &graph,
                                      const FunctionCluster &cluster) {
  ExtTSPStrategy extTsp(mode);
  std::vector<uint32_t> order = extTsp.run(graph, cluster);
  if (order.size() <= 4) return order;

  ArrayRef<XBBRNode> allNodes = graph.nodes();
  std::unordered_map<uint32_t, uint32_t> posInOrder;
  for (uint32_t P = 0; P < order.size(); ++P) posInOrder[order[P]] = P;

  struct DanglingEdge { uint32_t Src; uint32_t Dst; uint64_t Weight; };
  std::unordered_map<uint32_t, DanglingEdge> bestDangling;
  for (const XBBREdge &E : graph.edges()) {
    if (!E.IsCrossFunc) continue;
    bool srcIn = posInOrder.count(E.SrcNode) > 0;
    bool dstIn = posInOrder.count(E.DstNode) > 0;
    if (srcIn == dstIn) continue;
    uint32_t inNode = srcIn ? E.SrcNode : E.DstNode;
    auto it = bestDangling.find(inNode);
    if (it == bestDangling.end() || E.Weight > it->second.Weight)
      bestDangling[inNode] = {E.SrcNode, E.DstNode, E.Weight};
  }

  size_t quart = order.size() / 4;
  std::vector<uint32_t> head, middle, tail;
  for (uint32_t P = 0; P < order.size(); ++P) {
    uint32_t N = order[P];
    bool dangling = bestDangling.count(N) > 0;
    if (dangling && P < quart && !allNodes[N].isEntry())
      tail.push_back(N);
    else if (dangling && P >= 3 * quart && !allNodes[N].isEntry())
      head.push_back(N);
    else
      middle.push_back(N);
  }

  std::vector<uint32_t> result;
  result.insert(result.end(), head.begin(), head.end());
  result.insert(result.end(), middle.begin(), middle.end());
  result.insert(result.end(), tail.begin(), tail.end());
  return result;
}

std::unique_ptr<BBLayoutStrategy>
createBBLayoutStrategy(XBBRLayoutAlgo algo, XBBRMode mode) {
  switch (algo) {
  case XBBRLayoutAlgo::ExtTSP: return std::make_unique<ExtTSPStrategy>(mode);
  case XBBRLayoutAlgo::PH:     return std::make_unique<PHStrategy>(mode);
  case XBBRLayoutAlgo::Custom: return std::make_unique<ExtTSPStrategy>(mode);
  }
  llvm_unreachable("unknown XBBR layout algorithm");
}

} // namespace lld::elf::xbbr
