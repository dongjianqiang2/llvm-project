//===- XBBRGraph.cpp - Stage 0 of the XBBR linker pipeline ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements XBBRGraph::build — reads, per InputFile, the per-function
// SHT_LLVM_BB_ADDR_MAP and SHT_LLVM_XBBR_ATTR sections produced by the
// compiler-side XBBRMetadataEmitter pass, then folds in the
// cross-function call edges that lld already parsed out of
// SHT_LLVM_CALL_GRAPH_PROFILE into ctx.arg.callGraphProfile.
//
// Supported architectures: x86_64 and AArch64 (ELF64LE, RELA) and ARM/Thumb
// (ELF32LE, REL). ELF32LE dispatch and the SHT_REL BBAddrMap decode path are
// wired (P2-1); the ARM EH gate (PLAN §5.3/§5.4) keeps ARM functions at
// function-level reordering until .ARM.exidx multi-segment lands.
//
//===----------------------------------------------------------------------===//

#include "XBBR/XBBRGraph.h"

#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "Relocations.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <utility>

using namespace lld;
using namespace lld::elf;
using namespace lld::elf::xbbr;
using namespace llvm;
using namespace llvm::object;
using llvm::support::endian::read16le;

namespace {

/// Unthunkable short-range conditional-branch relocation types. lld cannot
/// range-extend these (only B/BL-class branches are thunkable), so a BB that
/// issues one — and its target — must stay co-located or the branch overflows
/// at link time. Shared by markRangeAnchors (cond-edge partnership) and the
/// per-function HasCondBranch detection (used by renameSectionsForHotColdSplit
/// to keep cond-branch functions out of .text.hot).
static bool isUnthunkableCondRelocType(uint16_t emachine, uint32_t type) {
  switch (emachine) {
  case ELF::EM_AARCH64:
    return type == ELF::R_AARCH64_CONDBR19 || type == ELF::R_AARCH64_TSTBR14;
  case ELF::EM_ARM:
    return type == ELF::R_ARM_THM_JUMP11 || type == ELF::R_ARM_THM_JUMP8;
  default:
    return false;
  }
}

template <class RelTy>
static bool relocRangeHasCondBranch(Relocs<RelTy> rels, uint16_t emachine) {
  for (const RelTy &rel : rels)
    if (isUnthunkableCondRelocType(emachine, rel.getType(false)))
      return true;
  return false;
}

/// True if `Sec` has any unthunkable conditional-branch relocation, read from
/// the raw ELF reloc data (relsOrRelas). Used at graph-build time, when the
/// section's scanned `relocations` vector is still empty (scanRelocations runs
/// later, in the Writer).
template <class ELFT>
static bool sectionHasCondBranch(InputSectionBase *Sec, uint16_t emachine) {
  if (!Sec || Sec->relSecIdx == 0)
    return false;
  const RelsOrRelas<ELFT> rels = Sec->template relsOrRelas<ELFT>();
  if (rels.areRelocsCrel())
    return relocRangeHasCondBranch(rels.crels, emachine);
  if (rels.areRelocsRel())
    return relocRangeHasCondBranch(rels.rels, emachine);
  return relocRangeHasCondBranch(rels.relas, emachine);
}

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
  /// Per-range InputSection, parallel to Maps.front().BBRanges. Under
  /// -fbasic-block-sections=all each range is one BB's section; resolved from
  /// the BBAddrMap BaseAddress relocations (decodeBBAddrMap drops the symbol
  /// and keeps only the addend, so we re-read the relocations here). Empty if
  /// the section has no relocation data.
  std::vector<InputSectionBase *> RangeSections;
};

/// Decode one SHT_LLVM_BB_ADDR_MAP section. Templated on ELFT so the same
/// logic serves ELF64LE (x86_64, AArch64, RELA) and ELF32LE (ARM/Thumb, REL).
/// The companion relocation section may be SHT_RELA (ELF64) or SHT_REL (ARM);
/// both are handled. Returns false on non-matching ELF class or parse error.
template <class ELFT>
bool decodeBBAddrMap(InputSectionBase *S, BBAddrMapDecoded &Out) {
  ObjFile<ELFT> *OF = dyn_cast<ObjFile<ELFT>>(S->file);
  if (!OF)
    return false;
  ELFFile<ELFT> EF = OF->getObj();
  auto Shdrs = EF.sections();
  if (!Shdrs)
    return false;
  // Linear scan to find this section's index. With N functions per
  // ObjFile this gives O(N²) total cost in collectFromObjFiles. A
  // DenseMap<InputSectionBase*, size_t> cache would amortize, but at
  // current scale the cost stays well below noise on realistic inputs.
  ArrayRef<InputSectionBase *> AllSecs = OF->getSections();
  size_t Idx = 0;
  for (; Idx < AllSecs.size(); ++Idx)
    if (AllSecs[Idx] == S)
      break;
  if (Idx >= AllSecs.size() || Idx >= Shdrs->size())
    return false;

  const auto &Shdr = (*Shdrs)[Idx];

  // Relocatable .o files reference the function symbol through a companion
  // relocation section whose `sh_info` points back at the BB_ADDR_MAP.
  // ELF64LE uses SHT_RELA; ELF32LE ARM uses SHT_REL (addend in the insn).
  // decodeBBAddrMap needs that reloc data — without it it fails with
  // "failed to get relocation data for offset N" on the function-address
  // field.
  const typename ELFT::Shdr *RelShdr = nullptr;
  for (const auto &Sh : *Shdrs) {
    if ((Sh.sh_type == ELF::SHT_RELA || Sh.sh_type == ELF::SHT_REL) &&
        Sh.sh_info == Idx) {
      RelShdr = &Sh;
      break;
    }
  }

  std::vector<PGOAnalysisMap> PGO;
  Expected<std::vector<BBAddrMap>> Maps =
      EF.decodeBBAddrMap(Shdr, RelShdr, &PGO);
  if (!Maps) {
    consumeError(Maps.takeError());
    return false;
  }
  Out.Maps = std::move(*Maps);
  Out.PGO = std::move(PGO);

  // Resolve each BB range's BaseAddress relocation to its per-BB
  // InputSection. Under -fbasic-block-sections=all, one BBAddrMap section
  // carries N ranges; each range's BaseAddress is a relocation against that
  // BB's section symbol. EF.decodeBBAddrMap resolves the BaseAddress to an
  // addend (discarding the symbol), so we re-read the reloc section here,
  // sort by r_offset (ranges are emitted in order), and pair positionally.
  // This is the per-BB InputSection association that the offset-based code
  // formerly lacked. REL (ARM) and RELA (ELF64) both carry r_offset.
  if (RelShdr) {
    std::vector<std::pair<uint64_t, InputSectionBase *>> ByOff;
    if (RelShdr->sh_type == ELF::SHT_RELA) {
      auto Rels = EF.relas(*RelShdr);
      if (!Rels) {
        consumeError(Rels.takeError());
        return true;
      }
      ByOff.reserve(Rels->size());
      for (const typename ELFT::Rela &R : *Rels) {
        Symbol &sym = OF->getRelocTargetSym(R);
        auto *d = dyn_cast<Defined>(&sym);
        auto *sec =
            d ? dyn_cast_or_null<InputSectionBase>(d->section) : nullptr;
        ByOff.emplace_back(R.r_offset, sec);
      }
    } else {
      auto Rels = EF.rels(*RelShdr);
      if (!Rels) {
        consumeError(Rels.takeError());
        return true;
      }
      ByOff.reserve(Rels->size());
      for (const typename ELFT::Rel &R : *Rels) {
        Symbol &sym = OF->getRelocTargetSym(R);
        auto *d = dyn_cast<Defined>(&sym);
        auto *sec =
            d ? dyn_cast_or_null<InputSectionBase>(d->section) : nullptr;
        ByOff.emplace_back(R.r_offset, sec);
      }
    }
    llvm::sort(ByOff, llvm::less_first());
    for (auto &KV : ByOff)
      Out.RangeSections.push_back(KV.second);
  }
  return true;
}

// Instantiate for both ELF classes we support (x86_64/AArch64 = ELF64LE,
// ARM/Thumb = ELF32LE). Explicit instantiations let collectFromObjFiles
// dispatch on the file's ELF class without exposing the template in the header.
template bool decodeBBAddrMap<ELF64LE>(InputSectionBase *, BBAddrMapDecoded &);
template bool decodeBBAddrMap<ELF32LE>(InputSectionBase *, BBAddrMapDecoded &);

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
  // Dispatch each input object by ELF class via `ekind` (the lld idiom), NOT
  // dyn_cast<ObjFile<ELFx>>: ObjFile<ELFT> inherits ELFFileBase::classof,
  // which returns true for *any* ELF file, so an unguarded
  // dyn_cast<ObjFile<ELF64LE>> would also match an ELF32 ObjFile and misparse
  // its 32-bit header as Elf64 — corrupting every field past the magic and
  // silently yielding zero nodes on ARM. ekind is set at ObjFile construction
  // (InputFiles.cpp) from the object's actual ELF class, so it is the reliable
  // discriminator. ELF64LE (x86_64, AArch64, RELA) and ELF32LE (ARM/Thumb,
  // REL) are handled; big-endian classes are skipped. FuncIds are assigned in
  // (objectFiles index, BBAddrMap section index) order — pure container
  // traversal, deterministic (PLAN §6).
  for (ELFFileBase *FB : ctx.objectFiles) {
    switch (FB->ekind) {
    case ELF64LEKind:
      if (!collectFromFile(ctx, cast<ObjFile<ELF64LE>>(FB)))
        return false;
      break;
    case ELF32LEKind:
      if (!collectFromFile(ctx, cast<ObjFile<ELF32LE>>(FB)))
        return false;
      break;
    default:
      break; // ELF32BE / ELF64BE (big-endian) not supported by XBBR.
    }
  }
  return true;
}

// Per-ObjFile Stage 0 body. Under -fbasic-block-sections=all (implied by
// -fbb-cross-reorder=partial|full, Phase 0a), each BB is its own InputSection
// and one SHT_LLVM_BB_ADDR_MAP section (with N ranges) describes a whole
// function == one FuncId. decodeBBAddrMap resolves each range's BaseAddress
// relocation to its per-BB InputSection (RangeSections).
template <class ELFT>
bool XBBRGraph::collectFromFile(Ctx &ctx, ObjFile<ELFT> *OF) {
  ArrayRef<InputSectionBase *> Secs = OF->getSections();
  if (Secs.empty())
    return true;
  ELFFile<ELFT> EF = OF->getObj();
  auto Shdrs = EF.sections();
  if (!Shdrs) {
    consumeError(Shdrs.takeError());
    return true;
  }

    // Pre-decode every BBAddrMap section in this ObjFile (with RangeSections).
    std::vector<BBAddrMapDecoded> DecodedMaps;
    for (InputSectionBase *S : Secs) {
      if (!S || S->type != ELF::SHT_LLVM_BB_ADDR_MAP)
        continue;
      BBAddrMapDecoded D;
      if (!decodeBBAddrMap<ELFT>(S, D) || D.Maps.empty())
        continue;
      DecodedMaps.push_back(std::move(D));
    }

    // EH gate (Phase 1b): collect the set of function-name suffixes that own
    // a `.gcc_except_table.<fn>` LSDA section (ELF64 C++) or an ARM
    // `.ARM.exidx.text.<fn>` unwind section. A function with an LSDA cannot
    // have its non-landing-pad BBs migrated — the call_site ranges are byte
    // offsets that stop mapping to the right BB once they drift. ARM exidx is
    // per-function (NOT per-BB-section under =all, unlike ELF64 FDEs), so any
    // ARM function with an exidx must be pinned wholesale or its unwind entry
    // stops covering the migrated BBs. Under =all the ELF64 FDEs are
    // per-BB-section (plain unwind follows migration), but LSDA dispatch and
    // ARM exidx still need the whole function pinned. Match by the suffix
    // shared with `.text.<fn>`.
    llvm::StringSet<> ExceptTableSuffixes;
    llvm::StringSet<> ArmExidxSuffixes;
    for (InputSectionBase *S : Secs) {
      if (!S)
        continue;
      StringRef N = S->name;
      if (N.starts_with(".gcc_except_table."))
        ExceptTableSuffixes.insert(N.substr(strlen(".gcc_except_table.")));
      else if (N.starts_with(".ARM.exidx.text."))
        ArmExidxSuffixes.insert(N.substr(strlen(".ARM.exidx.text.")));
    }

    // Pre-decode .llvm_xbbr_attr sections, keyed by the ENTRY text section
    // they attach to (sh_link → entry InputSection). One xbbr_attr per
    // function; the entry section is BBAddrMap range 0. FB->getSections() is
    // indexed in parallel with the ELF section headers, so getSections()[sh_link]
    // is the entry section.
    DenseMap<InputSectionBase *, XBBRAttrParseResult> AttrsByEntrySec;
    for (uint32_t Idx : OF->xbbrAttrSectionIndices) {
      if (Idx >= Shdrs->size())
        continue;
      const auto &Shdr = (*Shdrs)[Idx];
      if (Shdr.sh_link >= Secs.size())
        continue;
      InputSectionBase *entrySec = Secs[Shdr.sh_link];
      if (!entrySec)
        continue;
      auto Bytes = EF.getSectionContents(Shdr);
      if (!Bytes) {
        consumeError(Bytes.takeError());
        continue;
      }
      AttrsByEntrySec[entrySec] = parseXBBRAttr(*Bytes);
    }

    // Each decoded BBAddrMap = one function = one FuncId.
    for (BBAddrMapDecoded &DM : DecodedMaps) {
      const BBAddrMap &BAM = DM.Maps.front();
      const PGOAnalysisMap *PAM =
          DM.PGO.empty() ? nullptr : &DM.PGO.front();
      const std::vector<InputSectionBase *> &RangeSecs = DM.RangeSections;
      if (BAM.BBRanges.empty() || RangeSecs.empty())
        continue;
      // Every range must resolve to a section; range 0 (entry) is mandatory
      // (it carries the function symbol — ABI §5.1).
      InputSectionBase *entrySec = RangeSecs[0];
      if (!entrySec)
        continue;

      FuncId Fid = static_cast<FuncId>(Funcs.size());
      FuncInfo FI;
      FI.Section = entrySec;
      FI.FirstNode = static_cast<uint32_t>(Nodes.size());
      if (PAM) {
        FI.HasProfile = true;
        FI.EntryCount = PAM->FuncEntryCount;
      }

      // Map every per-BB InputSection of this function → FuncId (so CGProfile
      // edges and any section lookup resolve correctly).
      for (InputSectionBase *RS : RangeSecs)
        if (RS)
          SectionToFuncId.try_emplace(RS, Fid);

      // xbbr_attr for this function (attached to the entry section).
      const std::vector<uint16_t> *FuncAttrs = nullptr;
      auto AIt = AttrsByEntrySec.find(entrySec);
      if (AIt != AttrsByEntrySec.end() && AIt->second.Ok &&
          !AIt->second.PerFunc.empty())
        FuncAttrs = &AIt->second.PerFunc.front();

      // Flatten BB entries across ranges, carrying each BB's per-range
      // InputSection. AsmPrinter emits ranges in `for (MBB : MF)` order, so
      // positional pairing with PAM->BBEntries (same order) is well-defined.
      struct FlatEntry {
        BBId Id;
        uint32_t Size;
        InputSectionBase *Sec;
      };
      std::vector<FlatEntry> Flat;
      for (size_t R = 0; R < BAM.BBRanges.size(); ++R) {
        InputSectionBase *rsec = R < RangeSecs.size() ? RangeSecs[R] : nullptr;
        for (const auto &E : BAM.BBRanges[R].BBEntries)
          Flat.push_back({E.ID, E.Size, rsec});
      }

      // Consistency: .llvm_xbbr_attr num_bbs must match BBAddrMap BB count.
      // The compiler emits both from the same MIR pass; mismatch = tampering
      // or version skew.
      if (FuncAttrs && FuncAttrs->size() != Flat.size()) {
        Err(ctx) << "XBBR Stage 0: " << entrySec->name << " in "
                 << OF->getName() << ": .llvm_xbbr_attr num_bbs ("
                 << FuncAttrs->size() << ") does not match BB_ADDR_MAP ("
                 << Flat.size() << ")";
        return false;
      }

      // global_freq = BBFreq × EntryCount / EntryBBFreq (PLAN §3.2).
      uint64_t EntryRaw = 0;
      if (PAM && !PAM->BBEntries.empty())
        EntryRaw = PAM->BBEntries.front().BlockFreq.getFrequency();

      for (size_t I = 0; I < Flat.size(); ++I) {
        XBBRNode N;
        N.Func = Fid;
        N.BB = Flat[I].Id;
        N.Size = Flat[I].Size;
        N.BBSection = Flat[I].Sec;
        if (PAM && I < PAM->BBEntries.size())
          N.GlobalFreq = computeGlobalFreq(
              PAM->BBEntries[I].BlockFreq.getFrequency(), EntryRaw,
              FI.EntryCount);
        N.XBBRAttrs = FuncAttrs ? (*FuncAttrs)[I] : 0;
        BBKey K{Fid, N.BB};
        BBIndex.try_emplace(K, static_cast<uint32_t>(Nodes.size()));
        Nodes.push_back(N);
      }
      FI.NumNodes = static_cast<uint32_t>(Nodes.size()) - FI.FirstNode;

      // Detect unthunkable conditional branches from raw reloc TYPES. Section
      // relocs are not scanned yet at this point (scanRelocations runs later,
      // in the Writer), so read the raw ELF reloc data. A function with any
      // such branch must keep all its BBs co-located (same output section) or
      // the branch overflows; renameSectionsForHotColdSplit uses HasCondBranch
      // to skip .text.hot routing for the whole function.
      for (const FlatEntry &Fe : Flat) {
        if (sectionHasCondBranch<ELFT>(Fe.Sec, ctx.arg.emachine)) {
          FI.HasCondBranch = true;
          break;
        }
      }

      // EH gate (Phase 1b): gate if the function has a landing pad or owns an
      // LSDA (`.gcc_except_table.<fn>`). The entry section name is
      // `.text.<fn>` under -ffunction-sections (implied), so the function
      // suffix is everything after the first `.text.` component.
      bool HasLandingPad = false;
      for (uint32_t I = FI.FirstNode; I < Nodes.size(); ++I)
        if (Nodes[I].isLandingPad()) {
          HasLandingPad = true;
          break;
        }
      bool HasLSDA = false;
      bool HasArmExidx = false;
      StringRef EntryName = entrySec->name;
      // Strip a leading `.text.` (or `.text.{hot,unlikely,split,eh}.`) to get
      // the function suffix shared with `.gcc_except_table.<fn>` /
      // `.ARM.exidx.text.<fn>`.
      if (EntryName.starts_with(".text.")) {
        StringRef Suffix = EntryName.substr(strlen(".text."));
        HasLSDA = ExceptTableSuffixes.contains(Suffix);
        HasArmExidx = ArmExidxSuffixes.contains(Suffix);
      }
      FI.IsEHGated = HasLandingPad || HasLSDA || HasArmExidx;
      Funcs.push_back(FI);

      // Intra-function CFG edges from BBAddrMap BrProb (PGOBBEntry::Successors
      // carries (target-BB-ID, BranchProbability) per source BB). Primary input
      // to ExtTSP in Stage 2, alongside cross-function call edges from CGProfile.
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
            // Scale branch probability to an absolute count using the source
            // BB's global frequency: Prob/denom × GlobalFreq ≈ edge exec count.
            E.Weight = static_cast<uint64_t>(
                Succ.Prob.scale(uint64_t(Nodes[SrcIdx].GlobalFreq)));
            // Fallthrough iff the successor BB immediately follows the source
            // BB in original function order (BB IDs are dense after
            // RenumberBlocks).
            E.IsFallthrough = (Flat[I].Id + 1 == Succ.ID);
            E.IsCrossFunc = false;
            E.IsIndirectCall = false;
            Edges.push_back(E);
          }
        }
      }
    }

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
    // first node). Stage 2 will narrow it once BB-level call-site info
    // is available. The callee endpoint also goes to the entry block —
    // it's the only valid landing point for an external call (function
    // symbol = entry).
    const FuncInfo &FFrom = Funcs[From];
    const FuncInfo &FTo = Funcs[To];
    if (FFrom.NumNodes == 0 || FTo.NumNodes == 0)
      continue;
    E.SrcNode = FFrom.FirstNode;
    E.DstNode = FTo.FirstNode;
    E.Weight = KV.second;
    E.IsFallthrough = false;
    E.IsCrossFunc = true;
    // VP-derived indirect-call edges live in the same CGProfile stream
    // as direct calls; we can't tell them apart from the section alone.
    // Treat all edges uniformly here; refinement can come later.
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
  markRangeAnchors(ctx);
  return true;
}

void XBBRGraph::markRangeAnchors(Ctx &ctx) {
  // Unthunkable short-range conditional branches: if either endpoint migrates
  // the branch can overflow, and lld CANNOT extend it with a range-extension
  // thunk (only B/BL-class branches are thunkable). Pin both endpoints so they
  // never migrate — the analog of treating them as anchors.
  //
  //   AArch64: B.cond (R_AARCH64_CONDBR19, ±1 MiB) and TBZ/TBNZ
  //            (R_AARCH64_TSTBR14, ±32 KiB) — lld has no cond-branch thunk.
  //   ARM/Thumb: R_ARM_THM_JUMP8 (B<cond> narrow, ±256 B, Thumb-1) and
  //            R_ARM_THM_JUMP11 (unconditional B narrow, ±2 KiB, Thumb-1) —
  //            lld's needsThunk/inBranchRange do not handle either (the offset
  //            would be silently truncated on overflow), so pin both
  //            defensively. NOTE the encoding↔reloc mapping
  //            (ARMELFObjectWriter.cpp): THM_JUMP8 is the *conditional* B<cond>
  //            (the shorter range — the real hazard), THM_JUMP11 is the
  //            *unconditional* B. Thumb-2 B<cond>.W (R_ARM_THM_JUMP19, ±1 MiB)
  //            IS thunkable by lld (ARM::needsThunk creates a Thumb thunk), so
  //            it is range-checked by Stage 4 (isThunkableBranchReloc), not
  //            pinned here; A32 B<cond> shares R_ARM_JUMP24 with unconditional
  //            B, which lld thunks uniformly.
  //   x86: Jcc is rel32 (±2 GiB, never overflows in practice) — no-op.
  if (ctx.arg.emachine != ELF::EM_AARCH64 && ctx.arg.emachine != ELF::EM_ARM)
    return;

  // A cond-branch BB is "hot" (would land in .text.hot) iff non-anchor,
  // non-cold, and not in an EH-gated function.
  auto isHot = [this](uint32_t I) {
    const XBBRNode &n = Nodes[I];
    return !n.isAnchor() && !n.isCold() && !Funcs[n.Func].IsEHGated;
  };

  // Map each per-BB InputSection back to its node (under =all one section
  // per BB). Used to resolve a conditional branch's target to the dst node.
  DenseMap<const InputSectionBase *, uint32_t> secToNode;
  for (uint32_t I = 0; I < Nodes.size(); ++I)
    if (Nodes[I].BBSection)
      secToNode.try_emplace(Nodes[I].BBSection, I);

  // Mark CondInvolved and collect cond-branch partnership edges (X, Y).
  std::vector<std::pair<uint32_t, uint32_t>> condEdges;
  for (uint32_t I = 0; I < Nodes.size(); ++I) {
    InputSectionBase *src = Nodes[I].BBSection;
    if (!src)
      continue;
    for (const Relocation &r : src->relocs()) {
      if (!isUnthunkableCondRelocType(ctx.arg.emachine, r.type))
        continue;
      // This BB issues an unthunkable conditional branch — pin it (can't thunk).
      Nodes[I].CondInvolved = true;
      // Pin the target too: if the target BB migrates away, the branch
      // overflows just the same. Resolve reloc target → its section → node.
      auto *d = dyn_cast<Defined>(r.sym);
      auto *tsec = d ? dyn_cast_or_null<InputSectionBase>(d->section) : nullptr;
      auto it = tsec ? secToNode.find(tsec) : secToNode.end();
      if (it != secToNode.end()) {
        Nodes[it->second].CondInvolved = true;
        condEdges.push_back({I, it->second});
      }
    }
  }

  // P1-3 (AArch64 only): a CondInvolved BB may migrate (to .text.hot) iff its
  // entire cond-branch connected component is hot. Cross-section unsafety (one
  // endpoint .text.hot, the other .text — unthunkable, would hard-error)
  // propagates through the partnership graph, so a single non-hot partner pins
  // the whole component. Fixed-point: start CondSafeToMigrate = hot, then clear
  // it on any node whose partner is not safe, until stable. ARM Thumb-1
  // R_ARM_THM_JUMP8/JUMP11 stay unconditionally pinned (lld can neither thunk
  // nor relax them) — their CondSafeToMigrate is left false.
  if (ctx.arg.emachine == ELF::EM_AARCH64) {
    for (uint32_t I = 0; I < Nodes.size(); ++I)
      if (Nodes[I].CondInvolved)
        Nodes[I].CondSafeToMigrate = isHot(I);
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &E : condEdges) {
        uint32_t X = E.first, Y = E.second;
        if (Nodes[X].CondSafeToMigrate && !Nodes[Y].CondSafeToMigrate) {
          Nodes[X].CondSafeToMigrate = false;
          changed = true;
        }
        if (Nodes[Y].CondSafeToMigrate && !Nodes[X].CondSafeToMigrate) {
          Nodes[Y].CondSafeToMigrate = false;
          changed = true;
        }
      }
    }
  }
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
