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
// pipeline (PassBuilder::buildFunctionSimplificationPipeline); only the light
// cleanupFPM_ and the LowerExpect prefix are hand-added below.
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO/SCCP.h"
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
  // Use the real llvm::PassBuilder to register the FULL analysis set. The O2
  // function-simplification pipeline (GVN, CorrelatedValuePropagation, etc.)
  // needs analyses the minimal EJitPassBuilder does not register (~13 vs ~40).
  PassBuilder PB;
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

  // The standard Ox function-simplification pipeline already runs
  // profile-aware loop transforms when !prof metadata is present. Keep only
  // the additional Tier-2 memory-operation specialization here.
  pgoUseFPM_.addPass(PGOMemOPSizeOpt());

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

  if (ctx.tier == CompileTier::Instrumented) {
    // Tier-1 (PGO Gen): lightOpt then instrument. lightOpt is the shared
    // Gen/Use prefix (identical to Tier-2) so the CFG - and thus the PGO
    // hash - matches. No mainFPM_: Tier-1 is temporary, lightly optimized.
    runLightOptPipeline(M);
    ModulePassManager GenMPM;
    GenMPM.addPass(PGOInstrumentationGen(PGOInstrumentationType::FDO));
    GenMPM.addPass(InstrProfilingLoweringPass());
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
                                            ejit::OptimizationLevel level,
                                            CompileTier tier) {
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
      if (tier == CompileTier::PGOUse)
        pgoUseFPM_.run(F, FAM_);
    }

  // Phase 4: unrolling exposed new constant-index array accesses
  // (g_arr[k].field -> g_arr[0].field, g_arr[1].field, ...). Substitute them,
  // then fold/propagate/simplify the freshly-constant values.
  runStructFieldPass(M);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      cleanupFPM_.run(F, FAM_);
}
