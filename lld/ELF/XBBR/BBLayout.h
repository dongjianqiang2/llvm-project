//===- BBLayout.h - XBBR Stage 2: BB layout header -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_BBLAYOUT_H
#define LLD_ELF_XBBR_BBLAYOUT_H

#include "Config.h"
#include "XBBR/XBBRTypes.h"

namespace lld::elf::xbbr {

class XBBRGraph;

/// Separate migratable from non-migratable BBs within a cluster.
/// In `partial` mode, cold BBs (isCold()) stay with their original
/// functions (are excluded from migratable). In `full` mode, cold BBs
/// may also migrate — only §5.3 blacklisted anchors are excluded.
void collectMigratableBBs(const XBBRGraph &graph,
                          const FunctionCluster &cluster,
                          XBBRMode mode,
                          std::vector<uint32_t> &migratable,
                          std::vector<bool> &isAnchor);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_BBLAYOUT_H
