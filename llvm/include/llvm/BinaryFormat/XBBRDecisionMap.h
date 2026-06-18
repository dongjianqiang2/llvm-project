//===- XBBRDecisionMap.h - XBBR decision-map binary format ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// On-disk binary format for the `.debug_xbbr_decision` ELF section
// (PLAN §9.4). This header is shared between:
//   * lld   — the writer (lld/ELF/SyntheticSections.cpp + XBBR/SectionEmitter)
//   * tools — llvm-bbreorder-dump (and future BOLT consumers)
//
// Layout discipline:
//   * little-endian fields throughout (matches ELF EI_DATA on supported
//     targets — x86_64 and AArch64 are LE; big-endian targets are out
//     of scope, see SPEC §8.1);
//   * 16-byte header followed by zero or more 32-byte entries;
//   * the on-disk struct order is deliberately *not* a C struct — readers
//     and writers go through `read*le` / `write*le` to stay independent
//     of host alignment and padding rules.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BINARYFORMAT_XBBRDECISIONMAP_H
#define LLVM_BINARYFORMAT_XBBRDECISIONMAP_H

#include <cstdint>

namespace llvm {
namespace XBBRDecisionMap {

/// Magic bytes at offset 0 of the `.debug_xbbr_decision` section.
inline constexpr char kMagic[4] = {'X', 'B', 'B', 'R'};

/// Format version. Bump the major byte (high 16 bits) on incompatible
/// changes; readers must reject mismatched majors. The current format
/// is 1.0 — major 0x0001, minor 0x0000.
inline constexpr uint32_t kVersion = 0x00010000u;

inline constexpr uint32_t kVersionMajor = 0x0001u;

/// Bit positions for the 32-bit `flags` header field.
namespace HeaderFlags {
inline constexpr uint32_t Degraded = 1u << 0; ///< Stage 4 fell back to fn-mode
}                                              // namespace HeaderFlags

/// Bit positions for the 32-bit per-entry `decision_flags` field.
/// (PLAN §9.4: moved | anchored | fallback | thunk.)
namespace EntryFlags {
inline constexpr uint32_t Moved    = 1u << 0;
inline constexpr uint32_t Anchored = 1u << 1;
inline constexpr uint32_t Fallback = 1u << 2;
inline constexpr uint32_t Thunk    = 1u << 3;
} // namespace EntryFlags

/// Header is 16 bytes:
///   offset 0..3   : "XBBR"
///   offset 4..7   : uint32 version (= kVersion)
///   offset 8..11  : uint32 num_entries
///   offset 12..15 : uint32 flags (HeaderFlags)
inline constexpr unsigned kHeaderSize = 16;

/// Each entry is 32 bytes:
///   offset 0..7   : uint64 orig_func_addr
///   offset 8..11  : uint32 bb_index
///   offset 12..19 : uint64 new_address
///   offset 20..23 : uint32 cluster_id
///   offset 24..27 : uint32 decision_flags (EntryFlags)
///   offset 28..31 : uint32 func_id (internal XBBR FuncId)
inline constexpr unsigned kEntrySize = 32;

inline constexpr unsigned kEntryOffOrigFuncAddr = 0;
inline constexpr unsigned kEntryOffBBIndex      = 8;
inline constexpr unsigned kEntryOffNewAddress   = 12;
inline constexpr unsigned kEntryOffClusterId    = 20;
inline constexpr unsigned kEntryOffDecisionFlags = 24;
inline constexpr unsigned kEntryOffFuncId        = 28;

/// Compute total section size for `n` entries, in 64-bit math so callers
/// can defend against pathological `num_entries` from corrupt input.
inline constexpr uint64_t totalSize(uint64_t numEntries) {
  return static_cast<uint64_t>(kHeaderSize) +
         static_cast<uint64_t>(kEntrySize) * numEntries;
}

} // namespace XBBRDecisionMap
} // namespace llvm

#endif // LLVM_BINARYFORMAT_XBBRDECISIONMAP_H
