//===- XBBRClustering.cpp - XBBR Stage 1: function clustering ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stage 1 of the XBBR linker pipeline (PLAN §4.3): function-level clustering
// using the hfsort+ / C³ density-based merge algorithm. This reimplements the
// core logic of CallGraphSort on the XBBRGraph data structures so downstream
// Stage 2 can run ExtTSP within each cluster.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"
#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <vector>

using namespace llvm;

namespace lld::elf::xbbr {

namespace {

// Same constants as CallGraphSort — hfsort+ paper defaults.
constexpr int MAX_DENSITY_DEGRADATION = 8;
constexpr uint64_t MAX_CLUSTER_SIZE = 1024 * 1024; // 1 MiB

struct ClusterNode {
  int Next = -1;
  int Prev = -1;
  uint64_t Size = 0;
  uint64_t Weight = 0;
  int BestPred = -1;
  uint64_t BestPredWeight = 0;

  double getDensity() const {
    return Size == 0 ? 0.0 : static_cast<double>(Weight) / Size;
  }
};

} // namespace

std::vector<FunctionCluster>
clusterFunctions(const XBBRGraph &graph, XBBRClusterAlgo /*algo*/) {
  ArrayRef<FuncInfo> funcs = graph.funcs();

  // Filter: only functions with BB data (NumNodes > 0) participate.
  SmallVector<FuncId, 16> candidates;
  for (uint32_t F = 0; F < funcs.size(); ++F)
    if (funcs[F].NumNodes > 0)
      candidates.push_back(F);

  if (candidates.empty())
    return {};

  // Build per-function cluster nodes: size = sum of BB sizes,
  // weight = EntryCount (not Σ GlobalFreq — PLAN §3.2).
  std::vector<ClusterNode> nodes(funcs.size());
  for (FuncId F : candidates) {
    auto nr = funcs[F].nodes(graph.nodes());
    uint64_t sz = 0;
    for (const XBBRNode &N : nr)
      sz += N.Size;
    nodes[F].Size = sz;
    nodes[F].Weight = funcs[F].EntryCount;
    nodes[F].Next = static_cast<int>(F);
    nodes[F].Prev = static_cast<int>(F);
  }

  // Find best predecessor for each function: iterate cross-function
  // call edges, accumulate per-callee incoming edge weights.
  for (const XBBREdge &E : graph.edges()) {
    if (!E.IsCrossFunc)
      continue;
    FuncId caller = graph.nodes()[E.SrcNode].Func;
    FuncId callee = graph.nodes()[E.DstNode].Func;
    if (caller == callee || caller >= nodes.size() ||
        callee >= nodes.size())
      continue;
    if (nodes[caller].Size == 0 || nodes[callee].Size == 0)
      continue;
    uint64_t w = E.Weight;
    if (w > nodes[callee].BestPredWeight) {
      nodes[callee].BestPredWeight = w;
      nodes[callee].BestPred = static_cast<int>(caller);
    }
  }

  // Sort candidate functions by density descending.
  SmallVector<FuncId, 16> order(candidates.begin(), candidates.end());
  std::stable_sort(order.begin(), order.end(),
                   [&](FuncId a, FuncId b) {
                     return nodes[a].getDensity() > nodes[b].getDensity();
                   });

  // Density-merge loop.
  for (FuncId F : order) {
    ClusterNode &CN = nodes[F];
    if (CN.BestPred < 0 || CN.Size == 0)
      continue;
    // Find chain heads.
    int predChain = CN.BestPred;
    while (nodes[predChain].Prev != predChain)
      predChain = nodes[predChain].Prev;
    int myChain = static_cast<int>(F);
    while (nodes[myChain].Prev != myChain)
      myChain = nodes[myChain].Prev;
    if (predChain == myChain)
      continue;

    ClusterNode &PC = nodes[predChain];
    ClusterNode &MC = nodes[myChain];
    uint64_t newSize = PC.Size + MC.Size;
    if (newSize > MAX_CLUSTER_SIZE)
      continue;

    double newDensity =
        static_cast<double>(PC.Weight + MC.Weight) / newSize;
    if (newDensity * MAX_DENSITY_DEGRADATION < PC.getDensity())
      continue;

    // Merge chains: append myChain after predChain.
    int lastPred = PC.Prev;
    nodes[lastPred].Next = myChain;
    nodes[myChain].Prev = lastPred;
    MC.Size = PC.Size = newSize;
    MC.Weight = PC.Weight = PC.Weight + MC.Weight;
  }

  // Collect surviving clusters.
  std::vector<FunctionCluster> clusters;
  for (FuncId F : candidates) {
    if (nodes[F].Prev != static_cast<int>(F))
      continue;
    if (nodes[F].Size == 0)
      continue;
    FunctionCluster FC;
    FC.Id = static_cast<uint32_t>(clusters.size());
    int cur = static_cast<int>(F);
    do {
      FC.Members.push_back(static_cast<FuncId>(cur));
      // Use node-size sum rather than Section->getSize() (InputSectionBase
      // is forward-declared here).
      auto nr = funcs[cur].nodes(graph.nodes());
      for (const XBBRNode &N : nr)
        FC.TotalSize += N.Size;
      FC.TotalWeight += funcs[cur].EntryCount;
      cur = nodes[cur].Next;
    } while (cur != static_cast<int>(F) && cur != -1 &&
             static_cast<size_t>(cur) < funcs.size());
    FC.Density = FC.TotalSize == 0
                     ? 0.0
                     : static_cast<double>(FC.TotalWeight) / FC.TotalSize;
    clusters.push_back(std::move(FC));
  }

  // Sort clusters by density descending.
  std::stable_sort(clusters.begin(), clusters.end(),
                   [](const FunctionCluster &a, const FunctionCluster &b) {
                     return a.Density > b.Density;
                   });

  return clusters;
}

} // namespace lld::elf::xbbr
