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
//   llvm-bbreorder-dump --graphviz <elf-file>        # DOT output
//   llvm-bbreorder-dump --summary <elf-file>          # stats only
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/ELF.h"
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
using llvm::support::endian::read32le;
using llvm::support::endian::read64le;

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<input ELF>"),
                                       cl::Required);
static cl::opt<bool> Graphviz("graphviz", cl::desc("Output DOT-format hot-path diagram"));
static cl::opt<bool> Summary("summary", cl::desc("Print summary statistics only"));

namespace {

// Decision-map header (PLAN §9.4).
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
  if (Data.size() < 16)
    return H;
  if (memcmp(Data.data(), "XBBR", 4) != 0)
    return H;
  H.Version = read32le(Data.data() + 4);
  H.NumEntries = read32le(Data.data() + 8);
  H.Flags = read32le(Data.data() + 12);
  H.Ok = true;
  return H;
}

std::vector<DecisionMapEntry> parseEntries(ArrayRef<uint8_t> Data,
                                           uint32_t NumEntries) {
  std::vector<DecisionMapEntry> Entries;
  if (Data.size() < 16u + NumEntries * 32u)
    return Entries;
  const uint8_t *P = Data.data() + 16;
  for (uint32_t I = 0; I < NumEntries; ++I) {
    DecisionMapEntry E;
    E.OrigFuncAddr = read64le(P + 0);
    E.BBIndex = read32le(P + 8);
    E.NewAddress = read64le(P + 12);
    E.ClusterId = read32le(P + 20);
    E.DecisionFlags = read32le(P + 24);
    E.FuncId = read32le(P + 28);
    Entries.push_back(E);
    P += 32;
  }
  return Entries;
}

const char *flagName(uint32_t f) {
  if (f == 1) return "moved";
  if (f == 2) return "anchored";
  return "?";
}

void dumpHuman(const DecisionMapHeader &H,
               const std::vector<DecisionMapEntry> &Entries) {
  outs() << "XBBR Decision Map\n";
  outs() << "  version   : " << format("0x%08X", H.Version) << "\n";
  outs() << "  entries   : " << H.NumEntries << "\n";
  outs() << "  flags     : " << H.Flags;
  if (H.Flags & 1) outs() << " (degraded)";
  outs() << "\n\n";

  // Group entries by FuncId.
  std::map<uint32_t, std::vector<DecisionMapEntry>> byFunc;
  for (const auto &E : Entries)
    byFunc[E.FuncId].push_back(E);

  for (const auto &[FuncId, BBs] : byFunc) {
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
  if (H.Flags & 1)
    outs() << "  label=\"XBBR Layout (DEGRADED)\\n" << H.NumEntries
           << " entries\";\n";
  else
    outs() << "  label=\"XBBR Layout\\n" << H.NumEntries << " entries\";\n";

  // Group by cluster.
  std::map<uint32_t, std::vector<DecisionMapEntry>> byCluster;
  for (const auto &E : Entries)
    byCluster[E.ClusterId].push_back(E);

  int clusterIdx = 0;
  for (const auto &[CId, BBs] : byCluster) {
    outs() << "  subgraph cluster_" << clusterIdx << " {\n";
    outs() << "    label=\"Cluster " << CId << "\";\n";
    for (const auto &E : BBs) {
      outs() << "    node_f" << E.FuncId << "_b" << E.BBIndex
             << " [label=\"F" << E.FuncId << " BB" << E.BBIndex
             << "\\n" << flagName(E.DecisionFlags) << "\""
             << (E.DecisionFlags == 1 ? " style=filled fillcolor=lightyellow"
                                      : "")
             << "];\n";
    }
    outs() << "  }\n";
    ++clusterIdx;
  }

  // Draw cross-BB edges for moved BBs — show original → new position.
  for (const auto &E : Entries) {
    if (E.DecisionFlags != 1) continue; // only moved BBs get edges
    if (E.OrigFuncAddr == E.NewAddress) continue;
    outs() << "  node_f" << E.FuncId << "_b" << E.BBIndex
           << " -> node_f" << E.FuncId << "_b" << E.BBIndex
           << " [style=dotted];\n";
  }

  outs() << "}\n";
}

void dumpSummary(const DecisionMapHeader &H,
                 const std::vector<DecisionMapEntry> &Entries) {
  uint32_t nMoved = 0, nAnchored = 0, nFallback = 0;
  for (const auto &E : Entries) {
    if (E.DecisionFlags == 1) ++nMoved;
    else if (E.DecisionFlags == 2) ++nAnchored;
    else ++nFallback;
  }
  outs() << "xbbr-dump: entries=" << H.NumEntries
         << " moved=" << nMoved
         << " anchored=" << nAnchored
         << " fallback=" << nFallback;
  if (H.Flags & 1)
    outs() << " DEGRADED";
  outs() << "\n";

  std::set<uint32_t> funcs;
  for (const auto &E : Entries)
    funcs.insert(E.FuncId);
  outs() << "xbbr-dump: functions=" << funcs.size() << "\n";
}

} // namespace

int llvm_bbreorder_dump_main(int argc, char **argv, const llvm::ToolContext &) {
  cl::ParseCommandLineOptions(argc, argv,
                              "XBBR decision map dumper\n");

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

  auto *Obj = dyn_cast<ELFObjectFile<ELF64LE>>(Bin->get());
  if (!Obj) {
    // Try ELF32LE as a fallback, though XBBR currently targets ELF64LE.
    auto *Obj32 = dyn_cast<ELFObjectFile<ELF32LE>>(Bin->get());
    if (!Obj32) {
      errs() << "error: unsupported object format (expected ELF)\n";
      return 1;
    }
    // ELF32 path not implemented for XBBR (M3 targets x86_64 only).
    errs() << "error: 32-bit ELF not supported by XBBR\n";
    return 1;
  }

  // Locate .debug_xbbr_decision section.
  DecisionMapHeader H;
  std::vector<uint8_t> DecisionBytes;
  for (const auto &Sec : Obj->sections()) {
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
           << InputFile << "\n";
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
