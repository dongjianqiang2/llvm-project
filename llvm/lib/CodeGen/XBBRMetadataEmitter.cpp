//===-- XBBRMetadataEmitter.cpp - XBBR metadata emitter pass --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// XBBRMetadataEmitter runs after MachineBlockPlacement and computes, for each
// MachineBasicBlock, the XBBR blacklist/attribute bits (SPEC §5.3) that
// AsmPrinter emits into the .llvm_xbbr_attr section. The actual section
// emission lives in AsmPrinter (which owns the MCStreamer), mirroring how
// SHT_LLVM_BB_ADDR_MAP is emitted via AsmPrinter::emitBBAddrMapSection.
//
// This file currently provides the pass skeleton: registration, scheduling,
// and mode gating. Blacklist detection and section emission are layered on in
// subsequent M1 tasks (M1-T05).
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/XBBRMetadata.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "xbbr-metadata-emitter"

/// Testing flag enabling XBBR metadata emission from llc. clang instead sets
/// TargetOptions::XBBR via -fbb-cross-reorder= (wired up in M1-T06).
cl::opt<bool> llvm::EnableXBBR(
    "enable-xbbr", cl::Hidden, cl::init(false),
    cl::desc("Enable XBBR metadata emission (.llvm_xbbr_attr) for testing"));

namespace {

class XBBRMetadataEmitter : public MachineFunctionPass {
public:
  static char ID;

  XBBRMetadataEmitter() : MachineFunctionPass(ID) {
    initializeXBBRMetadataEmitterPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "XBBR Metadata Emitter"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (!EnableXBBR)
      return false;
    if (skipFunction(MF.getFunction()))
      return false;

    LLVM_DEBUG(dbgs() << "XBBRMetadataEmitter running on " << MF.getName()
                      << " (" << MF.size() << " MBBs)\n");

    // Skeleton: per-BB blacklist detection and .llvm_xbbr_attr emission are
    // added in M1-T05. This pass does not modify the MIR.
    return false;
  }
};

} // end anonymous namespace

char XBBRMetadataEmitter::ID = 0;
INITIALIZE_PASS(XBBRMetadataEmitter, "xbbr-metadata-emitter",
                "XBBR Metadata Emitter", false, false)

MachineFunctionPass *llvm::createXBBRMetadataEmitterPass() {
  return new XBBRMetadataEmitter();
}
