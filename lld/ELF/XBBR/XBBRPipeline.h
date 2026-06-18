//===- XBBRPipeline.h - XBBR pipeline orchestrator ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Orchestrates the XBBR multi-stage pipeline (PLAN §4.3): clustering,
// BB layout, cost tuning, constraint fallback. Returns a XBBRLayoutResult
// that Stage 5 consumes to emit the final BB-level section layout.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_XBBRPIPELINE_H
#define LLD_ELF_XBBR_XBBRPIPELINE_H

#include "lld/Common/ErrorHandler.h"
#include "llvm/Support/ErrorHandling.h"

namespace lld::elf {
struct Ctx;
namespace xbbr {
class XBBRGraph;
struct XBBRLayoutResult;
} // namespace xbbr
} // namespace lld::elf

namespace lld::elf::xbbr {

/// Run the XBBR pipeline (Stages 1-4) on the global BB graph.
/// Stages:
///   1. clusterFunctions() → vector<FunctionCluster>
///   2. per-cluster ExtTSP BB layout → ClusterBBOrders
///   3. multi-objective cost local search (M3-T03, placeholder)
///   4. constraint fallback loop (M3-T04, placeholder)
///
/// Places result in ctx.xbbrLayoutResult.
void runXBBRPipeline(Ctx &ctx, XBBRGraph &graph);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_XBBRPIPELINE_H
