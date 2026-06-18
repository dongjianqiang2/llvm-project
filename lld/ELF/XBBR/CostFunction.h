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
