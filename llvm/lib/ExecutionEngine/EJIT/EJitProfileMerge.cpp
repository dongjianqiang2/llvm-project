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
#include <cstdint>
#include <vector>

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

  // Runtime layout of __llvm_profile_data, mirroring InstrProfData.inc (same
  // LLVM build -> identical layout). Field offsets on 64-bit:
  //   0  NameRef      (uint64_t)
  //   8  FuncHash     (uint64_t)
  //  16  CounterPtr   (uintptr_t) -- NOT used: EJIT resolves the counter array
  //                                  address via ORC lookup (profcAddr), which
  //                                  is absolute; the struct's CounterPtr may
  //                                  be relative/biased.
  //  24  BitmapPtr    32 FunctionPointer    40 Values
  //  48  NumCounters  (uint32_t)
  // EJIT_ONLINE_PGO.md §5.3.
  static_assert(sizeof(uintptr_t) == 8,
                "EJIT PGO runtime profile-data offsets assume 64-bit");
  constexpr uintptr_t kFuncHashOff = 8;
  constexpr uintptr_t kNumCountersOff = 48;
  // Sanity cap: a single function's edge counter array is never huge; a bogus
  // offset read (layout drift) would typically yield a wild NumCounters.
  constexpr uint32_t kMaxCountersPerFunc = 1u << 20;

  for (const PgoCounterRef &C : counters) {
    if (!C.profdAddr || !C.profcAddr || !C.pgoName)
      continue;
    const auto *Data = reinterpret_cast<const uint8_t *>(C.profdAddr);
    uint64_t FuncHash =
        *reinterpret_cast<const uint64_t *>(Data + kFuncHashOff);
    uint32_t NumCounters =
        *reinterpret_cast<const uint32_t *>(Data + kNumCountersOff);
    if (NumCounters == 0 || NumCounters > kMaxCountersPerFunc)
      continue;
    const auto *CounterArray =
        reinterpret_cast<const uint64_t *>(C.profcAddr);
    // The __profc_* counters are being updated concurrently by shared Tier-1
    // machine code with `atomicrmw add` (§5). Read each counter with a RELAXED
    // atomic load so this synthesis never tears a value another core is mid-way
    // updating (a plain copy is a data race). Typed uint64_t scalar loads keep
    // this endian-safe on aarch64_be (no byte-wise counter parsing).
    std::vector<uint64_t> Counts;
    Counts.reserve(NumCounters);
    for (uint32_t i = 0; i < NumCounters; ++i)
      Counts.push_back(__atomic_load_n(&CounterArray[i], __ATOMIC_RELAXED));
    NamedInstrProfRecord Rec(C.pgoName, FuncHash, std::move(Counts));
    Writer.addRecord(std::move(Rec), 1, [](Error) {});
  }

  auto Buf = Writer.writeBuffer();
  if (!Buf)
    return {};
  return std::string(Buf->getBuffer());
}
