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
//  The undeclared-period-dependency warning that used to be printed here
//  now lives in clang Sema (warn_ejit_undeclared_period_dep), where it
//  gets a source location and -W flag control.
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

extern cl::opt<bool> EnableEJitSmallTableHooks;

#define DEBUG_TYPE "ejit-aot-module"

static cl::opt<bool> PrintIcacheDimSize(
    "print-ejit-icache-dim-size", cl::init(false), cl::Hidden,
    cl::desc("Print the EJIT_ICACHE_DIM_SIZE value this clang was built with "
             "and exit.  Used by the build system to verify the AOT pass and "
             "runtime agree on D."));

namespace {

static bool hasAnyEjitAotInput(Module &M) {
  for (Function &F : M.functions())
    if (F.hasMetadata(MD_EJIT_METADATA))
      return true;
  for (GlobalVariable &GV : M.globals())
    // A shared-only controller TU may have no EJIT metadata. The already
    // opt-in PASS2 path still needs to emit its exact static-object inventory.
    if (GV.hasMetadata(MD_EJIT_METADATA) ||
        (EnableEJitSmallTableHooks && !GV.isDeclaration() &&
         !GV.hasAvailableExternallyLinkage() && !GV.isThreadLocal() &&
         GV.getAddressSpace() == 0 &&
         (GV.getSection() == ".mc_shared" ||
          GV.getSection().starts_with(".mc_shared."))))
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

  if (!hasAnyEjitAotInput(M)) {
    LLVM_DEBUG(dbgs() << "ejit-aot-module: no EJIT input, skip\n");
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
