//===-- EJitOptimizer.cpp - JIT Optimization Pipeline ---------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/EJIT/EJitPassBuilder.h"
#include "llvm/Support/Debug.h"
// JIT Inline disabled: AOT pre-optimization already inlines.
// #include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/LoopDeletion.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Instrumentation/PGOInstrumentation.h"
#include "llvm/Transforms/Instrumentation/InstrProfiling.h"
#include "llvm/Transforms/IPO/Inliner.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/IR/GlobalValue.h"

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-optimizer"

EJitOptimizer::EJitOptimizer(PeriodArrayRegistry &reg)
    : registry_(reg) {
  EJitPassBuilder::registerFunctionAnalyses(FAM_);
  EJitPassBuilder::registerLoopAnalyses(LAM_);
  EJitPassBuilder::registerCGSCCAnalyses(CGAM_);
  EJitPassBuilder::registerModuleAnalyses(MAM_);
  EJitPassBuilder::crossRegisterProxies(LAM_, FAM_, CGAM_, MAM_);

  // Pre-build the two cached FunctionPassManagers of the optimization pipeline.
  //
  // By the time these run, runPipeline has turned the period-index argument and
  // every may_const field into a compile-time constant. The pipeline's whole job
  // is to exploit those constants maximally, so it is structured as a fold →
  // propagate → simplify fixed point followed by loop folding.
  //
  // mainFPM_ — Phase 2 (scalar) then Phase 3 (loops):
  //
  //   Phase 2 drives the substituted constants to a fixed point. The first round
  //   peephole-folds the constants (InstCombine), propagates them and folds the
  //   now-constant branches (SCCP), and deletes the unreachable blocks
  //   (SimplifyCFG). Folding a branch merges blocks and exposes fresh constants
  //   and dead code, so a second InstCombine + SimplifyCFG round catches that
  //   cascade; ADCE then removes whatever became dead.
  //   LowerExpectIntrinsic must run FIRST: the AOT IR carries llvm.expect
  //   guards (__builtin_expect / LIKELY) on top of may_const conditions. Once
  //   StructFieldPass (phase 1c) turns those conditions into constants, the
  //   expect intrinsic is the only thing preventing InstCombine/SCCP from
  //   folding the branch - without lowering it the constant branches stay,
  //   their dead blocks/calls survive ADCE, and the specialization does the
  //   same work as AOT (plus JIT overhead), i.e. a slowdown. Lowering
  //   expect(x,y) -> x lets the rest of the pipeline fold and DCE.
  mainFPM_.addPass(LowerExpectIntrinsicPass());
  mainFPM_.addPass(InstCombinePass());
  mainFPM_.addPass(SCCPPass());
  mainFPM_.addPass(SimplifyCFGPass());
  mainFPM_.addPass(InstCombinePass());
  mainFPM_.addPass(SimplifyCFGPass());
  mainFPM_.addPass(ADCEPass());
  //   Phase 3 folds loops whose bounds became constant (a no-op on loopless
  //   functions). LoopSimplify canonicalizes; LoopFullUnroll unrolls small
  //   bounded loops; IndVarSimplify uses SCEV to replace a larger loop's
  //   accumulator phi with its closed-form exit value; LoopDeletion removes the
  //   emptied loop; Promote returns any loop-created allocas to SSA.
  mainFPM_.addPass(LoopSimplifyPass());
  {
    LoopPassManager LPM;
    LPM.addPass(LoopFullUnrollPass());
    LPM.addPass(IndVarSimplifyPass());
    LPM.addPass(LoopDeletionPass());
    mainFPM_.addPass(createFunctionToLoopPassAdaptor(std::move(LPM)));
  }
  mainFPM_.addPass(PromotePass());

  // mainFpmPgo_ - PGO (Tier-2) variant of mainFPM_ (§12 阶段2): LoopUnrollPass
  // (profile-aware partial unroll/peel via BFI/PSI) replaces LoopFullUnrollPass,
  // + PGOMemOPSizeOpt (profile-guided mem-intrinsic size specialization).
  // Baseline stays on mainFPM_ (LoopFullUnroll) so PGO is opt-in isolated.
  mainFpmPgo_.addPass(InstCombinePass());
  mainFpmPgo_.addPass(SCCPPass());
  mainFpmPgo_.addPass(SimplifyCFGPass());
  mainFpmPgo_.addPass(InstCombinePass());
  mainFpmPgo_.addPass(SimplifyCFGPass());
  mainFpmPgo_.addPass(ADCEPass());
  mainFpmPgo_.addPass(LoopSimplifyPass());
  mainFpmPgo_.addPass(LoopUnrollPass()); // FunctionPass, profile-aware (BFI/PSI)
  {
    LoopPassManager LPM;
    LPM.addPass(IndVarSimplifyPass());
    LPM.addPass(LoopDeletionPass());
    mainFpmPgo_.addPass(createFunctionToLoopPassAdaptor(std::move(LPM)));
  }
  mainFpmPgo_.addPass(PromotePass());
  mainFpmPgo_.addPass(PGOMemOPSizeOpt());

  // cleanupFPM_ — Phase 4, run after the second StructFieldPass. Unrolling turns
  // a loop-variant array access g_arr[k].field into constant-index GEPs
  // (g_arr[0]/[1]/...), which only then become substitutable. Once
  // StructFieldPass replaces them, this fold/propagate/simplify pass collapses
  // the freshly-constant values and drops what became dead — the same treatment
  // Phase 2 gave the first wave of constants.
  cleanupFPM_.addPass(InstCombinePass());
  cleanupFPM_.addPass(SCCPPass());
  cleanupFPM_.addPass(SimplifyCFGPass());
  cleanupFPM_.addPass(ADCEPass());
}

void EJitOptimizer::clearAnalyses() {
  FAM_.clear();
  LAM_.clear();
  CGAM_.clear();
  MAM_.clear();
}

void EJitOptimizer::runPipeline(Module &M,
                                const SpecializationContext &ctx) {
  EJIT_DIAG_VERBOSE("pipeline begin func=%s key=0x%016lx opt=%d dims=%zu "
                    "tier=%d module=%s",
                    ctx.fnName.c_str(), ctx.cacheKey,
                    static_cast<int>(ctx.optLevel), ctx.dimensions.size(),
                    static_cast<int>(ctx.tier), M.getName().str().c_str());
  lastCounterNames_.clear();

  // Phase 1 - specialize (common to all tiers): turn the period index and
  // every may_const field into a compile-time constant.
  preReplacePeriodIndices(M, ctx);
  runInstCombine(M);
  runStructFieldPass(M);

  if (ctx.tier == CompileTier::Instrumented) {
    // Tier-1 (PGO Gen): lightOpt then instrument. lightOpt is the shared
    // Gen/Use prefix (identical to Tier-2) so the CFG - and thus the PGO
    // hash - matches. No mainFPM_: Tier-1 is temporary, lightly optimized.
    runLightOptPipeline(M);
    ModulePassManager GenMPM;
    GenMPM.addPass(PGOInstrumentationGen(PGOInstrumentationType::FDO));
    // Tier-1 machine code is SHARED and executed concurrently by multiple cores
    // (shared taskpool). A plain __profc_* load/add/store would lose counts and
    // let Tier-2 profile synthesis read a torn value. Lower with atomic counter
    // updates (InstrProfOptions.Atomic) so each increment is an `atomicrmw add`
    // (§5). Cost is confined to the temporary Tier-1 build; the final Baseline
    // / Tier-2 (PGOUse) machine code carries no profiling instrumentation.
    InstrProfOptions InstrProfOpts;
    InstrProfOpts.Atomic = true;
    GenMPM.addPass(InstrProfilingLoweringPass(InstrProfOpts));
    GenMPM.run(M, MAM_);
    captureCounterGlobals(M);
    EJIT_DIAG_VERBOSE("pipeline done (Tier-1) func=%s key=0x%016lx counters=%zu",
                      ctx.fnName.c_str(), ctx.cacheKey, lastCounterNames_.size());
    return; // codegen; counters land in RW data
  }

  if (ctx.tier == CompileTier::PGOUse) {
    // Tier-2 (PGO Use): same prefix as Tier-1 (lightOpt) for hash match,
    // then annotate !prof from the in-memory profile, then the full
    // optimization pipeline (consumes !prof via BFI/BPI/PSI -> codegen block
    // placement). If profile synthesis failed (empty), fall back to Baseline.
    runLightOptPipeline(M);
    if (!ctx.profileData.empty()) {
      auto InMemFS = IntrusiveRefCntPtr<vfs::InMemoryFileSystem>(
          new vfs::InMemoryFileSystem());
      InMemFS->addFile("/ejit.prof", 0,
                       MemoryBuffer::getMemBufferCopy(ctx.profileData));
      ModulePassManager UseMPM;
      UseMPM.addPass(PGOInstrumentationUse(
          /*Filename=*/"/ejit.prof", /*Remap=*/"", /*IsCS=*/false,
          IntrusiveRefCntPtr<vfs::FileSystem>(InMemFS)));
      UseMPM.run(M, MAM_);
    }
    // PGO-guided inline (§12 阶段3): profile-aware inlining of callees, after
    // PGOUse set ProfileSummary so the InlineAdvisor uses the profile. Requires
    // non-pre-inlined bitcode (PASS1 PGO mode, stage 3b) to have callees to
    // inline; a no-op on already-pre-inlined bitcode.
    {
      ModulePassManager InlineMPM;
      InlineMPM.addPass(ModuleInlinerWrapperPass());
      InlineMPM.run(M, MAM_);
    }
    runOptimizationPipeline(M, ctx.optLevel, ctx.tier);
    EJIT_DIAG_VERBOSE("pipeline done (Tier-2) func=%s key=0x%016lx",
                      ctx.fnName.c_str(), ctx.cacheKey);
    return;
  }

  // Baseline (PGO off): the existing full specialization pipeline.
  runOptimizationPipeline(M, ctx.optLevel, ctx.tier);
  EJIT_DIAG_VERBOSE("pipeline done func=%s key=0x%016lx", ctx.fnName.c_str(),
                    ctx.cacheKey);
}

void EJitOptimizer::runLightOptPipeline(Module &M) {
  FunctionPassManager FPM;
  FPM.addPass(InstCombinePass());
  FPM.addPass(SimplifyCFGPass());
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      FPM.run(F, FAM_);
}

void EJitOptimizer::captureCounterGlobals(Module &M) {
  lastCounterNames_.clear();
  for (GlobalVariable &GV : M.globals()) {
    StringRef Name = GV.getName();
    bool IsProfc = Name.starts_with("__profc_");
    bool IsProfd = Name.starts_with("__profd_");
    if (!IsProfc && !IsProfd)
      continue;
    // Default InternalLinkage is invisible to ORC J->lookup (P0-3): force
    // External so the compile driver can resolve counter addresses by name.
    if (GV.hasLocalLinkage())
      GV.setLinkage(GlobalValue::ExternalLinkage);
    if (IsProfc)
      // PGOFuncName = name with the "__profc_" prefix stripped.
      lastCounterNames_.emplace_back(Name.drop_front(strlen("__profc_")).str());
  }
}

void EJitOptimizer::preReplacePeriodIndices(
    Module &M, const SpecializationContext &ctx) {
  LLVM_DEBUG(dbgs() << "ejit-optimizer: preReplacePeriodIndices, "
                    << ctx.dimensions.size() << " dim(s)\n");
  for (Function &F : M.functions()) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;

    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 3)
        continue;

      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR_IND)
        continue;

      auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
      auto *IdxC = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
      if (!PN || !IdxC)
        continue;

      unsigned argIdx = static_cast<unsigned>(IdxC->getZExtValue());
      if (argIdx >= F.arg_size())
        continue;

      for (auto &dim : ctx.dimensions) {
        if (dim.periodName == PN->getString()) {
          Argument *arg = F.getArg(argIdx);
          arg->replaceAllUsesWith(
              ConstantInt::get(arg->getType(), dim.cellIdx));
          break;
        }
      }
    }
  }
}

void EJitOptimizer::runInstCombine(Module &M) {
  FunctionPassManager FPM;
  FPM.addPass(InstCombinePass());

  for (Function &F : M.functions())
    if (!F.isDeclaration())
      FPM.run(F, FAM_);
}

void EJitOptimizer::runStructFieldPass(Module &M) {
  EJitStructFieldPass structField(registry_);
  structField.initFromModule(M);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      structField.run(F, FAM_);
}

void EJitOptimizer::runOptimizationPipeline(Module &M,
                                            OptimizationLevel level,
                                            CompileTier tier) {
  // One fixed pipeline; `level` does not affect it.
  (void)level;
  EJIT_DIAG_DEBUG("pipeline stage5: optimization pipeline module=%s",
                  M.getName().str().c_str());

  // Phases 2-3: scalar fold/propagate/simplify fixed point, then loop folding.
  // Tier-2 (PGOUse) uses mainFpmPgo_ (LoopUnroll profile-aware + PGOMemOPSizeOpt);
  // Baseline/Instrumented use mainFPM_ (LoopFullUnroll). §12 阶段2 PGO opt-in.
  FunctionPassManager &fpm =
      (tier == CompileTier::PGOUse) ? mainFpmPgo_ : mainFPM_;
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      fpm.run(F, FAM_);

  // Phase 4: unrolling exposed new constant-index array accesses
  // (g_arr[k].field → g_arr[0].field, g_arr[1].field, ...). Substitute them,
  // then fold/propagate/simplify the freshly-constant values.
  runStructFieldPass(M);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      cleanupFPM_.run(F, FAM_);
}
