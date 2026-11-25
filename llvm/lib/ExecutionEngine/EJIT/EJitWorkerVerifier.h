//===-- EJitWorkerVerifier.h - Worker-only substitution checks ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITWORKERVERIFIER_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITWORKERVERIFIER_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#include <string>
#include <vector>

namespace llvm::ejit {

class EJitSharedTaskPool;

struct EJitWorkerVerifyWatch {
  std::string function;
  std::string object;
  unsigned loadOrdinal = 0;
  uint8_t width = 0;
  bool littleEndian = false;
  bool rootValue = false;
  uint64_t expected = 0;
  uintptr_t rootSlot = 0;
  uintptr_t expectedRoot = 0;
  SmallVector<uintptr_t, 10> addresses;
  uint16_t mismatched = 0;
  uint16_t seen = 0;
};

// Compile-side only. No LLVM IR or borrowed calling arguments survive capture.
struct EJitWorkerVerifyCapture {
  static constexpr unsigned MaxWatches = 128;
  StringRef period;
  std::vector<EJitWorkerVerifyWatch> watches;
  unsigned uncovered = 0;
  unsigned dropped = 0;
};

class EJitWorkerVerifier {
public:
  void onCompiled(const EJitCompileRequest &Req, uintptr_t Fn,
                  EJitWorkerVerifyCapture Capture);
  void onPublished(const EJitCompileRequest &Req, bool Published);
  void idle(const EJitSharedTaskPool &Pool, bool DumpRequested);

private:
  struct Version {
    EJitCompileRequest request;
    uintptr_t function;
    EJitWorkerVerifyCapture capture;
    unsigned watch = 0;
    unsigned residue = 0;
  };
  static bool sameRequest(const EJitCompileRequest &A,
                          const EJitCompileRequest &B);
  void dropCurrent();
  void advance();
  void dump() const;

  static constexpr unsigned MaxVersions = 1024;
  static constexpr unsigned MaxWatches = 8192;
  static constexpr unsigned IdleTicks = 8;
  static constexpr unsigned StepBudget = 32;
  std::vector<Version> pending_, live_;
  size_t cursor_ = 0;
  unsigned retainedWatches_ = 0;
  unsigned idleTicks_ = 0;
  uint64_t checks_ = 0;
  uint64_t activeProbes_ = 0;
  uint64_t checkedActiveProbes_ = 0;
  uint64_t mismatchSamples_ = 0;
  uint64_t uncovered_ = 0;
  uint64_t dropped_ = 0;
  uint64_t blockedRoot_ = 0;
  uint64_t retired_ = 0;
  uint64_t suppressed_ = 0;
};

} // namespace llvm::ejit
#endif
