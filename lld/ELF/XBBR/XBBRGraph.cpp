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
// Supported architectures: x86_64 and AArch64 (both ELF64LE). ARM (ELF32LE)
// support lands after M5 thunk integration.
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

/// XBBR attr-section format (PLAN §9.3, version 0x02).
///
/// One section may concatenate the attr blocks for multiple functions
/// when they share the same text section (typical without
/// -ffunction-sections). Each block is:
///   u8 version (0x02)
///   uleb128 num_bbs
///   u16 attrs[num_bbs]   (little-endian)
struct XBBRAttrParseResult {
  bool Ok = false;
  /// Each std::vector<uint16_t> is one function's per-BB attr words,
  /// in the order the functions appear in the .o.
  std::vector<std::vector<uint16_t>> PerFunc;
};

/// Parse a single .llvm_xbbr_attr section's bytes. Returns Ok=false
/// (with no diagnostic — caller decides) on any malformed content.
XBBRAttrParseResult parseXBBRAttr(ArrayRef<uint8_t> Bytes) {
  XBBRAttrParseResult R;
  size_t Pos = 0;
  while (Pos < Bytes.size()) {
    if (Bytes[Pos] != 0x02)
      return R; // unknown version
    ++Pos;
    if (Pos >= Bytes.size())
      return R;
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
    if (Pos + size_t(NumBBs) * 2 > Bytes.size())
      return R; // length truncated
    std::vector<uint16_t> Func;
    Func.reserve(NumBBs);
    for (uint32_t I = 0; I < NumBBs; ++I) {
      Func.push_back(read16le(Bytes.data() + Pos));
      Pos += 2;
    }
    R.PerFunc.push_back(std::move(Func));
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
  // M2: linear scan to find this section's index. With N functions
  // per ObjFile this gives O(N²) total cost in collectFromObjFiles.
  // M3 TODO: cache a DenseMap<InputSectionBase*, size_t> per ObjFile
  // to amortize. M2 only does function-level reordering, so the cost
  // stays well below noise on realistic inputs.
  ArrayRef<InputSectionBase *> AllSecs = OF->getSections();
  size_t Idx = 0;
  for (; Idx < AllSecs.size(); ++Idx)
    if (AllSecs[Idx] == S)
      break;
  if (Idx >= AllSecs.size() || Idx >= Shdrs->size())
    return false;

  const auto &Shdr = (*Shdrs)[Idx];

  // Relocatable .o files reference the function symbol through a
  // companion SHT_RELA section whose `sh_info` points back at the
  // BB_ADDR_MAP. decodeBBAddrMap needs that reloc data — without it
  // it fails with "failed to get relocation data for offset N" on
  // the function-address field.
  const typename ELFT::Shdr *RelaShdr = nullptr;
  for (const auto &Sh : *Shdrs) {
    if (Sh.sh_type == ELF::SHT_RELA && Sh.sh_info == Idx) {
      RelaShdr = &Sh;
      break;
    }
  }

  std::vector<PGOAnalysisMap> PGO;
  Expected<std::vector<BBAddrMap>> Maps =
      EF.decodeBBAddrMap(Shdr, RelaShdr, &PGO);
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

    // Pre-decode every .llvm_xbbr_attr section attached to this
    // ObjFile via SHF_EXCLUDE. The sections are NOT in
    // FB->getSections() (they were marked discarded in InputFiles —
    // SHF_EXCLUDE means they don't enter the output), so we have to
    // fetch them from the raw Elf_Shdr table using the indices that
    // ObjFile recorded for us. Map: sh_link → parsed attrs.
    DenseMap<uint32_t, XBBRAttrParseResult> AttrsByLink;
    {
      ObjFile<ELF64LE> *OF = cast<ObjFile<ELF64LE>>(FB);
      auto Shdrs = OF->getObj().sections();
      if (!Shdrs) {
        consumeError(Shdrs.takeError());
      } else {
        for (uint32_t Idx : OF->xbbrAttrSectionIndices) {
          if (Idx >= Shdrs->size())
            continue;
          const auto &Shdr = (*Shdrs)[Idx];
          auto Bytes = OF->getObj().getSectionContents(Shdr);
          if (!Bytes) {
            consumeError(Bytes.takeError());
            continue;
          }
          AttrsByLink[Shdr.sh_link] = parseXBBRAttr(*Bytes);
        }
      }
    }

    // For every text section in this file, look up its decoded
    // BB_ADDR_MAP / .llvm_xbbr_attr and create XBBRNodes.
    for (size_t SIdx = 0, E = Secs.size(); SIdx < E; ++SIdx) {
      InputSectionBase *Text = Secs[SIdx];
      if (!Text || Text->type != ELF::SHT_PROGBITS)
        continue;
      if ((Text->flags & ELF::SHF_EXECINSTR) == 0)
        continue;

      // Find sibling sections that link to this text section. The
      // BB_ADDR_MAP is in `Secs` (it's not SHF_EXCLUDE); the
      // `.llvm_xbbr_attr` was discarded from `Secs` and is fetched
      // from AttrsByLink, keyed on sh_link.
      InputSectionBase *AddrMap = nullptr;
      for (InputSectionBase *S : Secs) {
        if (!S || S->link != SIdx)
          continue;
        if (S->type == ELF::SHT_LLVM_BB_ADDR_MAP)
          AddrMap = S;
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

      const std::vector<uint16_t> *FuncAttrs = nullptr;
      auto AIt = AttrsByLink.find(SIdx);
      if (AIt != AttrsByLink.end() && AIt->second.Ok &&
          !AIt->second.PerFunc.empty()) {
        // M2: assume one function per text section (works under
        // -ffunction-sections, which XBBR effectively requires for
        // BB-level reorder anyway). M3 generalizes to N functions
        // per section by indexing into PerFunc with the BBAddrMap
        // entry's position.
        FuncAttrs = &AIt->second.PerFunc.front();
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
      if (FuncAttrs && FuncAttrs->size() != Flat.size()) {
        Err(ctx) << "XBBR Stage 0: " << Text->name << " in "
                 << FB->getName()
                 << ": .llvm_xbbr_attr num_bbs (" << FuncAttrs->size()
                 << ") does not match BB_ADDR_MAP (" << Flat.size() << ")";
        return false;
      }

      // Compute global_freq = BBFreq * EntryCount / EntryBBFreq.
      uint64_t EntryRaw = 0;
      if (PAM && !PAM->BBEntries.empty())
        EntryRaw = PAM->BBEntries.front().BlockFreq.getFrequency();

      // Walk Flat (BBAddrMap entries flattened across BBRanges) and
      // PAM->BBEntries in lock-step. Both are emitted by AsmPrinter in
      // `for (MBB : MF)` order (see AsmPrinter::emitBBAddrMapSection),
      // so a positional pairing is well-defined regardless of BB IDs —
      // the index `I` is *position*, not numeric BBID. This is also
      // why we don't index into PAM->BBEntries by Flat[I].Id: the
      // pairing is by emission order, not by ID.
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
        N.XBBRAttrs = FuncAttrs ? (*FuncAttrs)[I] : 0;

        BBKey K{Fid, N.BB};
        BBIndex.try_emplace(K, static_cast<uint32_t>(Nodes.size()));
        Nodes.push_back(N);
      }
      FI.NumNodes = static_cast<uint32_t>(Nodes.size()) - FI.FirstNode;

      // Collect intra-function CFG edges from BBAddrMap BrProb data.
      // PGOBBEntry::Successors carries (target-BB-ID, BranchProbability)
      // per source BB. These edges are the primary input to ExtTSP in
      // Stage 2, alongside the cross-function call edges from CGProfile.
      if (PAM) {
        for (size_t I = 0; I < Flat.size(); ++I) {
          if (I >= PAM->BBEntries.size())
            continue;
          const auto &PGOEntry = PAM->BBEntries[I];
          uint32_t SrcIdx = FI.FirstNode + static_cast<uint32_t>(I);
          for (const auto &Succ : PGOEntry.Successors) {
            auto Dst = findNode(Fid, Succ.ID);
            if (!Dst)
              continue;
            XBBREdge E;
            E.SrcNode = SrcIdx;
            E.DstNode = *Dst;
            // Scale the branch probability to an absolute count using the
            // source BB's global frequency. BranchProbability::scale() does
            // Prob/denom * GlobalFreq, yielding an approximate execution
            // count for this edge.
            E.Weight = static_cast<uint64_t>(
                Succ.Prob.scale(uint64_t(Nodes[SrcIdx].GlobalFreq)));
            // Fallthrough iff the successor BB immediately follows the
            // source BB in the original function order (BB IDs are dense
            // and sequential after RenumberBlocks).
            E.IsFallthrough = (Flat[I].Id + 1 == Succ.ID);
            E.IsCrossFunc = false;
            E.IsIndirectCall = false;
            Edges.push_back(E);
          }
        }
      }
    }
  }

  // Funcs may contain entries with NumNodes==0: text sections that
  // weren't compiled with -fbb-cross-reorder= and therefore have no
  // BB_ADDR_MAP. Keeping them around preserves stable FuncIds and is
  // harmless (downstream code defends with NumNodes==0 checks).
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
    // ctx.arg.callGraphProfile keys are `const InputSectionBase *`.
    // sectionToFunc accepts that directly — no const_cast required.
    const InputSectionBase *FromS = KV.first.first;
    const InputSectionBase *ToS = KV.first.second;
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

FuncId XBBRGraph::sectionToFunc(const InputSectionBase *S) const {
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
    // Use isAnchor() so this stays consistent with downstream
    // consumers (Stage 1/2 use isAnchor() for cluster fragmentation
    // bounds). `XBBRAttrs != 0` would over-count IsCold blocks
    // (cold ≠ anchored, see XBBRTypes.h).
    if (Node.isAnchor())
      ++N;
  return N;
}
