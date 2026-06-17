//===-- llvm/CodeGen/XBBRMetadata.h - XBBR metadata emission ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This declares the XBBRMetadataEmitter pass interface. The pass runs after
// MachineBlockPlacement and computes per-basic-block XBBR metadata (blacklist
// attributes, SPEC §5.3) that AsmPrinter emits into the .llvm_xbbr_attr
// section. Enabled by -fbb-cross-reorder=partial|full (clang) or -enable-xbbr
// (llc testing).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_XBBRMETADATA_H
#define LLVM_CODEGEN_XBBRMETADATA_H

#include "llvm/Support/CommandLine.h"

namespace llvm {

class MachineFunctionPass;

/// Testing flag that enables XBBR metadata emission from llc (clang instead
/// sets TargetOptions::XBBR via -fbb-cross-reorder=). Referenced by both the
/// pass scheduler (TargetPassConfig) and AsmPrinter so they stay in sync.
extern cl::opt<bool> EnableXBBR;

/// createXBBRMetadataEmitterPass - This pass computes XBBR per-BB metadata
/// after MachineBlockPlacement, for emission of .llvm_xbbr_attr by AsmPrinter.
MachineFunctionPass *createXBBRMetadataEmitterPass();

} // namespace llvm

#endif // LLVM_CODEGEN_XBBRMETADATA_H
