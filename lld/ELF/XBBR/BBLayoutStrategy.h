//===- BBLayoutStrategy.h - XBBR BB layout algorithm base -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_BBLAYOUTSTRATEGY_H
#define LLD_ELF_XBBR_BBLAYOUTSTRATEGY_H

#include "Config.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace lld::elf::xbbr {

class XBBRGraph;
struct FunctionCluster;

class BBLayoutStrategy {
public:
  virtual ~BBLayoutStrategy() = default;
  virtual std::vector<uint32_t>
  run(const XBBRGraph &graph, const FunctionCluster &cluster) = 0;
  virtual const char *name() const = 0;
};

/// Factory — picks the strategy based on Config.
/// `mode` controls partial vs full: in partial mode cold BBs are excluded
/// from migration; in full mode only §5.3 anchors are excluded.
std::unique_ptr<BBLayoutStrategy>
createBBLayoutStrategy(XBBRLayoutAlgo algo, XBBRMode mode);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_BBLAYOUTSTRATEGY_H
