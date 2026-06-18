//===- ConstraintSolver.h - XBBR Stage 4: constraint solver -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_CONSTRAINTSOLVER_H
#define LLD_ELF_XBBR_CONSTRAINTSOLVER_H

namespace lld::elf {
struct Ctx;
namespace xbbr {
class XBBRGraph;
struct XBBRLayoutResult;
} // namespace xbbr
} // namespace lld::elf

namespace lld::elf::xbbr {

/// Run the pin-based monotonic fallback loop on the layout result.
/// Returns true on success, false if fallback=none and constraints
/// cannot be satisfied (fatal error).
bool runConstraintSolver(Ctx &ctx, XBBRGraph &graph,
                         XBBRLayoutResult &result);

} // namespace lld::elf::xbbr

#endif // LLD_ELF_XBBR_CONSTRAINTSOLVER_H
