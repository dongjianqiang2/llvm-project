//===- CostFunction.cpp - XBBR Stage 3: multi-objective cost -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stage 3 of the XBBR linker pipeline (PLAN §4.3): multi-objective cost
// function with projected-address model and local-search refinement.
//
// Cost = w_icache * IcacheCrossings
//      + w_itlb   * TLBPageCrossings
//      + w_btb    * BTBPressure
//      + w_size   * SizeOverhead
//
// Uses projected offsets (sum of aligned sizes) to break the circular
// dependency between cost and final addresses. Stage 5 verifies against
// actual addresses.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "CostFunction.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"

#include <algorithm>
#include <unordered_set>

using namespace llvm;

namespace lld::elf::xbbr {

namespace {

constexpr unsigned ICACHE_LINE = 64;
constexpr unsigned PAGE_SIZE = 4096;

// E9 <rel32> thunk byte size — used in Stage 5 cost verification.

} // namespace

/// Compute projected offsets for a single cluster's BB order (PLAN §4.3
/// Stage 3). Defined at namespace scope (not anonymous) so Stage 4 can reuse
/// it for branch-range testing — see CostFunction.h.
std::vector<ProjectedBB>
computeProjectedOffsets(const XBBRGraph &graph,
                        const std::vector<uint32_t> &order,
                        unsigned maxAlign) {
  std::vector<ProjectedBB> result;
  result.reserve(order.size());
  uint64_t offset = 0;
  for (uint32_t N : order) {
    const XBBRNode &node = graph.nodes()[N];
    // Cap alignment.
    unsigned align = maxAlign;
    if (align > 1 && (offset % align) != 0)
      offset += align - (offset % align);
    ProjectedBB pb{N, offset, node.Size};
    result.push_back(pb);
    offset += node.Size;
  }
  return result;
}

namespace {

double computeIcacheCrossings(const XBBRGraph &graph,
                              const std::vector<ProjectedBB> &proj,
                              const std::vector<uint32_t> &order,
                              const CostWeights &w) {
  if (w.Icache == 0)
    return 0.0;
  // Build position → projected offset map.
  std::unordered_map<uint32_t, const ProjectedBB *> nodeToProj;
  for (const auto &p : proj)
    nodeToProj[p.NodeIdx] = &p;

  double cost = 0.0;
  for (const XBBREdge &E : graph.edges()) {
    auto sIt = nodeToProj.find(E.SrcNode);
    auto dIt = nodeToProj.find(E.DstNode);
    if (sIt == nodeToProj.end() || dIt == nodeToProj.end())
      continue;
    uint64_t sEnd = sIt->second->Offset + sIt->second->Size;
    uint64_t dStart = dIt->second->Offset;
    // Crossing a cache line boundary: src end and dst start are on
    // different 64-byte lines.
    if ((sEnd / ICACHE_LINE) != (dStart / ICACHE_LINE))
      cost += w.Icache * static_cast<double>(E.Weight);
  }
  return cost;
}

double computeTLBCrossings(const XBBRGraph &graph,
                           const std::vector<ProjectedBB> &proj,
                           const CostWeights &w) {
  if (w.Itlb == 0)
    return 0.0;
  std::unordered_map<uint32_t, const ProjectedBB *> nodeToProj;
  for (const auto &p : proj)
    nodeToProj[p.NodeIdx] = &p;

  double cost = 0.0;
  for (const XBBREdge &E : graph.edges()) {
    auto sIt = nodeToProj.find(E.SrcNode);
    auto dIt = nodeToProj.find(E.DstNode);
    if (sIt == nodeToProj.end() || dIt == nodeToProj.end())
      continue;
    // Crossing a page boundary.
    uint64_t sPage = sIt->second->Offset / PAGE_SIZE;
    uint64_t dPage = dIt->second->Offset / PAGE_SIZE;
    if (sPage != dPage)
      cost += w.Itlb * static_cast<double>(E.Weight);
  }
  return cost;
}

double computeBTBPressure(const XBBRGraph &graph,
                          const std::vector<ProjectedBB> &proj,
                          const CostWeights &w) {
  if (w.Btb == 0)
    return 0.0;
  std::unordered_map<uint32_t, const ProjectedBB *> nodeToProj;
  for (const auto &p : proj)
    nodeToProj[p.NodeIdx] = &p;

  // Count unique (src_addr, dst_addr) pairs using a collision-free
  // pair hash (not (hi<<32)|lo which truncates when Offset ≥ 4 GiB).
  std::unordered_set<uint64_t> seen;
  for (const XBBREdge &E : graph.edges()) {
    auto sIt = nodeToProj.find(E.SrcNode);
    auto dIt = nodeToProj.find(E.DstNode);
    if (sIt == nodeToProj.end() || dIt == nodeToProj.end())
      continue;
    // Hash a (u64,u64) pair into a u64 via splitmix64-ish mixing,
    // avoiding the truncation of (hi<<32)|lo.
    uint64_t s = sIt->second->Offset, d = dIt->second->Offset;
    uint64_t h = s * 0x9E3779B97F4A7C15ULL;
    h ^= d + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    seen.insert(h);
  }
  return static_cast<double>(seen.size()) * w.Btb;
}

double computeSizeOverhead(const std::vector<ProjectedBB> &proj,
                           unsigned maxAlign) {
  // Count alignment padding bytes and estimated thunk bytes for
  // broken fallthrough edges.
  (void)maxAlign;
  double overhead = 0.0;
  for (size_t I = 1; I < proj.size(); ++I) {
    uint64_t prevEnd = proj[I - 1].Offset + proj[I - 1].Size;
    if (proj[I].Offset > prevEnd) {
      // Padding between BBs → alignment overhead.
      overhead += static_cast<double>(proj[I].Offset - prevEnd);
    }
  }
  return overhead;
}

} // namespace

double computeLayoutCost(const XBBRGraph &graph,
                         const std::vector<uint32_t> &order,
                         const CostWeights &weights,
                         unsigned maxAlign) {
  auto proj = computeProjectedOffsets(graph, order, maxAlign);
  return computeIcacheCrossings(graph, proj, order, weights) +
         computeTLBCrossings(graph, proj, weights) +
         computeBTBPressure(graph, proj, weights) +
         weights.Size * computeSizeOverhead(proj, maxAlign);
}

std::vector<uint32_t> localSearchRefine(const XBBRGraph &graph,
                                        std::vector<uint32_t> order,
                                        const CostWeights &weights,
                                        unsigned maxAlign,
                                        unsigned maxIterations) {
  if (order.size() < 3)
    return order;

  // Scale maxIterations inversely with order size so the total per-cluster
  // cost is roughly constant: a 1024-BB cluster costs O(1024) per cost eval;
  // a 32-BB cluster costs O(32). This prevents 787 tiny clusters from
  // running 100 iterations each at O(E). See PLAN §4.3 Stage 3.
  if (order.size() > 512)
    maxIterations = std::min(maxIterations, unsigned(4));
  else if (order.size() > 256)
    maxIterations = std::min(maxIterations, unsigned(6));
  else if (order.size() > 64)
    maxIterations = std::min(maxIterations, unsigned(8));
  else
    maxIterations = std::min(maxIterations, unsigned(4));

  // ---- Pre-filter edges to only those whose both endpoints are in `order`.
  //      Without this every cost-eval scans all ~35K edges (O(E)); with it
  //      each cost-eval scans only O(F) where F ≪ E (typically ~50 edges
  //      for a 30-BB cluster). This is a ~700× speedup per call.
  std::unordered_set<uint32_t> nodeInOrder(order.begin(), order.end());
  std::vector<XBBREdge> localEdges;
  localEdges.reserve(512);
  for (const XBBREdge &E : graph.edges())
    if (nodeInOrder.count(E.SrcNode) && nodeInOrder.count(E.DstNode))
      localEdges.push_back(E);

  // Inline cost evaluator that scans only localEdges (not graph.edges()).
  auto evalCost = [&](const std::vector<uint32_t> &ord) {
    auto proj = computeProjectedOffsets(graph, ord, maxAlign);
    std::unordered_map<uint32_t, const ProjectedBB *> n2p;
    for (const auto &p : proj)
      n2p[p.NodeIdx] = &p;
    double cost = 0.0;
    if (weights.Icache)
      for (const XBBREdge &E : localEdges) {
        auto s = n2p.find(E.SrcNode), d = n2p.find(E.DstNode);
        if (s == n2p.end() || d == n2p.end()) continue;
        uint64_t sEnd = s->second->Offset + s->second->Size;
        if ((sEnd / ICACHE_LINE) != (d->second->Offset / ICACHE_LINE))
          cost += weights.Icache * static_cast<double>(E.Weight);
      }
    if (weights.Itlb)
      for (const XBBREdge &E : localEdges) {
        auto s = n2p.find(E.SrcNode), d = n2p.find(E.DstNode);
        if (s == n2p.end() || d == n2p.end()) continue;
        if (s->second->Offset / PAGE_SIZE != d->second->Offset / PAGE_SIZE)
          cost += weights.Itlb * static_cast<double>(E.Weight);
      }
    if (weights.Btb) {
      std::unordered_set<uint64_t> seen;
      for (const XBBREdge &E : localEdges) {
        auto s = n2p.find(E.SrcNode), d = n2p.find(E.DstNode);
        if (s == n2p.end() || d == n2p.end()) continue;
        uint64_t h = s->second->Offset * 0x9E3779B97F4A7C15ULL;
        h ^= d->second->Offset + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        seen.insert(h);
      }
      cost += static_cast<double>(seen.size()) * weights.Btb;
    }
    if (weights.Size)
      for (size_t I = 1; I < proj.size(); ++I)
        if (proj[I].Offset > proj[I - 1].Offset + proj[I - 1].Size)
          cost += weights.Size * static_cast<double>(
              proj[I].Offset - proj[I - 1].Offset - proj[I - 1].Size);
    return cost;
  };

  auto bestCost = evalCost(order);
  bool improved = true;
  unsigned iters = 0;

  while (improved && iters < maxIterations) {
    improved = false;
    ++iters;

    // Pass 1: adjacent swaps (one full scan).
    for (size_t I = 0; I + 1 < order.size(); ++I) {
      if (graph.nodes()[order[I]].isAnchor() ||
          graph.nodes()[order[I + 1]].isAnchor())
        continue;
      std::swap(order[I], order[I + 1]);
      double newCost = evalCost(order);
      if (newCost < bestCost) { bestCost = newCost; improved = true; }
      else { std::swap(order[I], order[I + 1]); }
    }

    if (!improved) break; // early exit: no improvement in Pass 1

    // Pass 2: single-BB moves within ±window.
    int bestI = -1, bestJ = -1;
    double bestImprovement = 0.0;
    const size_t window = std::min(size_t(6), order.size() - 1);
    for (size_t I = 0; I < order.size(); ++I) {
      if (graph.nodes()[order[I]].isAnchor()) continue;
      size_t lo = I > window ? I - window : 0;
      size_t hi = std::min(order.size() - 1, I + window);
      for (size_t J = lo; J <= hi; ++J) {
        if (I == J || graph.nodes()[order[J]].isAnchor()) continue;
        uint32_t moved = order[I];
        if (I < J) { for (size_t K = I; K < J; ++K) order[K] = order[K + 1]; order[J] = moved; }
        else { for (size_t K = I; K > J; --K) order[K] = order[K - 1]; order[J] = moved; }
        double imp = bestCost - evalCost(order);
        if (imp > bestImprovement) { bestImprovement = imp; bestI = static_cast<int>(I); bestJ = static_cast<int>(J); }
        // Revert.
        if (J < I) { for (size_t K = J; K < I; ++K) order[K] = order[K + 1]; order[I] = moved; }
        else { for (size_t K = J; K > I; --K) order[K] = order[K - 1]; order[I] = moved; }
      }
    }

    if (bestI >= 0) {
      uint32_t moved = order[bestI];
      if (bestI < bestJ) { for (size_t K = bestI; K < size_t(bestJ); ++K) order[K] = order[K + 1]; order[bestJ] = moved; }
      else { for (size_t K = bestI; K > size_t(bestJ); --K) order[K] = order[K - 1]; order[bestJ] = moved; }
      bestCost = evalCost(order);
    }
  }

  return order;
}

} // namespace lld::elf::xbbr
