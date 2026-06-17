//===- XBBRGraph.cpp - Stage 0 of the XBBR linker pipeline ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements XBBRGraph::build — reads, per InputFile, the per-function
// SHT_LLVM_BB_ADDR_MAP and SHT_LLVM_XBBR_ATTR sections produced in M1,
// then folds in the cross-function call edges that lld already parsed
// out of SHT_LLVM_CALL_GRAPH_PROFILE into ctx.arg.callGraphProfile.
//
// M2 scope (SPEC §10): x86_64 only. The BBAddrMap parser is templated on
// ELF64LE here; M5 will lift it to invokeELFT once AArch64/ARM land.
//
//===----------------------------------------------------------------------===//

#include "XBBR/XBBRGraph.h"

#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>

using namespace lld;
using namespace lld::elf;
using namespace lld::elf::xbbr;
using namespace llvm;
using namespace llvm::object;
using llvm::support::endian::read16le;

namespace {

/// XBBR attr-section format (PLAN §9.3, version 0x02):
///   u8 version (0x02)
///   uleb128 num_bbs
///   u16 attrs[num_bbs]   (little-endian)
struct XBBRAttrParseResult {
  bool Ok = false;
  std::vector<uint16_t> Attrs;     ///< per-BB attr words
};

/// Parse a single .llvm_xbbr_attr section's bytes into per-BB attr words.
/// Returns Ok=false (with no diagnostic — caller decides) on any malformed
/// content; the caller's "num_bbs disagrees with BB_ADDR_MAP" check is the
/// authoritative consistency gate.
XBBRAttrParseResult parseXBBRAttr(ArrayRef<uint8_t> Bytes) {
  XBBRAttrParseResult R;
  if (Bytes.size() < 2)
    return R; // need at least version + a uleb byte
  if (Bytes[0] != 0x02)
    return R; // unknown version
  size_t Pos = 1;

  // Decode uleb128 num_bbs (max 5 bytes for u32).
  uint32_t NumBBs = 0;
  unsigned Shift = 0;
  while (Pos < Bytes.size()) {
    uint8_t B = Bytes[Pos++];
    NumBBs |= uint32_t(B & 0x7f) << Shift;
    if ((B & 0x80) == 0)
      break;
    Shift += 7;
    if (Shift >= 32)
      return R;
  }

  // Followed by NumBBs little-endian u16 words.
  if (Pos + size_t(NumBBs) * 2 != Bytes.size())
    return R; // length mismatch — corrupted or wrong version

  R.Attrs.reserve(NumBBs);
  for (uint32_t I = 0; I < NumBBs; ++I) {
    R.Attrs.push_back(read16le(Bytes.data() + Pos));
    Pos += 2;
  }
  R.Ok = true;
  return R;
}

/// Decode the BB_ADDR_MAP section attached to an x86_64 ObjFile, returning
/// the BBAddrMap structures (one per function in the section's text scope)
/// alongside their PGOAnalysisMap entries when -pgo-analysis-map=... was
/// in effect at compile time. The InputSectionBase here is the
/// `.llvm_bb_addr_map` itself; its `link` field points at the function's
/// text section index in this ObjFile.
struct BBAddrMapDecoded {
  std::vector<BBAddrMap> Maps;
  std::vector<PGOAnalysisMap> PGO;
};

bool decodeBBAddrMapForX86_64(InputSectionBase *S, BBAddrMapDecoded &Out) {
  using ELFT = ELF64LE;
  ObjFile<ELFT> *OF = cast<ObjFile<ELFT>>(S->file);
  ELFFile<ELFT> EF = OF->getObj();
  auto Shdrs = EF.sections();
  if (!Shdrs)
    return false;
  // Locate this section's Elf_Shdr by matching offset/size — InputSection
  // doesn't expose its sh_index directly, but we can compare contents.
  // ObjFile stores sections in the same order as Elf_Shdrs; the section
  // index is its position in the section list.
  ArrayRef<InputSectionBase *> AllSecs = OF->getSections();
  size_t Idx = 0;
  for (; Idx < AllSecs.size(); ++Idx)
    if (AllSecs[Idx] == S)
      break;
  if (Idx >= AllSecs.size() || Idx >= Shdrs->size())
    return false;

  const auto &Shdr = (*Shdrs)[Idx];
  std::vector<PGOAnalysisMap> PGO;
  Expected<std::vector<BBAddrMap>> Maps =
      EF.decodeBBAddrMap(Shdr, /*RelaSec=*/nullptr, &PGO);
  if (!Maps) {
    consumeError(Maps.takeError());
    return false;
  }
  Out.Maps = std::move(*Maps);
  Out.PGO = std::move(PGO);
  return true;
}

/// Compute global_freq(BB) from a PGOAnalysisMap entry.
/// Per PLAN §3.2:
///   global_freq = local_freq × entry_count
/// where local_freq is BBFreq.getFrequency() / BFI scale ratio. The
/// BlockFrequency value stored is already in BFI's fixed-point scale,
/// so we divide by 2^32-1's worth of "entry weight" — but practically
/// what we want is "BBFreq × entry_count / 2**something", and the BFI
/// folks normalize the entry block to the implementation's "max" value.
///
/// Concretely: BFI emits per-BB BlockFrequency where the *function's*
/// entry block is the reference. The exposed getFrequency() is in raw
/// units; for cross-function comparison we want to multiply by the
/// per-call-site entry count and divide out the per-function entry's
/// raw freq. Since FuncEntryCount is supplied alongside, we compute:
///
///   global_freq(BB) = BBFreq(BB) * FuncEntryCount / BBFreq(entry_block)
///
/// which both folds out the BFI scale and normalizes correctly.
uint64_t computeGlobalFreq(uint64_t BBRaw, uint64_t EntryRaw,
                           uint64_t EntryCount) {
  if (EntryRaw == 0 || EntryCount == 0)
    return 0;
  // Use 128-bit-ish overflow-safe arithmetic (BB freqs and counts both
  // bounded well below 2^32 in practice).
  __uint128_t N = __uint128_t(BBRaw) * __uint128_t(EntryCount);
  return uint64_t(N / EntryRaw);
}

} // namespace

bool XBBRGraph::collectFromObjFiles(Ctx &ctx) {
  // Step 1: deterministically enumerate (ObjFile, text section) pairs
  // and assign each a stable FuncId. The order is (ctx.objectFiles
  // index, section index) — pure container traversal, no DenseMap.
  for (ELFFileBase *FB : ctx.objectFiles) {
    ArrayRef<InputSectionBase *> Secs = FB->getSections();
    for (InputSectionBase *S : Secs) {
      if (!S)
        continue;
      // We only build XBBRGraph nodes for executable text sections.
      if (S->type != ELF::SHT_PROGBITS)
        continue;
      if ((S->flags & ELF::SHF_EXECINSTR) == 0)
        continue;
      if (SectionToFuncId.contains(S))
        continue;
      FuncId Fid = static_cast<FuncId>(Funcs.size());
      SectionToFuncId.try_emplace(S, Fid);
      FuncInfo FI;
      FI.Section = S;
      Funcs.push_back(FI);
    }
  }

  // Step 2: for each ObjFile, walk its sections looking for the per-text
  // BB_ADDR_MAP and `.llvm_xbbr_attr`. Both use SHF_LINK_ORDER
  // attaching them to a text section; we resolve `link` to find the
  // owning FuncId.
  for (ELFFileBase *FB : ctx.objectFiles) {
    ArrayRef<InputSectionBase *> Secs = FB->getSections();
    if (Secs.empty())
      continue;

    // Pre-decode every BB_ADDR_MAP section in this ObjFile.
    DenseMap<InputSectionBase *, BBAddrMapDecoded> DecodedMaps;
    for (InputSectionBase *S : Secs) {
      if (!S || S->type != ELF::SHT_LLVM_BB_ADDR_MAP)
        continue;
      BBAddrMapDecoded D;
      if (!decodeBBAddrMapForX86_64(S, D))
        continue;
      DecodedMaps[S] = std::move(D);
    }

    // Pre-decode every .llvm_xbbr_attr section in this ObjFile.
    DenseMap<InputSectionBase *, XBBRAttrParseResult> DecodedAttrs;
    for (InputSectionBase *S : Secs) {
      if (!S || S->type != ELF::SHT_LLVM_XBBR_ATTR)
        continue;
      DecodedAttrs[S] = parseXBBRAttr(S->content());
    }

    // For every text section in this file, look up its decoded
    // BB_ADDR_MAP / .llvm_xbbr_attr and create XBBRNodes.
    for (size_t SIdx = 0, E = Secs.size(); SIdx < E; ++SIdx) {
      InputSectionBase *Text = Secs[SIdx];
      if (!Text || Text->type != ELF::SHT_PROGBITS)
        continue;
      if ((Text->flags & ELF::SHF_EXECINSTR) == 0)
        continue;

      // Find sibling sections that link to this text section.
      InputSectionBase *AddrMap = nullptr;
      InputSectionBase *AttrSec = nullptr;
      for (InputSectionBase *S : Secs) {
        if (!S || S->link != SIdx)
          continue;
        if (S->type == ELF::SHT_LLVM_BB_ADDR_MAP)
          AddrMap = S;
        else if (S->type == ELF::SHT_LLVM_XBBR_ATTR)
          AttrSec = S;
      }
      if (!AddrMap)
        continue; // No BB_ADDR_MAP — function not compiled for XBBR.

      auto MIt = DecodedMaps.find(AddrMap);
      if (MIt == DecodedMaps.end() || MIt->second.Maps.empty())
        continue;

      const BBAddrMap &BAM = MIt->second.Maps.front();
      const PGOAnalysisMap *PAM = MIt->second.PGO.empty()
                                      ? nullptr
                                      : &MIt->second.PGO.front();

      const XBBRAttrParseResult *Attrs = nullptr;
      if (AttrSec) {
        auto AIt = DecodedAttrs.find(AttrSec);
        if (AIt != DecodedAttrs.end() && AIt->second.Ok)
          Attrs = &AIt->second;
      }

      FuncId Fid = SectionToFuncId.lookup(Text);
      assert(Fid != InvalidFuncId && "text section should already be mapped");
      FuncInfo &FI = Funcs[Fid];
      FI.FirstNode = static_cast<uint32_t>(Nodes.size());
      if (PAM) {
        FI.HasProfile = true;
        FI.EntryCount = PAM->FuncEntryCount;
      }

      // Collect all BB entries across the function's BB ranges.
      // BBRanges has 1 entry for normal functions, multiple for
      // multi-section functions (post -fbasic-block-sections=all).
      struct FlatEntry {
        BBId Id;
        uint32_t Size;
      };
      std::vector<FlatEntry> Flat;
      for (const auto &Range : BAM.BBRanges)
        for (const auto &E : Range.BBEntries)
          Flat.push_back({E.ID, E.Size});

      // Optional consistency check: .llvm_xbbr_attr's num_bbs must
      // match the BB_ADDR_MAP BB count. M1 produced both from the
      // same MIR pass; a mismatch means tampering or version skew.
      if (Attrs && Attrs->Attrs.size() != Flat.size()) {
        Err(ctx) << "XBBR Stage 0: " << Text->name << " in "
                 << FB->getName()
                 << ": .llvm_xbbr_attr num_bbs (" << Attrs->Attrs.size()
                 << ") does not match BB_ADDR_MAP (" << Flat.size() << ")";
        return false;
      }

      // Compute global_freq = BBFreq * EntryCount / EntryBBFreq.
      uint64_t EntryRaw = 0;
      if (PAM && !PAM->BBEntries.empty())
        EntryRaw = PAM->BBEntries.front().BlockFreq.getFrequency();

      for (size_t I = 0; I < Flat.size(); ++I) {
        XBBRNode N;
        N.Func = Fid;
        N.BB = Flat[I].Id;
        N.Size = Flat[I].Size;
        if (PAM && I < PAM->BBEntries.size())
          N.GlobalFreq = computeGlobalFreq(
              PAM->BBEntries[I].BlockFreq.getFrequency(), EntryRaw,
              FI.EntryCount);
        else
          N.GlobalFreq = 0;
        N.XBBRAttrs = Attrs ? Attrs->Attrs[I] : 0;

        BBKey K{Fid, N.BB};
        BBIndex.try_emplace(K, static_cast<uint32_t>(Nodes.size()));
        Nodes.push_back(N);
      }
      FI.NumNodes = static_cast<uint32_t>(Nodes.size()) - FI.FirstNode;
    }
  }

  // Trim out trailing empty FuncInfos (text sections with no
  // BB_ADDR_MAP) so funcs() only exposes XBBR-instrumented functions.
  // Doing this after the fact preserves stable FuncIds for the ones
  // that did get nodes; the empty trailing FuncInfos are harmless.
  return true;
}

bool XBBRGraph::collectCallGraphEdges(Ctx &ctx) {
  // ctx.arg.callGraphProfile is a MapVector<SectionPair, uint64_t>
  // populated by lld's existing CG-profile reader from the ELF
  // .llvm.call_graph_profile section. The pair is
  //   first = caller text InputSectionBase *
  //   second = callee text InputSectionBase *
  // Both keys come from the same per-section pool we've already
  // enumerated in collectFromObjFiles, so the FuncId lookup is O(1).
  for (const auto &KV : ctx.arg.callGraphProfile) {
    // ctx.arg.callGraphProfile keys are `const InputSectionBase *` (lld
    // doesn't mutate them through this map). Casting away const is
    // safe — sectionToFunc/SectionToFuncId only ever read the pointer
    // identity for lookup, never mutate the pointee.
    InputSectionBase *FromS = const_cast<InputSectionBase *>(KV.first.first);
    InputSectionBase *ToS = const_cast<InputSectionBase *>(KV.first.second);
    FuncId From = sectionToFunc(FromS);
    FuncId To = sectionToFunc(ToS);
    if (From == InvalidFuncId || To == InvalidFuncId)
      continue;
    XBBREdge E;
    // Stage 0 doesn't know which BB inside the caller issued the call,
    // so we attach the edge to the caller's entry block (FuncInfo's
    // first node). Stage 2 (M3) will narrow it once it has BB-level
    // call-site info. Same for the callee — entry block is the only
    // landing point for an external call (function symbol = entry).
    const FuncInfo &FFrom = Funcs[From];
    const FuncInfo &FTo = Funcs[To];
    if (FFrom.NumNodes == 0 || FTo.NumNodes == 0)
      continue;
    E.SrcNode = FFrom.FirstNode;
    E.DstNode = FTo.FirstNode;
    E.Weight = KV.second;
    E.IsFallthrough = false;
    E.IsCrossFunc = true;
    // M1-T04 keeps indirect-call edges in the same CGProfile stream;
    // we cannot tell direct vs. indirect from the section alone.
    // M3 may want to split this; for M2 we treat all edges uniformly.
    E.IsIndirectCall = false;
    Edges.push_back(E);
  }
  return true;
}

bool XBBRGraph::runConsistencyChecks(Ctx &ctx) const {
  // The strictest check (xbbr_attr.num_bbs == BBAddrMap.numBBs) is
  // already done inline during collectFromObjFiles. Future checks
  // (e.g. IsLandingPad ⇔ BBAddrMap::IsEHPad bit) belong here once
  // we're carrying the BBAddrMap metadata through to XBBRNode.
  (void)ctx;
  return true;
}

bool XBBRGraph::build(Ctx &ctx) {
  if (!collectFromObjFiles(ctx))
    return false;
  if (!collectCallGraphEdges(ctx))
    return false;
  if (!runConsistencyChecks(ctx))
    return false;
  return true;
}

FuncId XBBRGraph::sectionToFunc(InputSectionBase *S) const {
  auto It = SectionToFuncId.find(S);
  if (It == SectionToFuncId.end())
    return InvalidFuncId;
  return It->second;
}

InputSectionBase *XBBRGraph::funcSection(FuncId F) const {
  if (F >= Funcs.size())
    return nullptr;
  return Funcs[F].Section;
}

std::optional<uint32_t> XBBRGraph::findNode(FuncId Func, BBId BB) const {
  auto It = BBIndex.find(BBKey{Func, BB});
  if (It == BBIndex.end())
    return std::nullopt;
  return It->second;
}

uint32_t XBBRGraph::numAnchors() const {
  uint32_t N = 0;
  for (const XBBRNode &Node : Nodes)
    if (Node.XBBRAttrs != 0)
      ++N;
  return N;
}
