//===- XBBRGraph.h - Global XBBR basic-block graph -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// XBBRGraph is the linker-side "stage 0" data structure that aggregates,
// over all input ObjFiles, the BB-level metadata XBBR needs to drive
// hot-cluster reordering (PLAN §4.3 Stage 0 / §8.1).
//
// Inputs (per ObjFile):
//   * SHT_LLVM_BB_ADDR_MAP   — BB IDs/sizes + PGO analyses (FuncEntryCount,
//                              BBFreq, BrProb), produced by the
//                              compiler-side XBBRMetadataEmitter pass.
//   * SHT_LLVM_XBBR_ATTR     — per-BB blacklist bitmask, also from the
//                              compiler-side emitter.
//   * SHT_LLVM_CALL_GRAPH_PROFILE — cross-function call edges (lld already
//                              parses this into ctx.arg.callGraphProfile;
//                              we adopt it directly).
//
// Outputs:
//   * std::vector<XBBRNode> nodes — global enumeration of XBBR-relevant BBs.
//   * std::vector<XBBREdge> edges — CFG (intra-fn) + call (cross-fn) edges.
//   * Two indices: function → first node index; (FuncId, BBId) → node index.
//
// Determinism: `nodes` is ordered by (input file index, section index, BB id)
// and never visited via DenseMap iteration — see PLAN §6. The two indices
// above are convenience lookups, never used as a sort key.
//
// Lifetime: one XBBRGraph per `ld.lld` invocation, owned by Ctx.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_XBBRGRAPH_H
#define LLD_ELF_XBBR_XBBRGRAPH_H

#include "XBBR/XBBRTypes.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace lld::elf {

struct Ctx;
template <class ELFT> class ObjFile;

namespace xbbr {

/// Per-function summary, populated from SHT_LLVM_BB_ADDR_MAP +
/// SHT_LLVM_XBBR_ATTR + (optionally) IRPGO entry count. One per
/// FuncId in the global graph.
struct FuncInfo {
  InputSectionBase *Section = nullptr;  ///< lld object for this function
  uint32_t FirstNode = 0;       ///< first index in XBBRGraph::nodes
  uint32_t NumNodes = 0;        ///< node count for this function
  uint64_t EntryCount = 0;      ///< from BBAddrMap PGO (FuncEntryCount)
  bool HasProfile = false;      ///< true if FuncEntryCount was present
  /// EH gate (Phase 1b). True if migrating this function's BBs would break
  /// exception dispatch / unwind. Under -fbasic-block-sections=all the
  /// compiler emits one FDE per BB section, so plain unwind FDEs follow
  /// migrations (PC-begin relocations resolve to the moved BB) and need no
  /// rewriting. The unsafe case is LSDA (`.gcc_except_table`) functions:
  /// their call_site ranges are byte offsets relative to the function that
  /// stop mapping to the right BB once non-landing-pad BBs drift. Landing
  /// pads are already anchored individually, but the whole LSDA function is
  /// gated so its call_site table stays valid. Set when the function has a
  /// `.gcc_except_table.<fn>` section or any landing-pad BB.
  bool IsEHGated = false;
  /// True if any BB of this function issues an unthunkable conditional branch
  /// (AArch64 B.cond/TBZ; ARM Thumb-1 B<cond>/B narrow). Such branches cannot
  /// be range-extended by lld, so their source+target must stay co-located
  /// (same output section) or the branch overflows. Detected from raw reloc
  /// TYPES at graph-build time (collectFromFile), because section relocs are
  /// not yet parsed when renameSectionsForHotColdSplit runs in the Driver —
  /// the rename uses this to keep the whole function out of .text.hot so the
  /// cond pair is never split across output sections.
  bool HasCondBranch = false;

  llvm::ArrayRef<XBBRNode> nodes(const std::vector<XBBRNode> &all) const {
    return llvm::ArrayRef<XBBRNode>(&all[FirstNode], NumNodes);
  }
};

class XBBRGraph {
public:
  XBBRGraph() = default;
  XBBRGraph(const XBBRGraph &) = delete;
  XBBRGraph &operator=(const XBBRGraph &) = delete;

  /// Stage 0 entry point — populate the graph from ctx's input ObjFiles
  /// and ctx.arg.callGraphProfile. After build() the graph is read-only
  /// for downstream stages.
  ///
  /// Returns false if a fatal inconsistency was diagnosed (e.g. a
  /// `.llvm_xbbr_attr` whose num_bbs disagrees with BB_ADDR_MAP — these
  /// must round-trip; the compiler-side emitter writes both from the
  /// same MIR pass).
  bool build(Ctx &ctx);

  /// Read-only views — downstream stages never mutate the graph.
  llvm::ArrayRef<XBBRNode> nodes() const { return Nodes; }
  llvm::ArrayRef<XBBREdge> edges() const { return Edges; }
  llvm::ArrayRef<FuncInfo> funcs() const { return Funcs; }

  /// Bridge between FuncId and the underlying lld object. `funcSection`
  /// is the inverse of `sectionToFunc`. Returns InvalidFuncId / nullptr
  /// if no mapping exists (e.g. function compiled without
  /// `-fbb-cross-reorder=`). `sectionToFunc` accepts a const pointer
  /// because it is purely a lookup — it never mutates the section,
  /// and accepting const lets callers pass the const-qualified
  /// pointers ctx.arg.callGraphProfile keys carry without const_cast.
  FuncId sectionToFunc(const InputSectionBase *S) const;
  InputSectionBase *funcSection(FuncId F) const;

  /// Look up the global node index for (Func, BB), or nullopt if the BB
  /// was not in any input's BB_ADDR_MAP.
  std::optional<uint32_t> findNode(FuncId Func, BBId BB) const;

  /// Number of nodes flagged isAnchor() — useful for stats / cost-model
  /// sanity checks (anchors set the lower bound on cluster fragmentation).
  uint32_t numAnchors() const;

private:
  std::vector<XBBRNode> Nodes;
  std::vector<XBBREdge> Edges;
  std::vector<FuncInfo> Funcs;       ///< indexed by FuncId

  // Indices (DenseMap is fine — never iterated for output).
  // Key is `const InputSectionBase *` because lookups (and
  // ctx.arg.callGraphProfile keys) are const; XBBRGraph never mutates
  // the section through this map.
  llvm::DenseMap<const InputSectionBase *, FuncId> SectionToFuncId;
  llvm::DenseMap<BBKey, uint32_t> BBIndex;            ///< (FuncId, BBId) → Nodes idx

  // Stage 0 internals — declared here so unit tests in a follow-up
  // patch can drive them piecewise without going through build().
  bool collectFromObjFiles(Ctx &ctx);
  bool collectCallGraphEdges(Ctx &ctx);
  bool runConsistencyChecks(Ctx &ctx) const;
  /// Per-ObjFile Stage 0 body, templated on ELFT so the same logic serves
  /// ELF64LE (x86_64, AArch64, RELA) and ELF32LE (ARM/Thumb, REL). Defined in
  /// XBBRGraph.cpp; collectFromObjFiles dispatches on the file's ELF class.
  template <class ELFT> bool collectFromFile(Ctx &ctx, ObjFile<ELFT> *OF);
  /// Phase 1a: on AArch64, mark BBs that are the source or target of a
  /// conditional/test branch (R_AARCH64_CONDBR19/TSTBR14) as CondInvolved so
  /// Stage 2/4 pin them — those relocs can't be thunked, so migrating either
  /// endpoint risks a hard overflow. No-op on other arches.
  void markRangeAnchors(Ctx &ctx);
};

} // namespace xbbr
} // namespace lld::elf

#endif // LLD_ELF_XBBR_XBBRGRAPH_H
