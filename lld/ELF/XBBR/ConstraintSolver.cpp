//===- ConstraintSolver.cpp - XBBR Stage 4: constraint fallback --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stage 4 of the XBBR linker pipeline (PLAN §4.3): pin-based monotonic
// fallback loop driven by a projected-VA branch-range model.
//
// Model: the post-Stage-2 BB order is projected onto byte offsets (reusing
// CostFunction::computeProjectedOffsets, PLAN §4.3 Stage 3's projected-address
// model). For every thunkable direct branch/call (B/BL on AArch64, i.e.
// R_AARCH64_JUMP26/CALL26 — the relocs lld can extend with a range-extension
// thunk; conditional/test branches CONDBR19/TSTBR14 are unthunkable and
// already pinned by XBBRGraph::markRangeAnchors, so they never reach here),
// we measure the projected src→dst distance. If it exceeds the ISA branch
// range, that edge would force lld to emit a range-extension thunk.
//
// Budget gate (--bb-cross-reorder-max-thunk-bytes, SPEC §7): if the estimated
// total thunk bytes exceed the budget, Stage 4 pins (reverts to original
// function position) the migratable BB involved in the most over-range edges,
// recomputes, and repeats — pinning is monotonic (a pinned BB never migrates
// again), so the loop converges in ≤ totalMigratable+1 iterations. If >30%
// of migratable BBs must be pinned, the whole pipeline degrades to
// function-level mode (SPEC §7). --bb-cross-reorder-fallback=none turns any
// unavoidable degradation into a fatal error (CI strict mode).
//
// What Stage 4 does NOT do: insert thunks. lld's existing
// finalizeAddressDependentContent thunk loop does that, using real (post-layout)
// addresses. Stage 4 only decides which BBs may safely migrate given the
// thunk-byte budget; the tail-end recheck (P0-2, PLAN §4.3 Stage 5 "末梢复核")
// compares the estimate here against real ThunkSection sizes after emission.
//
// EH conflicts are handled upstream by the Phase 1b EH gate
// (FuncInfo::IsEHGated → every BB of an LSDA function is an anchor), so no
// per-BB EH-range check is needed here.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "CostFunction.h"
#include "InputSection.h"
#include "Relocations.h"
#include "Symbols.h"
#include "XBBR/XBBRGraph.h"
#include "XBBR/XBBRTypes.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

namespace lld::elf::xbbr {

namespace {

constexpr double GLOBAL_FALLBACK_THRESHOLD = 0.30;

/// Per-arch direct-branch (thunkable) range and range-extension thunk byte
/// size. Only B/BL-class branches are thunkable by lld. Returns {0,0} for
/// arches XBBR doesn't range-check — then no over-range edges are ever
/// detected, the budget never trips, and Stage 4 is a safe no-op (the
/// tail-end recheck still runs in Stage 5).
struct ArchBranchInfo {
  uint64_t Range;     ///< max |target - branch| in bytes (0 = no range check)
  uint64_t ThunkSize; ///< bytes per range-extension thunk for this arch
};
ArchBranchInfo getArchBranchInfo(uint16_t emachine) {
  switch (emachine) {
  case ELF::EM_AARCH64:
    // B/BL (JUMP26/CALL26) ±128 MiB; lld AArch64ABSLongThunk = 16 B.
    return {0x8000000, 16};
  case ELF::EM_ARM:
    // Thumb-2 B.W/BL ±16 MiB (conservative; ARM A32 B/BL is ±32 MiB). ARM
    // Stage 0 (ELF32LE/REL) is wired (P2-1), so this is live. Thumb long
    // branch thunk (ThumbV7ABSLong / ARMv7ABSLong) = 12 B.
    return {0x1000000, 12};
  case ELF::EM_386:
  case ELF::EM_X86_64:
    // rel32 ±2 GiB — never overflows in practice; jmp thunk = 5 B.
    return {0x80000000, 5};
  default:
    return {0, 0};
  }
}

/// Is `type` a thunkable direct branch/call reloc (B/BL-class)? These are the
/// only relocs lld can extend with a range-extension thunk. Conditional/test
/// branches (AArch64 CONDBR19/TSTBR14) are unthunkable and pinned by
/// markRangeAnchors, so they are deliberately excluded — their endpoints never
/// migrate and need no range check here.
bool isThunkableBranchReloc(uint16_t emachine, uint32_t type) {
  switch (emachine) {
  case ELF::EM_AARCH64:
    return type == ELF::R_AARCH64_JUMP26 || type == ELF::R_AARCH64_CALL26;
  case ELF::EM_ARM:
    // Thunkable B/BL-class branches (lld's ARM::needsThunk extends these):
    // A32 B/BL (R_ARM_JUMP24/CALL, ±32 MiB) and Thumb B.W/BL
    // (R_ARM_THM_JUMP24/THM_CALL, ±16 MiB). Thumb-2 B<cond>.W
    // (R_ARM_THM_JUMP19, ±1 MiB) is also thunkable by lld (a Thumb thunk is
    // created when out of range), so Stage 4 range-checks it. R_ARM_THM_JUMP11
    // (Thumb-1 B<cond> narrow, ±2 KiB) is NOT thunkable by lld and is pinned
    // by markRangeAnchors instead — deliberately excluded here.
    return type == ELF::R_ARM_CALL || type == ELF::R_ARM_JUMP24 ||
           type == ELF::R_ARM_THM_CALL || type == ELF::R_ARM_THM_JUMP24 ||
           type == ELF::R_ARM_THM_JUMP19;
  case ELF::EM_386:
    return type == ELF::R_386_PLT32 || type == ELF::R_386_PC32;
  case ELF::EM_X86_64:
    return type == ELF::R_X86_64_PLT32 || type == ELF::R_X86_64_PC32;
  default:
    return false;
  }
}

/// Projected placement of a node in the global layout. Offsets are relative to
/// an arbitrary layout base; only differences are meaningful.
struct ProjEntry {
  uint64_t StartOff;
  uint64_t EndOff;
};

/// Build a global projected-offset map across all clusters (cluster 0 first,
/// then cluster 1, …), reusing CostFunction::computeProjectedOffsets per
/// cluster with a running base. BBs that have been pinned (reverted to their
/// function position) are absent from the orders and therefore absent from the
/// map — edges involving them are skipped (they land in normal function layout
/// at function scale, where lld's thunk loop handles them as usual).
DenseMap<uint32_t, ProjEntry>
buildGlobalProjected(const XBBRGraph &graph,
                     const std::vector<std::vector<uint32_t>> &clusterBBOrders,
                     unsigned maxAlign) {
  DenseMap<uint32_t, ProjEntry> proj;
  uint64_t base = 0;
  for (const std::vector<uint32_t> &order : clusterBBOrders) {
    std::vector<ProjectedBB> pbs =
        computeProjectedOffsets(graph, order, maxAlign);
    for (const ProjectedBB &pb : pbs)
      proj[pb.NodeIdx] = {base + pb.Offset, base + pb.Offset + pb.Size};
    if (!pbs.empty())
      base += pbs.back().Offset + pbs.back().Size;
  }
  return proj;
}

/// Over-range analysis result: unique over-range (src, dst) BB pairs and a
/// per-node involvement count used to pick which BB to pin.
struct OverRangeInfo {
  std::vector<std::pair<uint32_t, uint32_t>> Edges;
  std::unordered_map<uint32_t, uint32_t> Involvement; // node → #over-range edges
};

/// Walk every BB's per-section relocations; for each thunkable direct branch
/// whose projected src→dst distance exceeds `range`, record the unique
/// (src, dst) pair. Both endpoints must be on the projected map (i.e. both
/// still migrating); edges touching a reverted/pinned BB are skipped. Targets
/// that don't resolve to an XBBR node (external/PLT symbols) are skipped —
/// lld's thunk loop handles those at its own scale, and they carry no BB the
/// budget can pin.
OverRangeInfo collectOverRangeEdges(const XBBRGraph &graph,
                                    const DenseMap<uint32_t, ProjEntry> &proj,
                                    uint16_t emachine, uint64_t range) {
  OverRangeInfo info;
  ArrayRef<XBBRNode> nodes = graph.nodes();

  // per-BB InputSection → node index (one section per BB under =all).
  DenseMap<const InputSectionBase *, uint32_t> secToNode;
  for (uint32_t I = 0; I < nodes.size(); ++I)
    if (nodes[I].BBSection)
      secToNode.try_emplace(nodes[I].BBSection, I);

  std::unordered_set<uint64_t> seen; // dedup (src, dst)
  for (uint32_t I = 0; I < nodes.size(); ++I) {
    InputSectionBase *src = nodes[I].BBSection;
    if (!src)
      continue;
    auto srcIt = proj.find(I);
    if (srcIt == proj.end())
      continue; // src reverted/pinned — off the projected hot layout
    for (const Relocation &r : src->relocs()) {
      if (!isThunkableBranchReloc(emachine, r.type))
        continue;
      auto *d = dyn_cast<Defined>(r.sym);
      auto *tsec =
          d ? dyn_cast_or_null<InputSectionBase>(d->section) : nullptr;
      if (!tsec)
        continue;
      auto dnIt = secToNode.find(tsec);
      if (dnIt == secToNode.end())
        continue; // target not an XBBR node (external/PLT)
      uint32_t dstNode = dnIt->second;
      auto dstIt = proj.find(dstNode);
      if (dstIt == proj.end())
        continue; // dst reverted/pinned
      // Projected branch-site and target VAs (base-relative; diff is exact).
      int64_t branchSite =
          static_cast<int64_t>(srcIt->second.StartOff) + r.offset;
      int64_t target =
          static_cast<int64_t>(dstIt->second.StartOff) + r.addend;
      int64_t diff = target - branchSite;
      uint64_t dist = diff >= 0 ? static_cast<uint64_t>(diff)
                                : static_cast<uint64_t>(-diff);
      if (dist <= range)
        continue;
      uint64_t key = (static_cast<uint64_t>(I) << 32) | dstNode;
      if (!seen.insert(key).second)
        continue;
      info.Edges.push_back({I, dstNode});
      ++info.Involvement[I];
      ++info.Involvement[dstNode];
    }
  }
  return info;
}

/// P1-3 safety margin: a B.cond/TBZ whose projected distance exceeds
/// range×margin is pinned, even though the projection is conservative on
/// alignment. The margin leaves headroom for the B/BL range-extension thunks
/// (and any alignment padding) the projected layout doesn't model — those grow
/// the real distance, and a cond-branch overflow is a hard, unthunkable link
/// error. 0.9 leaves 10% (100 KiB for B.cond, 3.2 KiB for TBZ) — far more than
/// any realistic in-span thunk growth. Conservative pending test-suite
/// validation of a tighter margin.
constexpr double COND_RANGE_SAFETY_MARGIN = 0.9;

/// P1-3: collect the set of CondSafeToMigrate nodes that must be pinned because
/// an AArch64 B.cond (CONDBR19 ±1 MiB) / TBZ (TSTBR14 ±32 KiB) branch they
/// issue is unsafe WITHIN .text.hot. (Cross-section unsafety is handled upfront
/// by CondSafeToMigrate in markRangeAnchors; this only sees all-hot-component
/// BBs that migrated into .text.hot.) Two cases:
///  - within-.text.hot over-range: both endpoints CondSafeToMigrate and still
///    in the projected layout, but their distance exceeds range×margin → pin
///    BOTH (revert to .text, co-located, original in-range distance). Pinning
///    one alone would make the branch cross-section (still unsafe).
///  - partner already pinned (cascade): the target is CondSafeToMigrate but no
///    longer in the layout (pinned this loop) → the branch is now cross-section
///    → pin the source too. Monotonic; converges.
/// `condRange` (0 = real ISA range) is the hidden testing knob.
std::unordered_set<uint32_t>
collectCondPins(const XBBRGraph &graph,
                const DenseMap<uint32_t, ProjEntry> &proj, uint64_t condRange) {
  std::unordered_set<uint32_t> pins;
  ArrayRef<XBBRNode> nodes = graph.nodes();
  DenseMap<const InputSectionBase *, uint32_t> secToNode;
  for (uint32_t I = 0; I < nodes.size(); ++I)
    if (nodes[I].BBSection)
      secToNode.try_emplace(nodes[I].BBSection, I);

  for (uint32_t I = 0; I < nodes.size(); ++I) {
    if (!nodes[I].CondSafeToMigrate)
      continue; // only all-hot-component cond BBs migrate into .text.hot
    InputSectionBase *src = nodes[I].BBSection;
    if (!src)
      continue;
    for (const Relocation &r : src->relocs()) {
      if (r.type != ELF::R_AARCH64_CONDBR19 &&
          r.type != ELF::R_AARCH64_TSTBR14)
        continue;
      auto *d = dyn_cast<Defined>(r.sym);
      auto *tsec =
          d ? dyn_cast_or_null<InputSectionBase>(d->section) : nullptr;
      auto it = tsec ? secToNode.find(tsec) : secToNode.end();
      if (it == secToNode.end())
        continue; // target not an XBBR node
      uint32_t J = it->second;
      auto pX = proj.find(I);
      if (pX == proj.end())
        continue; // I already pinned — handled via its partner
      auto pY = proj.find(J);
      if (nodes[J].CondSafeToMigrate && pY != proj.end()) {
        // Both still in .text.hot → within-section range check.
        int64_t branchSite =
            static_cast<int64_t>(pX->second.StartOff) + r.offset;
        int64_t target =
            static_cast<int64_t>(pY->second.StartOff) + r.addend;
        int64_t diff = target - branchSite;
        uint64_t dist = diff >= 0 ? static_cast<uint64_t>(diff)
                                  : static_cast<uint64_t>(-diff);
        uint64_t r2 = condRange ? condRange
                                : (r.type == ELF::R_AARCH64_TSTBR14 ? 0x8000
                                                                    : 0x100000);
        if (dist > static_cast<uint64_t>(r2 * COND_RANGE_SAFETY_MARGIN)) {
          pins.insert(I);
          pins.insert(J);
        }
      } else {
        // Partner not in .text.hot (pinned this loop, or — defensively — not
        // CondSafeToMigrate): branch is now cross-section → pin the source.
        pins.insert(I);
      }
    }
  }
  return pins;
}

/// Revert a pinned BB's per-BB InputSection name back to `.text.<rest>` so it
/// lands in the `.text` output section at its function slot (co-located with
/// its function's other BBs → original intra-function distance, within cond
/// range) instead of staying in `.text.hot`/`.text.unlikely` at the
/// sortISDBySectionOrder mid-point. Without this, pinning only drops the BB
/// from ClusterBBOrders — its section keeps the `.text.hot.*` name
/// renameSectionsForHotColdSplit gave it (Driver, pre-Writer) and physically
/// remains in `.text.hot`, so a cond branch whose partner was pinned is never
/// co-located and `collectCondPins`'s cross-section pin can't restore the
/// in-range guarantee. Idempotent: a section already named `.text.*` is a
/// no-op (the entry/anchor BBs were never renamed).
void revertBBSectionToText(Ctx &ctx, const XBBRNode &node) {
  InputSectionBase *sec = node.BBSection;
  if (!sec)
    return;
  StringRef nm = sec->name;
  StringRef rest;
  if (nm.starts_with(".text.hot."))
    rest = nm.substr(strlen(".text.hot."));
  else if (nm.starts_with(".text.unlikely."))
    rest = nm.substr(strlen(".text.unlikely."));
  else
    return; // already .text.* (entry/anchor never renamed) or non-text
  sec->name = ctx.saver.save((Twine(".text.") + rest).str());
}

} // namespace

bool runConstraintSolver(Ctx &ctx, XBBRGraph &graph,
                         XBBRLayoutResult &result) {
  ArrayRef<XBBRNode> allNodes = graph.nodes();

  // Count migratable (non-anchor) BBs across all clusters. Anchors are placed
  // by Stage 2 and never drift, so the 30% fallback threshold (SPEC §7 /
  // PLAN §4.3) applies only to truly migratable BBs.
  uint32_t totalMigratable = 0;
  for (const auto &order : result.ClusterBBOrders)
    for (uint32_t n : order)
      if (!allNodes[n].isAnchor())
        ++totalMigratable;

  if (totalMigratable == 0)
    return true;

  const ArchBranchInfo abi = getArchBranchInfo(ctx.arg.emachine);
  // Branch range: the hidden testing knob overrides the ISA range (0 → ISA).
  const uint64_t range = ctx.arg.xbbrBranchRangeForTesting
                             ? ctx.arg.xbbrBranchRangeForTesting
                             : abi.Range;
  const uint64_t thunkSize = abi.ThunkSize;
  const uint64_t budget = ctx.arg.xbbrMaxThunkBytes; // 0 = unlimited
  const bool rangeCheckActive = (range != 0);

  // Pinned BBs: reverted to their original function position, never migrated
  // again. Monotonic growth guarantees convergence (PLAN §4.3 Stage 4).
  std::unordered_set<uint32_t> pinned;
  uint32_t fallbackCount = 0;
  const uint32_t maxIters = totalMigratable + 1; // safety net
  const uint32_t fallbackLimit = std::max(
      static_cast<uint32_t>(GLOBAL_FALLBACK_THRESHOLD * totalMigratable), 1u);

  bool degraded = false;
  uint32_t iter = 0;
  uint32_t lastOverRange = 0;
  uint64_t lastEstThunk = 0;

  // P1-3: AArch64 cond-branch (B.cond/TBZ) range-checking. These branches are
  // unthunkable, so an overflow is a hard link error; Stage 4 must pin any
  // CondSafeToMigrate BB whose B.cond/TBZ becomes over-range within .text.hot
  // (cross-section unsafety is already handled upfront by CondSafeToMigrate).
  const bool condCheckActive = (ctx.arg.emachine == ELF::EM_AARCH64);

  for (iter = 0; iter < maxIters; ++iter) {
    DenseMap<uint32_t, ProjEntry> proj;
    if (rangeCheckActive || condCheckActive)
      proj = buildGlobalProjected(graph, result.ClusterBBOrders,
                                  ctx.arg.xbbrMaxAlign);

    // P1-3: hard-pin cond branches that would overflow within .text.hot (or
    // whose partner was just pinned — cascade). No budget gate: these MUST be
    // pinned (unthunkable). Pinning reverts a BB to its .text function slot.
    if (condCheckActive) {
      std::unordered_set<uint32_t> condPins =
          collectCondPins(graph, proj, ctx.arg.xbbrCondRangeForTesting);
      if (!condPins.empty()) {
        for (uint32_t p : condPins) {
          if (pinned.count(p) || allNodes[p].isAnchor())
            continue;
          pinned.insert(p);
          ++fallbackCount;
          revertBBSectionToText(ctx, allNodes[p]);
          for (auto &order : result.ClusterBBOrders) {
            auto it = std::remove(order.begin(), order.end(), p);
            if (it != order.end())
              order.erase(it, order.end());
          }
        }
        if (fallbackCount > fallbackLimit) {
          degraded = true;
          break;
        }
        continue; // re-project after cond pins
      }
    }

    OverRangeInfo over;
    if (rangeCheckActive)
      over = collectOverRangeEdges(graph, proj, ctx.arg.emachine, range);
    lastOverRange = static_cast<uint32_t>(over.Edges.size());
    lastEstThunk = static_cast<uint64_t>(over.Edges.size()) * thunkSize;

    // Budget gate: unlimited (budget==0) or under budget → converged.
    const bool budgetOK = (budget == 0) || (lastEstThunk <= budget);
    if (budgetOK)
      break;

    // Budget exceeded: pin the migratable node involved in the most over-range
    // edges (greedy). Pinning reverts it to its function slot, removing its
    // over-range edges from the projected hot layout; subsequent BBs shift
    // closer, so the over-range count monotonically decreases.
    //
    // Deterministic tie-break (SPEC §9.3): `over.Involvement` is an
    // unordered_map, so equal-involvement ties are broken by explicit
    // lowest-node-index, never by hash-map iteration order.
    uint32_t best = ~0u;
    uint32_t bestCount = 0;
    for (const auto &kv : over.Involvement) {
      if (pinned.count(kv.first))
        continue;
      if (allNodes[kv.first].isAnchor())
        continue; // anchors don't migrate; nothing to pin
      if (kv.second > bestCount ||
          (kv.second == bestCount && kv.first < best)) {
        bestCount = kv.second;
        best = kv.first;
      }
    }
    if (best == ~0u) {
      // No migratable node is in any over-range edge — every over-range edge
      // is anchor↔anchor. Anchors ARE in ClusterBBOrders (BBLayout places
      // them), so XBBR's cross-function layout can separate two anchored BBs
      // (function entries / musttail) beyond the B/BL range, forcing thunks
      // the budget forbids. Pinning can't help (anchors don't migrate), so
      // degrade to function-level layout, which co-locates each function's
      // BBs and eliminates the XBBR-introduced over-range. Without this,
      // the budget cap is silently downgraded to a P0-2 warning (addresses
      // are final by then; no revert possible) — a SPEC §7 violation.
      degraded = true;
      break;
    }

    // Pin `best`: drop it from its cluster order and revert its section name
    // so it lands in `.text` at its function slot (co-located with its
    // function's BBs → original intra-function distance), not left in
    // `.text.hot` at the mid-point.
    pinned.insert(best);
    ++fallbackCount;
    revertBBSectionToText(ctx, allNodes[best]);
    for (auto &order : result.ClusterBBOrders) {
      auto it = std::remove(order.begin(), order.end(), best);
      if (it != order.end())
        order.erase(it, order.end());
    }

    if (fallbackCount > fallbackLimit) {
      degraded = true;
      break;
    }
  }

  if (iter >= maxIters)
    degraded = true; // safety net; normally unreachable (degrade triggers first)

  result.EstimatedOverRangeEdges = lastOverRange;
  result.EstimatedThunkBytes = lastEstThunk;

  if (degraded) {
    if (ctx.arg.xbbrFallback == XBBRFallback::None) {
      ErrAlways(ctx) << "XBBR: thunk-budget constraints cannot be satisfied ("
                     << fallbackCount << " of " << totalMigratable
                     << " migratable BBs reverted; estThunkBytes="
                     << lastEstThunk << " > budget=" << budget
                     << ") and --bb-cross-reorder-fallback=none is set; aborting.";
      return false;
    }
    if (ctx.arg.xbbrStats || ctx.arg.xbbrFallback != XBBRFallback::Auto)
      Warn(ctx) << "XBBR: " << fallbackCount << " of " << totalMigratable
                << " migratable BBs reverted over the "
                << static_cast<int>(GLOBAL_FALLBACK_THRESHOLD * 100)
                << "% threshold (estThunkBytes=" << lastEstThunk
                << " > budget=" << budget
                << "); degrading to function-level mode.";
    result.Degraded = true;
    result.ClusterBBOrders.clear();
    result.Placements.clear();
  }

  if (ctx.arg.xbbrStats)
    errs() << "xbbr-stage4: overRange=" << lastOverRange
           << " estThunkBytes=" << lastEstThunk
           << " pinned=" << fallbackCount
           << " degraded=" << (degraded ? 1 : 0) << "\n";

  return true;
}

} // namespace lld::elf::xbbr
