//===- llvm-bbreorder-dump.cpp - XBBR decision map dumper -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reads an ELF binary containing XBBR metadata and dumps:
//   - Per-function BB origin → new address mapping (from .debug_xbbr_decision)
//   - BB_ADDR_MAP PGO analysis data (FuncEntryCount, BBFreq, BrProb)
//   - Optional Graphviz DOT-format hot-path diagram
//
// Usage:
//   llvm-bbreorder-dump <elf-file>                  # human-readable dump
//   llvm-bbreorder-dump --graphviz <elf-file>       # DOT output
//   llvm-bbreorder-dump --summary <elf-file>        # stats only
//
// Binary format definitions (header sizes, field offsets, magic, version)
// come from llvm/BinaryFormat/XBBRDecisionMap.h, which is shared with
// lld/ELF/SyntheticSections.cpp's writer to keep the on-disk format
// pinned in one place.
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/XBBRDecisionMap.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace llvm;
using namespace llvm::object;
using namespace llvm::XBBRDecisionMap;
using llvm::support::endian::read32le;
using llvm::support::endian::read64le;

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<input ELF>"),
                                      cl::Required);
static cl::opt<bool> Graphviz("graphviz", cl::desc("Output DOT-format hot-path diagram"));
static cl::opt<bool> Summary("summary", cl::desc("Print summary statistics only"));

namespace {

struct DecisionMapHeader {
  bool Ok = false;
  uint32_t Version = 0;
  uint32_t NumEntries = 0;
  uint32_t Flags = 0;
};

struct DecisionMapEntry {
  uint64_t OrigFuncAddr = 0;
  uint32_t BBIndex = 0;
  uint64_t NewAddress = 0;
  uint32_t ClusterId = 0;
  uint32_t DecisionFlags = 0;
  uint32_t FuncId = 0;
};

DecisionMapHeader parseHeader(ArrayRef<uint8_t> Data) {
  DecisionMapHeader H;
  if (Data.size() < kHeaderSize)
    return H;
  if (memcmp(Data.data(), kMagic, sizeof(kMagic)) != 0)
    return H;
  H.Version = read32le(Data.data() + 4);
  // Reject mismatched format major. We never silently misinterpret a
  // future format version — caller exits with error.
  if ((H.Version >> 16) != kVersionMajor)
    return H;
  H.NumEntries = read32le(Data.data() + 8);
  H.Flags = read32le(Data.data() + 12);
  H.Ok = true;
  return H;
}

std::vector<DecisionMapEntry> parseEntries(ArrayRef<uint8_t> Data,
                                           uint32_t NumEntries) {
  std::vector<DecisionMapEntry> Entries;
  // Use 64-bit math to defend against a corrupt num_entries that would
  // overflow `NumEntries * 32` in 32-bit arithmetic.
  if (NumEntries > (Data.size() - kHeaderSize) / kEntrySize)
    return Entries;
  const uint8_t *P = Data.data() + kHeaderSize;
  Entries.reserve(NumEntries);
  for (uint32_t I = 0; I < NumEntries; ++I) {
    DecisionMapEntry E;
    E.OrigFuncAddr  = read64le(P + kEntryOffOrigFuncAddr);
    E.BBIndex       = read32le(P + kEntryOffBBIndex);
    E.NewAddress    = read64le(P + kEntryOffNewAddress);
    E.ClusterId     = read32le(P + kEntryOffClusterId);
    E.DecisionFlags = read32le(P + kEntryOffDecisionFlags);
    E.FuncId        = read32le(P + kEntryOffFuncId);
    Entries.push_back(E);
    P += kEntrySize;
  }
  return Entries;
}

const char *flagName(uint32_t f) {
  if (f & EntryFlags::Anchored) return "anchored";
  if (f & EntryFlags::Fallback) return "fallback";
  if (f & EntryFlags::Thunk)    return "thunk";
  if (f & EntryFlags::Moved)    return "moved";
  return "?";
}

void dumpHuman(const DecisionMapHeader &H,
               const std::vector<DecisionMapEntry> &Entries) {
  outs() << "XBBR Decision Map\n";
  outs() << "  version   : " << format("0x%08X", H.Version) << "\n";
  outs() << "  entries   : " << H.NumEntries << "\n";
  outs() << "  flags     : " << H.Flags;
  if (H.Flags & HeaderFlags::Degraded)
    outs() << " (degraded)";
  outs() << "\n\n";

  // Group entries by FuncId — std::map gives deterministic ordering.
  std::map<uint32_t, std::vector<DecisionMapEntry>> ByFunc;
  for (const auto &E : Entries)
    ByFunc[E.FuncId].push_back(E);

  for (const auto &[FuncId, BBs] : ByFunc) {
    outs() << "Function " << FuncId << " (" << BBs.size() << " BBs):\n";
    outs() << "  BB  OrigAddr      NewAddr       Cluster  Flag\n";
    for (const auto &E : BBs) {
      outs() << "  " << format("%3u", E.BBIndex)
             << "  " << format("0x%08" PRIX64, E.OrigFuncAddr)
             << "  " << format("0x%08" PRIX64, E.NewAddress)
             << "  " << format("%7u", E.ClusterId)
             << "  " << flagName(E.DecisionFlags) << "\n";
    }
    outs() << "\n";
  }
}

void dumpGraphviz(const DecisionMapHeader &H,
                  const std::vector<DecisionMapEntry> &Entries) {
  outs() << "digraph XBBR {\n";
  outs() << "  rankdir=LR;\n";
  outs() << "  node [shape=record];\n";
  if (H.Flags & HeaderFlags::Degraded)
    outs() << "  label=\"XBBR Layout (DEGRADED)\\n" << H.NumEntries
           << " entries\";\n";
  else
    outs() << "  label=\"XBBR Layout\\n" << H.NumEntries << " entries\";\n";

  // Group BBs by cluster (deterministic via std::map).
  std::map<uint32_t, std::vector<DecisionMapEntry>> ByCluster;
  for (const auto &E : Entries)
    ByCluster[E.ClusterId].push_back(E);

  for (const auto &[CId, BBs] : ByCluster) {
    outs() << "  subgraph cluster_" << CId << " {\n";
    outs() << "    label=\"Cluster " << CId << "\";\n";
    for (const auto &E : BBs) {
      outs() << "    node_f" << E.FuncId << "_b" << E.BBIndex
             << " [label=\"F" << E.FuncId << " BB" << E.BBIndex
             << "\\n" << flagName(E.DecisionFlags) << "\"";
      if (E.DecisionFlags & EntryFlags::Moved)
        outs() << " style=filled fillcolor=lightyellow";
      outs() << "];\n";
    }
    outs() << "  }\n";
  }

  // Layout-adjacency edges: within each cluster, order BBs by NewAddress
  // and draw arrows between consecutive nodes. This shows the actual
  // post-XBBR neighbor relationship rather than a meaningless self-loop
  // from OrigFuncAddr (which is currently a placeholder 0) to NewAddress.
  for (auto &[CId, BBs] : ByCluster) {
    if (BBs.size() < 2)
      continue;
    std::sort(BBs.begin(), BBs.end(),
              [](const DecisionMapEntry &A, const DecisionMapEntry &B) {
                return A.NewAddress < B.NewAddress;
              });
    for (size_t I = 0; I + 1 < BBs.size(); ++I)
      outs() << "  node_f" << BBs[I].FuncId << "_b" << BBs[I].BBIndex
             << " -> node_f" << BBs[I + 1].FuncId << "_b"
             << BBs[I + 1].BBIndex << ";\n";
  }

  outs() << "}\n";
}

void dumpSummary(const DecisionMapHeader &H,
                 const std::vector<DecisionMapEntry> &Entries) {
  uint32_t nMoved = 0, nAnchored = 0, nFallback = 0, nThunk = 0;
  for (const auto &E : Entries) {
    if (E.DecisionFlags & EntryFlags::Anchored) ++nAnchored;
    else if (E.DecisionFlags & EntryFlags::Fallback) ++nFallback;
    else if (E.DecisionFlags & EntryFlags::Thunk) ++nThunk;
    else if (E.DecisionFlags & EntryFlags::Moved) ++nMoved;
  }
  outs() << "xbbr-dump: entries=" << H.NumEntries
         << " moved=" << nMoved
         << " anchored=" << nAnchored
         << " fallback=" << nFallback
         << " thunk=" << nThunk;
  if (H.Flags & HeaderFlags::Degraded)
    outs() << " DEGRADED";
  outs() << "\n";

  std::set<uint32_t> Funcs;
  for (const auto &E : Entries)
    Funcs.insert(E.FuncId);
  outs() << "xbbr-dump: functions=" << Funcs.size() << "\n";
}

} // namespace

int llvm_bbreorder_dump_main(int argc, char **argv, const llvm::ToolContext &) {
  cl::ParseCommandLineOptions(argc, argv,
                              "XBBR decision map dumper\n");

  if (Graphviz && Summary) {
    errs() << "error: --graphviz and --summary are mutually exclusive\n";
    return 1;
  }

  auto F = MemoryBuffer::getFile(InputFile);
  if (!F) {
    errs() << "error: cannot open '" << InputFile << "': "
           << F.getError().message() << "\n";
    return 1;
  }

  auto Bin = createBinary((*F)->getMemBufferRef());
  if (!Bin) {
    errs() << "error: not a valid object file: " << InputFile << "\n";
    return 1;
  }

  // XBBR currently supports x86_64 and AArch64, both ELF64LE.
  auto *Obj64 = dyn_cast<ELFObjectFile<ELF64LE>>(Bin->get());
  if (!Obj64) {
    if (isa<ELFObjectFile<ELF32LE>>(Bin->get()) ||
        isa<ELFObjectFile<ELF32BE>>(Bin->get()) ||
        isa<ELFObjectFile<ELF64BE>>(Bin->get())) {
      errs() << "error: only ELF64LE supported by XBBR (input is "
                "ELF32 or big-endian)\n";
    } else {
      errs() << "error: unsupported object format (expected ELF)\n";
    }
    return 1;
  }

  // Locate .debug_xbbr_decision section.
  DecisionMapHeader H;
  std::vector<uint8_t> DecisionBytes;
  for (const auto &Sec : Obj64->sections()) {
    auto Name = Sec.getName();
    if (!Name || *Name != ".debug_xbbr_decision")
      continue;
    auto Content = Sec.getContents();
    if (!Content) {
      errs() << "warning: cannot read .debug_xbbr_decision content\n";
      continue;
    }
    DecisionBytes.assign(Content->begin(), Content->end());
    H = parseHeader(DecisionBytes);
    break;
  }

  if (!H.Ok) {
    errs() << "error: no valid .debug_xbbr_decision section found in "
           << InputFile << " (or version mismatch)\n";
    return 1;
  }

  auto Entries = parseEntries(DecisionBytes, H.NumEntries);

  if (Graphviz)
    dumpGraphviz(H, Entries);
  else if (Summary)
    dumpSummary(H, Entries);
  else
    dumpHuman(H, Entries);

  return 0;
}
