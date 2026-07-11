//===-- EJitProfileMerge.cpp - in-memory PGO profile synthesis ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfWriter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace llvm::ejit;

std::string ejit::synthesizeProfileBuffer(ArrayRef<PgoCounterRef> counters) {
  InstrProfWriter Writer;
  // P0 finding: manual addRecord does not propagate the IR-level flag
  // (llvm-profdata merge normally reads it from the raw profile header). Set
  // it explicitly so PGOInstrumentationUse accepts this as an IR profile
  // (else "Not an IR level instrumentation profile").
  if (auto E = Writer.mergeProfileKind(InstrProfKind::IRInstrumentation))
    consumeError(std::move(E));

  // Stage 1 (EJIT_ONLINE_PGO.md §5.3): for each counter, read the
  // __llvm_profile_data struct at profdAddr (via InstrProfData.inc:
  // FuncHash + Counters pointer + NumCounters), build a NamedInstrProfRecord
  // with the counter values at profcAddr, and Writer.addRecord. The
  // __llvm_profile_data layout matches this LLVM build (InstrProfData.inc).
  //
  // Skeleton (Stage 0): no records added yet; returning the empty-writer
  // buffer exercises the InstrProfWriter link path so the PGO component
  // deps are pulled into LLVMEJIT. Behavior is opt-in (PGO off) so this is
  // not called until Stage 1 wires it.
  (void)counters;

  auto Buf = Writer.writeBuffer();
  if (!Buf)
    return {};
  return std::string(Buf->getBuffer());
}
