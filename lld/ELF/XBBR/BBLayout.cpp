//===- BBLayout.cpp - XBBR Stage 2: ExtTSP BB layout -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stage 2 of the XBBR linker pipeline (PLAN §4.3): per-cluster BB layout
// using ExtTSP. For each FunctionCluster from Stage 1, collect migratable
// BBs, build ExtTSP input arrays, call computeExtTspLayout(), then
// interleave anchored BBs at their original positions.
//
//===----------------------------------------------------------------------===//

#include "BBLayoutStrategy.h"
#include "Config.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"
#include "llvm/Transforms/Utils/CodeLayout.h"

#include <unordered_map>

using namespace llvm;
using namespace llvm::codelayout;

namespace lld::elf::xbbr {

namespace {

class ExtTSPStrategy : public BBLayoutStrategy {
public:
  explicit ExtTSPStrategy(unsigned mAlign) : maxAlign(mAlign) { (void)this->maxAlign; } // M3-T03

  const char *name() const override { return "ExtTSP"; }

  std::vector<uint32_t> run(const XBBRGraph &graph,
                            const FunctionCluster &cluster,
                            const CostWeights &weights) override;

private:
  unsigned maxAlign;
};

} // namespace

std::vector<uint32_t> ExtTSPStrategy::run(const XBBRGraph &graph,
                                          const FunctionCluster &cluster,
                                          const CostWeights & /*weights*/) {
  ArrayRef<XBBRNode> allNodes = graph.nodes();

  // Phase 1: separate migratable BBs from anchors.
  std::vector<uint32_t> migratable;
  std::vector<bool> isAnchor(allNodes.size(), false);

  for (FuncId F : cluster.Members) {
    auto fn = graph.funcs()[F];
    for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I) {
      if (allNodes[I].isAnchor()) {
        isAnchor[I] = true;
      } else {
        migratable.push_back(I);
      }
    }
  }

  if (migratable.empty()) {
    // Nothing to optimize — anchors-only, keep original order.
    std::vector<uint32_t> result;
    for (FuncId F : cluster.Members) {
      auto fn = graph.funcs()[F];
      for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I)
        result.push_back(I);
    }
    return result;
  }

  // For ≤2 migratable BBs, ExtTSP offers no benefit; return original order
  // with migratable BBs after their entry blocks.
  if (migratable.size() <= 2) {
    std::vector<uint32_t> result;
    for (FuncId F : cluster.Members) {
      auto fn = graph.funcs()[F];
      for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I) {
        if (isAnchor[I])
          result.push_back(I);
      }
    }
    for (uint32_t M : migratable)
      result.push_back(M);
    return result;
  }

  // Phase 2: build local-index mapping.
  std::vector<uint32_t> localToGlobal = migratable;
  std::unordered_map<uint32_t, uint32_t> globalToLocal;
  for (uint32_t L = 0; L < localToGlobal.size(); ++L)
    globalToLocal[localToGlobal[L]] = L;

  // Phase 3: build ExtTSP input arrays.
  SmallVector<uint64_t, 128> nodeSizes;
  SmallVector<uint64_t, 128> nodeCounts;
  SmallVector<EdgeCount, 256> edgeCounts;
  nodeSizes.reserve(localToGlobal.size());
  nodeCounts.reserve(localToGlobal.size());

  for (uint32_t L = 0; L < localToGlobal.size(); ++L) {
    const XBBRNode &N = allNodes[localToGlobal[L]];
    nodeSizes.push_back(N.Size);
    nodeCounts.push_back(N.GlobalFreq);
  }

  // Collect edges where both endpoints are in the migratable set.
  for (const XBBREdge &E : graph.edges()) {
    auto SrcIt = globalToLocal.find(E.SrcNode);
    if (SrcIt == globalToLocal.end())
      continue;
    auto DstIt = globalToLocal.find(E.DstNode);
    if (DstIt == globalToLocal.end())
      continue;
    edgeCounts.push_back(
        {SrcIt->second, DstIt->second, E.Weight});
  }

  // Phase 4: run ExtTSP.
  std::vector<uint64_t> extTspOrder =
      computeExtTspLayout(nodeSizes, nodeCounts, edgeCounts);

  // Phase 5: remap back to global indices.
  std::vector<uint32_t> result;
  // Anchors first (in original function order).
  for (FuncId F : cluster.Members) {
    auto fn = graph.funcs()[F];
    for (uint32_t I = fn.FirstNode; I < fn.FirstNode + fn.NumNodes; ++I) {
      if (isAnchor[I])
        result.push_back(I);
    }
  }
  // Migratable BBs in ExtTSP order.
  for (uint64_t Local : extTspOrder) {
    if (Local < localToGlobal.size())
      result.push_back(localToGlobal[Local]);
  }

  return result;
}

/// Pettis-Hansen-style chain merge: post-processes an ExtTSP layout by
/// repositioning dangling BBs (those with heavy cross-cluster edges)
/// toward the cluster boundary closest to their cross-cluster neighbor.
/// Entry blocks are never moved — chains must not cross anchor boundaries.
class PHStrategy : public BBLayoutStrategy {
public:
  explicit PHStrategy(unsigned mAlign) : maxAlign(mAlign) { (void)this->maxAlign; } // M3-T03

  const char *name() const override { return "PH"; }

  std::vector<uint32_t> run(const XBBRGraph &graph,
                            const FunctionCluster &cluster,
                            const CostWeights &weights) override;

private:
  unsigned maxAlign;
};

std::vector<uint32_t> PHStrategy::run(const XBBRGraph &graph,
                                      const FunctionCluster &cluster,
                                      const CostWeights & /*weights*/) {
  // Run ExtTSP first to get the base layout.
  ExtTSPStrategy extTsp(maxAlign);
  std::vector<uint32_t> order = extTsp.run(graph, cluster, CostWeights{});

  if (order.size() <= 4)
    return order;

  ArrayRef<XBBRNode> allNodes = graph.nodes();

  // Build position map.
  std::unordered_map<uint32_t, uint32_t> posInOrder;
  for (uint32_t P = 0; P < order.size(); ++P)
    posInOrder[order[P]] = P;

  // Collect the heaviest dangling edge per in-cluster BB.
  struct DanglingEdge { uint32_t Src; uint32_t Dst; uint64_t Weight; };
  std::unordered_map<uint32_t, DanglingEdge> bestDangling;
  for (const XBBREdge &E : graph.edges()) {
    if (!E.IsCrossFunc)
      continue;
    bool srcIn = posInOrder.count(E.SrcNode) > 0;
    bool dstIn = posInOrder.count(E.DstNode) > 0;
    if (srcIn == dstIn)
      continue;
    uint32_t inNode = srcIn ? E.SrcNode : E.DstNode;
    auto it = bestDangling.find(inNode);
    if (it == bestDangling.end() || E.Weight > it->second.Weight)
      bestDangling[inNode] = {E.SrcNode, E.DstNode, E.Weight};
  }

  // Move dangling BBs toward cluster boundaries (never entry blocks).
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
createBBLayoutStrategy(XBBRLayoutAlgo algo, unsigned maxAlign) {
  switch (algo) {
  case XBBRLayoutAlgo::ExtTSP:
    return std::make_unique<ExtTSPStrategy>(maxAlign);
  case XBBRLayoutAlgo::PH:
    return std::make_unique<PHStrategy>(maxAlign);
  case XBBRLayoutAlgo::Custom:
    return std::make_unique<ExtTSPStrategy>(maxAlign);
  }
  llvm_unreachable("unknown XBBR layout algorithm");
}

} // namespace lld::elf::xbbr
