//===- XBBRTypes.h - XBBR core types for the linker ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Core type aliases and small structs for the XBBR linker pipeline (PLAN
// §4 Stages 0-5). Kept in a thin header so XBBRGraph.h and per-stage
// implementation files don't pull in each other's heavy dependencies.
//
// Stability invariants (PLAN §6, M2-T01):
//   * `FuncId` is a small, dense integer assigned by XBBRGraph at Stage 0,
//     in the deterministic order (input file index, section index). The
//     mapping back to the lld InputSectionBase* lives in XBBRGraph.
//     Rationale for an integer ID instead of using `InputSectionBase *`
//     directly:
//       - tests can spell concrete IDs ({0,1,2,...}); raw pointers vary
//         per run and are useless in FileCheck output;
//       - decision-map serialization (M4 / PLAN §9.4) is straightforward;
//       - DenseMap<FuncId,...> is more compact than DenseMap<ptr,...>.
//     The pointer is still the source of truth — the integer IS the
//     pointer's stable position in the deterministic sort order, so
//     across two ld.lld runs of the same inputs the IDs match.
//   * `BBId` is the basic block ID emitted in the per-function
//     `SHT_LLVM_BB_ADDR_MAP` section (BBEntry::ID; `UniqueBBID::BaseID`).
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_XBBR_XBBRTYPES_H
#define LLD_ELF_XBBR_XBBRTYPES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/Object/ELFTypes.h" // object::BBAddrMap

#include <cstdint>
#include <utility>

namespace lld::elf {

class InputSectionBase;

namespace xbbr {

/// Stable integer identifier for a function within one ld.lld invocation.
/// Assigned by XBBRGraph in (input file index, section index) order so
/// the same inputs always produce the same IDs. Use
/// `XBBRGraph::sectionToFunc(InputSectionBase *)` and
/// `XBBRGraph::funcSection(FuncId)` to bridge to/from lld objects.
using FuncId = uint32_t;

/// Sentinel for "no such function" (e.g. the lookup target was not part
/// of any input ObjFile's BB_ADDR_MAP).
inline constexpr FuncId InvalidFuncId = ~uint32_t{0};

/// Basic-block identifier within a function (matches BBAddrMap BBEntry::ID).
using BBId = uint32_t;

/// (FuncId, BBId) pair, hashable for use as a DenseMap key.
using BBKey = std::pair<FuncId, BBId>;

/// Provenance flags for an XBBRNode field — lets Stage 0 consistency
/// checks tell the user *which* input section disagreed when the
/// `.llvm_xbbr_attr` byte and the `BBEntry::Metadata` bit conflict.
enum class Provenance : uint8_t {
  Unknown = 0,
  BBAddrMap = 1u << 0,    ///< from SHT_LLVM_BB_ADDR_MAP
  XBBRAttr = 1u << 1,     ///< from SHT_LLVM_XBBR_ATTR
  CGProfile = 1u << 2,    ///< from SHT_LLVM_CALL_GRAPH_PROFILE
  Derived = 1u << 3,      ///< computed from the above (e.g. global_freq)
};

inline Provenance operator|(Provenance a, Provenance b) {
  return static_cast<Provenance>(static_cast<uint8_t>(a) |
                                 static_cast<uint8_t>(b));
}
inline bool any(Provenance a, Provenance b) {
  return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

/// One node in the XBBR graph. Lifetime is the duration of one ld.lld
/// link. Heavy fields (successors) live in XBBRGraph::edges, indexed
/// by node index, to keep XBBRNode trivially-copyable in a vector.
struct XBBRNode {
  FuncId Func = InvalidFuncId; ///< owning function (XBBRGraph::funcSection)
  BBId BB = 0;                 ///< MBB id within the function
  uint32_t Size = 0;           ///< BB byte size. M2-T01: from BBAddrMap
                               ///<   (the .o-time size). M5 / §4.5
                               ///<   ConstraintSolver may patch this to
                               ///<   post-relaxation size.
  uint64_t GlobalFreq = 0;     ///< pre-multiplied BBFreq × FuncEntryCount
                               ///<   (PLAN §3.2). Use this directly for
                               ///<   cross-function comparison; the raw
                               ///<   per-entry BBFreq is recoverable as
                               ///<   GlobalFreq / FuncInfo.EntryCount.
  uint8_t XBBRAttrs = 0;       ///< bitmask, see xbbr::AttrBit (LLVM side)

  /// Quick predicates derived from XBBRAttrs (mirroring xbbr::AttrBit
  /// in include/llvm/CodeGen/XBBRMetadata.h — Stage 0 will assert these
  /// stay in sync after parsing the linker side).
  bool isEntry() const { return XBBRAttrs & 0x01; }
  bool isLandingPad() const { return XBBRAttrs & 0x02; }
  bool isIndirectBrTarget() const { return XBBRAttrs & 0x04; }
  bool hasSetjmp() const { return XBBRAttrs & 0x08; }
  bool hasInlineAsmLabel() const { return XBBRAttrs & 0x10; }
  bool isMustTail() const { return XBBRAttrs & 0x20; }
  bool userBlacklisted() const { return XBBRAttrs & 0x40; }
  bool isCold() const { return XBBRAttrs & 0x80; }

  /// PLAN §5.3: an "anchor" BB cannot drift. Entry blocks anchor by
  /// definition; the rest is the §5.3 blacklist.
  bool isAnchor() const {
    return XBBRAttrs & (0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40);
  }
};

/// One edge in the XBBR graph. CFG edges (intra-function, from BBAddrMap
/// `BrProb`) and call edges (cross-function, from CGProfile, including
/// indirect call edges from IRPGO VP — see M1-T04 / PLAN §3.3) live in
/// the same array, distinguished by IsCrossFunc.
struct XBBREdge {
  uint32_t SrcNode = 0;      ///< index into XBBRGraph::nodes
  uint32_t DstNode = 0;
  uint64_t Weight = 0;       ///< absolute frequency estimate
  bool IsFallthrough : 1 = false;  ///< fall-through in the input baseline
  bool IsCrossFunc : 1 = false;    ///< cross-function call edge
  bool IsIndirectCall : 1 = false; ///< from IRPGO IPVK_IndirectCallTarget VP
};

} // namespace xbbr
} // namespace lld::elf

#endif // LLD_ELF_XBBR_XBBRTYPES_H
