//===- EJitDiagnostics.cpp - Location-anchored errors for EJIT passes -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitDiagnostics.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"

using namespace llvm;

void llvm::ejit::emitFunctionError(Function &F, StringRef PassName,
                                   const Twine &Msg) {
  // Anchor at the function's subprogram when one is attached so the frontend
  // can point at the definition directly; without it clang still approximates
  // the location from the function's definition (getFunctionSourceLocation).
  DiagnosticLocation Loc;
  if (const auto *SP = F.getSubprogram())
    Loc = DiagnosticLocation(SP);
  // DiagnosticInfoUnsupported keeps a reference, so materialize the Twine.
  std::string MsgStr = (PassName + ": " + Msg).str();
  F.getContext().diagnose(DiagnosticInfoUnsupported(F, MsgStr, Loc, DS_Error));
}
