//===- ConstraintSolver.cpp - XBBR Stage 4: constraint fallback --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stage 4 of the XBBR linker pipeline (PLAN §4.3): pin-based monotonic
// fallback loop. Checks per-BB constraints (branch range, thunk budget,
// blacklist, EH conflicts) and reverts violating BBs to their original
// function positions. If >30% of migratable BBs must be reverted, the
// entire pipeline degrades to function-level mode.
//
// Convergence: reverted BBs are pinned (never migrate again), so the
// set of still-drifting BBs strictly shrinks each iteration — no
// infinite oscillation. Safety net: max iterations = num_migratable + 1.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"
#include "lld/Common/ErrorHandler.h"

#include <algorithm>
#include <unordered_set>

using namespace llvm;

namespace lld::elf::xbbr {

namespace {

constexpr double GLOBAL_FALLBACK_THRESHOLD = 0.30;

/// Check whether a BB violates ISA branch-range constraints.
/// On x86_64, direct branches have ±2 GiB range — effectively never
/// violated in practice. M5 adds AArch64 (±128 MiB) and ARM (±32/16 MiB).
bool violatesBranchRange(const XBBRGraph &graph,
                         const std::vector<uint32_t> &order,
                         const XBBRNode &node,
                         uint64_t /*projectedVA*/) {
  // x86_64: 2 GiB range, no practical violation possible.
  // M5 will add arch-specific range checks for AArch64/ARM.
  (void)graph;
  (void)order;
  (void)node;
  return false;
}

/// Check EH constraints. For M3, all landing pads are already anchored
/// (isAnchor() returns true for IsLandingPad), so they never migrate.
/// M4 will add full EH range conflict detection.
bool violatesEHConstraint(const XBBRNode &node) {
  return node.isLandingPad(); // already handled by isAnchor(), but belt-and-suspenders
}

} // namespace

bool runConstraintSolver(Ctx &ctx, XBBRGraph &graph,
                         XBBRLayoutResult &result) {
  ArrayRef<XBBRNode> allNodes = graph.nodes();

  // Count migratable (non-anchor) BBs across all clusters. Anchors are
  // placed by Stage 2 and never drift, so the 30% fallback threshold
  // (SPEC §7 / PLAN §4.3) applies only to truly migratable BBs.
  uint32_t totalMigratable = 0;
  for (const auto &order : result.ClusterBBOrders)
    for (uint32_t n : order)
      if (!allNodes[n].isAnchor())
        ++totalMigratable;

  if (totalMigratable == 0)
    return true;

  // Pinned set: BBs that have been reverted and must never migrate again.
  std::unordered_set<uint32_t> pinned;
  uint32_t fallbackCount = 0;
  const uint32_t maxIters = totalMigratable + 1;
  const uint32_t fallbackLimit =
      std::max(static_cast<uint32_t>(GLOBAL_FALLBACK_THRESHOLD * totalMigratable),
               1u);

  bool degraded = false;
  uint32_t iter = 0;

  for (iter = 0; iter < maxIters; ++iter) {
    bool changedThisIter = false;

    for (auto &order : result.ClusterBBOrders) {
      // Filter out pinned BBs from the order.
      size_t writePos = 0;
      for (size_t I = 0; I < order.size(); ++I) {
        uint32_t nodeIdx = order[I];
        if (pinned.count(nodeIdx))
          continue;

        const XBBRNode &node = allNodes[nodeIdx];

        // Constraint checks — only for migratable BBs.
        // Anchors are placed by Stage 2 and never drift; skip them.
        bool violated = false;
        if (node.isAnchor()) {
          order[writePos++] = nodeIdx; // keep anchor in place
          continue;
        }

        // 1. Branch range (x86_64: always passes).
        if (violatesBranchRange(graph, order, node, /*va=*/0))
          violated = true;

        // 2. EH constraint.
        if (violatesEHConstraint(node))
          violated = true;

        // 3. Thunk budget (M5).
        // Not checked in M3 — x86_64 has no thunks.

        if (violated) {
          // Pin this BB: it reverts to original function position.
          pinned.insert(nodeIdx);
          ++fallbackCount;
          changedThisIter = true;
          // Don't include it in the output order.
          continue;
        }

        order[writePos++] = nodeIdx;
      }
      order.resize(writePos);
    }

    // Global threshold check.
    if (fallbackCount > fallbackLimit) {
      degraded = true;
      break;
    }

    if (!changedThisIter)
      break; // converged
  }

  if (iter >= maxIters) {
    // Safety net: never loop forever.
    degraded = true;
  }

  if (degraded) {
    if (ctx.arg.xbbrFallback == XBBRFallback::None) {
      ErrAlways(ctx) << "XBBR: constraint violations exceed "
                     << static_cast<int>(GLOBAL_FALLBACK_THRESHOLD * 100)
                     << "% threshold, and --bb-cross-reorder-fallback=none "
                        "is set. Aborting.";
      return false;
    }

    if (ctx.arg.xbbrStats || ctx.arg.xbbrFallback != XBBRFallback::Auto)
      Warn(ctx) << "XBBR: " << fallbackCount << " of " << totalMigratable
                << " migratable BBs reverted (" << (fallbackLimit)
                << " limit); degrading to function-level mode.";

    // Degrade: clear all BB-level orders and record in result.
    result.Degraded = true;
    result.ClusterBBOrders.clear();
    result.Placements.clear();
  }

  return true;
}

} // namespace lld::elf::xbbr
