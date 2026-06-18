//===- BBLayoutStrategy.h - XBBR BB layout algorithm base -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Abstract base for BB layout algorithms (ExtTSP / PH / Custom), as
// described in PLAN §7.1. Each strategy takes a cluster of functions and
// produces the optimal within-cluster BB order.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_BBLAYOUTSTRATEGY_H
#define LLD_ELF_XBBR_BBLAYOUTSTRATEGY_H

#include "Config.h"
#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lld::elf::xbbr {

class XBBRGraph;
struct FunctionCluster;

/// Abstract base for BB layout algorithms. Each implementation takes
/// a cluster of functions and produces an ordered sequence of global
/// node indices representing the optimal within-cluster BB layout.
class BBLayoutStrategy {
public:
  virtual ~BBLayoutStrategy() = default;

  /// Compute the optimal BB order within the given cluster.
  /// Returns ordered sequence of global node indices.
  virtual std::vector<uint32_t>
  run(const XBBRGraph &graph, const FunctionCluster &cluster) = 0;

  /// Human-readable name for diagnostics / --stats output.
  virtual const char *name() const = 0;
};

/// Factory — picks the strategy based on Config.
std::unique_ptr<BBLayoutStrategy>
createBBLayoutStrategy(XBBRLayoutAlgo algo, unsigned maxAlign = 0);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_BBLAYOUTSTRATEGY_H
