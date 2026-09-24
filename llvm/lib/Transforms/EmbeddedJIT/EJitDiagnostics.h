//===- EJitDiagnostics.h - Location-anchored errors for EJIT passes -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Location-anchored error reporting for the EmbeddedJIT AOT passes. A bare
// LLVMContext::emitError reaches clang's generic backend-plugin diagnostic,
// which drops both the function and the source location; the helper here
// emits a DiagnosticInfoUnsupported instead so the frontend renders
// "file:line:col: error: <msg>" (or, without debug info, points at the
// function's definition).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_EMBEDDEDJIT_EJITDIAGNOSTICS_H
#define LLVM_LIB_TRANSFORMS_EMBEDDEDJIT_EJITDIAGNOSTICS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace llvm {

class Function;

namespace ejit {

/// Emit a pass error anchored at \p F. The clang backend pipeline renders it
/// as "file:line:col: error: <msg>" when the function carries debug info and
/// falls back to the function's definition location otherwise; the plain opt
/// default handler prints "in function <name>: <msg>". \p PassName prefixes
/// the message the way the old bare emitError() calls did ("ejit-wrapper-gen:
/// ..."). \p Msg must describe the offending metadata itself (period name,
/// argument index, ...) since the diagnostic class carries the anchor.
void emitFunctionError(Function &F, StringRef PassName, const Twine &Msg);

} // namespace ejit
} // namespace llvm

#endif
