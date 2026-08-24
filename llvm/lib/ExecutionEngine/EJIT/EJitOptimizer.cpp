//===-- EJitOptimizer.cpp - JIT Optimization Pipeline ---------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Debug.h"
// The post-specialization cleanup is the real LLVM -O2 function-simplification
// pipeline (PassBuilder::buildFunctionSimplificationPipeline); hand-added on
// top of it are the LowerExpect prefix, the light cleanupFPM_, the Phase 1g
// module cleanup (RPO attrs + DAE + GlobalDCE), and the Phase 5 vectorization
// FPMs (SLP + partial unroll at L2, loop vectorizer at L3).
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO/DeadArgumentElimination.h"
#include "llvm/Transforms/IPO/FunctionAttrs.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/CorrelatedValuePropagation.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/Transforms/Scalar/LoopLoadElimination.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopUnrollAndJamPass.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"
#include "llvm/Transforms/Vectorize/SLPVectorizer.h"
#include "llvm/Transforms/Vectorize/VectorCombine.h"

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-optimizer"

/// Build the post-specialization vectorization FPM. Mirrors the non-LTO path
/// of PassBuilder::addVectorPasses (PassBuilderPipelines.cpp): loop
/// vectorization (L3 only), SLP for parallel scalar chains, trip-count-based
/// partial unrolling + unroll-and-jam to hide backedge latency, and a late
/// LICM. SpeedupLevel feeds the unroll cost model (2 = O2, 3 = O3).
///
/// Deliberate deviations from the host path, all compile-time tradeoffs:
/// SLP runs unconditionally at L2+ (the host gates it on a cl::opt that has
/// no meaning for an embedded runtime); the aggressive SimplifyCFG before
/// SLP, WarnMissedTransformations, InferAlignment, AlignmentFromAssumptions
/// and the unswitch/LICM part of the ExtraVectorizerPasses block are skipped
/// (LICM still runs at the end of this FPM).
static FunctionPassManager
buildVectorizeFPM(const PipelineTuningOptions &PTO, unsigned SpeedupLevel,
                  bool EnableLoopVectorize) {
  FunctionPassManager FPM;
  if (EnableLoopVectorize) {
    FPM.addPass(LoopVectorizePass(LoopVectorizeOptions()));
    // Eliminate loads by forwarding stores from the previous iteration.
    FPM.addPass(LoopLoadEliminationPass());
    FPM.addPass(InstCombinePass());
    // Scalar part of the host's ExtraVectorizerPasses block: fold and hoist
    // the runtime alignment/overlap checks the vectorizer inserted.
    FPM.addPass(EarlyCSEPass());
    FPM.addPass(CorrelatedValuePropagationPass());
    FPM.addPass(InstCombinePass());
  }
  // Optimize parallel scalar instruction chains into SIMD instructions.
  FPM.addPass(SLPVectorizerPass());
  FPM.addPass(VectorCombinePass());
  FPM.addPass(InstCombinePass());
  // Partial unrolling: LoopFullUnroll (inside the simplification pipeline)
  // only fully unrolls; partial trip-count unrolling hides backedge latency.
  // UnrollAndJam first, as in the host pipeline.
  FPM.addPass(createFunctionToLoopPassAdaptor(
      LoopUnrollAndJamPass(static_cast<int>(SpeedupLevel))));
  FPM.addPass(LoopUnrollPass(LoopUnrollOptions(static_cast<int>(SpeedupLevel),
                                               /*OnlyWhenForced=*/false,
                                               /*ForgetSCEV=*/false)));
  // Unrolling can turn variable-offset GEPs into alloca into constant-offset
  // ones; promote them. PreserveCFG: no CFG cleanup is scheduled after us.
  FPM.addPass(SROAPass(SROAOptions::PreserveCFG));
  FPM.addPass(InstCombinePass());
  // Late LICM after the unroll/vectorize cleanup, as in the host pipeline.
  // Caps come from PipelineTuningOptions, exactly as the host uses them.
  FPM.addPass(createFunctionToLoopPassAdaptor(
      LICMPass(PTO.LicmMssaOptCap, PTO.LicmMssaNoAccForPromotionCap,
               /*AllowSpeculation=*/true),
      /*UseMemorySSA=*/true, /*UseBlockFrequencyInfo=*/false));
  return FPM;
}

EJitOptimizer::EJitOptimizer(PeriodArrayRegistry &reg, TargetMachine *TM)
    : registry_(reg) {
  // Use the real llvm::PassBuilder to register the FULL analysis set. The O2
  // function-simplification pipeline (GVN, CorrelatedValuePropagation, etc.)
  // needs analyses the minimal EJitPassBuilder does not register (~13 vs ~40).
  // With a TargetMachine (the engine passes the same JTMB-derived TM the JIT
  // compiles with), TargetIRAnalysis is backend-accurate — real vector
  // register widths, so Phase 5 vectorization can fire. Without one it falls
  // back to the 32-bit baseline TTI, which disables the vectorizers (and
  // skews the loop-unroll cost models).
  PassBuilder PB(TM);
  PB.registerFunctionAnalyses(FAM_);
  PB.registerLoopAnalyses(LAM_);
  PB.registerCGSCCAnalyses(CGAM_);
  PB.registerModuleAnalyses(MAM_);
  PB.crossRegisterProxies(LAM_, FAM_, CGAM_, MAM_);

  // Pre-build the cached FunctionPassManagers, reused across compilations.
  //
  // By the time these run, runPipeline has turned the period-index argument and
  // every may_const field into a compile-time constant. The post-specialization
  // cleanup is the real LLVM -O2 function-simplification pipeline (SROA,
  // InstCombine, SCCP, GVN, CorrelatedValuePropagation, BDCE, DSE, loop-rotate,
  // LICM, loop-unroll, ... - NO vectorization), which exploits those constants
  // far more aggressively than the old hand-rolled 8-pass sequence. One cached
  // FPM per tier; runOptimizationPipeline picks by ctx.optLevel.
  //
  // LowerExpectIntrinsic must run FIRST: it is NOT in
  // buildFunctionSimplificationPipeline, and the AOT IR carries llvm.expect
  // guards (__builtin_expect / LIKELY) on top of may_const conditions. Once
  // StructFieldPass (phase 1c) turns those conditions into constants, lowering
  // expect(x,y) -> x lets the O2 pipeline fold the branch and DCE its dead half.
  lowerExpectFPM_.addPass(LowerExpectIntrinsicPass());

  simplifyO1_ = PB.buildFunctionSimplificationPipeline(
      llvm::OptimizationLevel::O1, ThinOrFullLTOPhase::None);
  simplifyO2_ = PB.buildFunctionSimplificationPipeline(
      llvm::OptimizationLevel::O2, ThinOrFullLTOPhase::None);
  simplifyO3_ = PB.buildFunctionSimplificationPipeline(
      llvm::OptimizationLevel::O3, ThinOrFullLTOPhase::None);

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

  // cleanupMPM_ — Phase 1g, module-level cleanup of the specialized IR, run on
  // every tier. RPO attribute inference feeds downstream folds, DAE drops
  // parameters IPSCCP replaced with constants (DAE only rewrites
  // local-linkage functions, so the external ejit_entry is untouched), and
  // GlobalDCE deletes callees whose call sites the folded guards deleted —
  // shrinking what the JIT backend must compile.
  cleanupMPM_.addPass(ReversePostOrderFunctionAttrsPass());
  cleanupMPM_.addPass(DeadArgumentEliminationPass());
  cleanupMPM_.addPass(GlobalDCEPass());

  // vectorizeL2/L3_ — Phase 5, post-specialization vectorization. Runs after
  // the final StructFieldPass so the vectorizers see the fully-specialized
  // loops (Phase 3 unrolling exposed constant-index accesses that Phase 4
  // substituted). L2: SLP + partial unrolling; L3 adds the loop vectorizer.
  // The Phase 5 unrolling itself runs after that last substitution: safe,
  // because it only creates variable-index GEPs for runtime-trip-count loops
  // (constant-trip loops were already fully unrolled in Phase 3), and
  // vectorizer-synthesized loads never carry !ejit.may_const.
  PipelineTuningOptions PTO;
  vectorizeL2_ = buildVectorizeFPM(PTO, /*SpeedupLevel=*/2,
                                   /*EnableLoopVectorize=*/false);
  vectorizeL3_ = buildVectorizeFPM(PTO, /*SpeedupLevel=*/3,
                                   /*EnableLoopVectorize=*/true);
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
                    "module=%s",
                    ctx.fnName.c_str(), ctx.cacheKey,
                    static_cast<int>(ctx.optLevel), ctx.dimensions.size(),
                    M.getName().str().c_str());

  // Phase 1 — specialize: turn the period index and every may_const field into
  // a compile-time constant.
  //   (a) Substitute the ejit_period_arr_ind argument with its constant index.
  preReplacePeriodIndices(M, ctx);
  EJIT_DIAG_DEBUG("pipeline phase1a done: preReplacePeriodIndices");
  //   (b) Fold the constant GEP chains that exposes so StructFieldPass can
  //       compute the byte offset of each may_const field access.
  runInstCombine(M);
  EJIT_DIAG_DEBUG("pipeline phase1b done: InstCombine");
  // Inlining is intentionally not run here: the AOT pre-optimization
  // (EJitRegisterBitcodePass: AlwaysInline + ModuleInliner(O2)) already expanded
  // callee bodies in the embedded bitcode, so their may_const GEP chains are
  // already traceable to the global.
  //   (c) Replace the may_const loads with their runtime constant values.
  runStructFieldPass(M);
  EJIT_DIAG_DEBUG("pipeline phase1c done: StructFieldPass");
  //   (d) Push the constants across call edges. Wherever the AOT inliner kept
  //       a call, the callee still re-derives cell addressing and re-tests
  //       guards from its arguments — which phases 1a-1c just made constant at
  //       every call site. IPSCCP propagates them into the callee bodies (and
  //       constant returns back out).
  runInterproceduralPropagation(M);
  EJIT_DIAG_DEBUG("pipeline phase1d done: IPSCCP");
  //   (e,f) The propagated arguments expose callee GEP chains exactly as 1a
  //       did for the entry: fold them, then substitute the callee may_const
  //       loads they root, so phases 2-4 exploit the constants in every
  //       function, not just the entry.
  runInstCombine(M);
  runStructFieldPass(M);
  EJIT_DIAG_DEBUG("pipeline phase1ef done: callee InstCombine+StructFieldPass");
  //   (g) Module-level cleanup: RPO attrs, dead-argument elimination, and
  //       GlobalDCE shrink the specialized module before the expensive
  //       per-function phases run on it.
  runModuleCleanup(M);
  EJIT_DIAG_DEBUG("pipeline phase1g done: module cleanup (RPO attrs + DAE + GlobalDCE)");

  // Phases 2-5 — exploit those constants (scalar fixed point → loops →
  // re-specialize → cleanup → vectorize). ctx.optLevel selects the
  // function-simplification tier and gates Phase 5 vectorization (L2+).
  runOptimizationPipeline(M, ctx.optLevel);
  EJIT_DIAG_VERBOSE("pipeline done func=%s key=0x%016lx", ctx.fnName.c_str(),
                    ctx.cacheKey);
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

void EJitOptimizer::runInterproceduralPropagation(Module &M) {
  // IPSCCP only reasons about a function's arguments when it can enumerate
  // every call site: local linkage and no address-taken uses. The
  // specialization module is self-contained — the JIT looks up only the
  // ejit_entry symbols; every other definition is called module-internally and
  // external references resolve to AOT symbols — so all non-entry definitions
  // can be internalized. (Address-taken callees are internalized too; IPSCCP
  // itself skips their arguments, and their symbols are still resolved
  // module-internally.)
  for (Function &F : M.functions()) {
    if (F.isDeclaration() || F.hasLocalLinkage())
      continue;
    if (hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY))
      continue;
    // The IR verifier requires default visibility with local linkage.
    F.setVisibility(GlobalValue::DefaultVisibility);
    F.setLinkage(GlobalValue::InternalLinkage);
  }

  ModulePassManager MPM;
  MPM.addPass(IPSCCPPass());
  MPM.run(M, MAM_);
}

void EJitOptimizer::runStructFieldPass(Module &M) {
  EJitStructFieldPass structField(registry_);
  structField.initFromModule(M);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      structField.run(F, FAM_);
}

FunctionPassManager &
EJitOptimizer::simplifyFPMForLevel(ejit::OptimizationLevel level) {
  switch (level) {
  case ejit::OptimizationLevel::L1:
    return simplifyO1_;
  case ejit::OptimizationLevel::L3:
    return simplifyO3_;
  case ejit::OptimizationLevel::L2:
  default:
    return simplifyO2_;
  }
}

void EJitOptimizer::runOptimizationPipeline(Module &M,
                                            ejit::OptimizationLevel level) {
  EJIT_DIAG_DEBUG("pipeline stage5: optimization pipeline module=%s opt=%d",
                  M.getName().str().c_str(), static_cast<int>(level));

  // Phase 2: lower llvm.expect (not in buildFunctionSimplificationPipeline).
  // Phase 3: the real LLVM -Ox function-simplification pipeline, selected by
  // level (L1->O1, L2->O2, L3->O3). It folds the substituted constants, DCEs
  // the dead branches, and unrolls loops whose bounds became constant; this is
  // the bulk of the post-specialization optimization.
  FunctionPassManager &simplifyFPM = simplifyFPMForLevel(level);
  for (Function &F : M.functions())
    if (!F.isDeclaration()) {
      lowerExpectFPM_.run(F, FAM_);
      simplifyFPM.run(F, FAM_);
    }

  // Phase 4: unrolling exposed new constant-index array accesses
  // (g_arr[k].field -> g_arr[0].field, g_arr[1].field, ...). Substitute them,
  // then fold/propagate/simplify the freshly-constant values.
  runStructFieldPass(M);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      cleanupFPM_.run(F, FAM_);

  // Phase 5: vectorization + partial unrolling, L2+. Runs after the final
  // StructFieldPass so the vectorizers see the fully-specialized loops. L1
  // skips vectorization entirely (matching clang -O1).
  if (static_cast<int>(level) >=
      static_cast<int>(ejit::OptimizationLevel::L2)) {
    runVectorization(M, level);
    EJIT_DIAG_DEBUG("pipeline phase5 done: vectorization opt=%d",
                    static_cast<int>(level));
  }
}

void EJitOptimizer::runModuleCleanup(Module &M) {
  cleanupMPM_.run(M, MAM_);
}

void EJitOptimizer::runVectorization(Module &M, ejit::OptimizationLevel level) {
  FunctionPassManager &FPM =
      (level == ejit::OptimizationLevel::L3) ? vectorizeL3_ : vectorizeL2_;
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      FPM.run(F, FAM_);
}

