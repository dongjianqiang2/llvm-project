//===- CostFunction.h - XBBR Stage 3: cost function header -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_COSTFUNCTION_H
#define LLD_ELF_XBBR_COSTFUNCTION_H

#include <cstdint>
#include <vector>

namespace lld::elf::xbbr {

class XBBRGraph;
struct CostWeights;

/// Projected byte placement of one BB within a layout: the offset from the
/// layout base (after accumulating preceding BBs with alignment capping) and
/// the BB's own size. The projected-address model (PLAN §4.3 Stage 3) breaks
/// the cost↔address circular dependency by estimating positions from BB sizes
/// before real VAs are assigned. Stage 4 reuses the same projection to test
/// branch-range constraints (PLAN §4.3 Stage 4).
struct ProjectedBB {
  uint32_t NodeIdx = 0;   ///< index into XBBRGraph::nodes
  uint64_t Offset = 0;    ///< projected byte offset from the layout base
  uint64_t Size = 0;      ///< BB byte size (XBBRNode::Size)
};

/// Compute projected offsets for a single cluster's BB order. Each BB is
/// placed at the running offset, alignment-capped to `maxAlign` (PLAN §4.3
/// Stage 3 "对齐上限"). Exposed so Stage 4 can build a global projection
/// across all clusters and test branch ranges without final VAs.
std::vector<ProjectedBB>
computeProjectedOffsets(const XBBRGraph &graph,
                        const std::vector<uint32_t> &order,
                        unsigned maxAlign);

/// Compute the multi-objective cost for a given BB layout order.
/// Uses projected addresses (sum of aligned sizes), not final VAs.
double computeLayoutCost(const XBBRGraph &graph,
                         const std::vector<uint32_t> &order,
                         const CostWeights &weights,
                         unsigned maxAlign = 16);

/// Local-search refinement: swap and single-BB move operations that
/// reduce cost. Returns the improved order.
std::vector<uint32_t> localSearchRefine(const XBBRGraph &graph,
                                        std::vector<uint32_t> order,
                                        const CostWeights &weights,
                                        unsigned maxAlign = 16,
                                        unsigned maxIterations = 100);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_COSTFUNCTION_H
