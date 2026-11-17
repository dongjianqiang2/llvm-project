//===-- EJitAotModulePass.cpp - EmbeddedJIT AOT Coordinator --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  PASS5: Late-stage AOT pipeline coordinator. Runs PASS2→PASS3→PASS4
//  in order (PASS1 is an independent early pass) and manages
//  PreservedAnalyses composition.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-aot-module"

static cl::opt<bool> PrintIcacheDimSize(
    "print-ejit-icache-dim-size", cl::init(false), cl::Hidden,
    cl::desc("Print the EJIT_ICACHE_DIM_SIZE value this clang was built with "
             "and exit.  Used by the build system to verify the AOT pass and "
             "runtime agree on D."));

namespace {

static bool hasAnyEjitMetadata(Module &M) {
  for (Function &F : M.functions())
    if (F.hasMetadata(MD_EJIT_METADATA))
      return true;
  for (GlobalVariable &GV : M.globals())
    if (GV.hasMetadata(MD_EJIT_METADATA))
      return true;
  return false;
}

} // anonymous namespace

PreservedAnalyses
EJitAotModulePass::run(Module &M, ModuleAnalysisManager &AM) {
  // Build-system consistency check: print the D value this clang was built with
  // and exit immediately so CMake can compare against the runtime's value.
  if (PrintIcacheDimSize) {
    outs() << EJIT_ICACHE_DIM_SIZE << '\n';
    std::exit(0);
  }

  if (!hasAnyEjitMetadata(M)) {
    LLVM_DEBUG(dbgs() << "ejit-aot-module: no EJIT metadata, skip\n");
    return PreservedAnalyses::all();
  }

  // Run sub-passes in order: PASS2 → PASS3 → PASS4
  // (PASS1 is an independent early pass, not part of this pipeline)
  PreservedAnalyses PA = PreservedAnalyses::all();

  PA.intersect(EJitRegisterPeriodPass().run(M, AM));
  PA.intersect(EJitWrapperGenPass().run(M, AM));
  PA.intersect(EJitPeriodHandlerPass().run(M, AM));

  return PA;
}
