//===-- EJitOptimizer.cpp - JIT Optimization Pipeline ---------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#include "llvm/ADT/DenseSet.h"
#endif
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
#include "llvm/ExecutionEngine/EJIT/EJitValueProfile.h"
#endif
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Debug.h"
// The post-specialization cleanup is the real LLVM -O2 function-simplification
// pipeline (PassBuilder::buildFunctionSimplificationPipeline); only the light
// cleanupFPM_ and the LowerExpect prefix are hand-added below.
#include "llvm/IR/GlobalValue.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Transforms/IPO/Inliner.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Instrumentation/InstrProfiling.h"
#include "llvm/Transforms/Instrumentation/PGOInstrumentation.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/LoopDeletion.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include <limits>
#include <optional>

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-optimizer"

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
static std::vector<EJitMayConstLoadSite>
collectMayConstSites(Module &M, PeriodArrayRegistry &Registry) {
  EJitStructFieldPass Pass(Registry);
  Pass.initFromModule(M);
  return Pass.collectMayConstLoadSites(M);
}

static uint64_t saturatingAuditAdd(uint64_t L, uint64_t R) {
  const uint64_t Max = std::numeric_limits<uint64_t>::max();
  return R > Max - L ? Max : L + R;
}

static bool mayConstSitesCorrespond(const EJitMayConstLoadSite &L,
                                    const EJitMayConstLoadSite &R) {
  if (L.hasFieldOffset && R.hasFieldOffset && !L.globalName.empty() &&
      L.globalName == R.globalName && L.fieldOffset == R.fieldOffset)
    return true;
  return L.sourceLine != 0 && R.sourceLine != 0 && !L.sourceFile.empty() &&
         L.sourceFile == R.sourceFile && L.sourceLine == R.sourceLine &&
         L.sourceColumn == R.sourceColumn;
}
#endif

EJitOptimizer::EJitOptimizer(PeriodArrayRegistry &reg, bool verifySubstitution)
    : registry_(reg), verifySubstitution_(verifySubstitution) {
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
  // expect(x,y) -> x lets the O2 pipeline fold the branch and DCE its dead
  // half.
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

  // cleanupFPM_ — Phase 4, run after the second StructFieldPass. Unrolling
  // turns a loop-variant array access g_arr[k].field into constant-index GEPs
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

void EJitOptimizer::runPipeline(Module &M, const SpecializationContext &ctx) {
  EJIT_DIAG_VERBOSE("pipeline begin func=%s key=0x%016lx opt=%d dims=%zu "
                    "tier=%d module=%s",
                    ctx.fnName.c_str(), ctx.cacheKey,
                    static_cast<int>(ctx.optLevel), ctx.dimensions.size(),
                    static_cast<int>(ctx.tier), M.getName().str().c_str());
  lastCounterNames_.clear();
  lastVpFunctions_.clear();
  scalarSiteCountsByFunc_.clear();
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  lastMayConstLoadSites_.clear();
#endif

#ifdef EJIT_VERIFY_SUBSTITUTION
  if (verifySubstitution_)
    EJIT_DIAG("verify mode: checking may_const values instead of freezing "
              "them; this specialization is NOT optimized");
#endif

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  std::vector<EJitMayConstLoadSite> AuditInputSites;
  uint64_t AuditSampledEntries = 0;
  if (ctx.tier == CompileTier::Instrumented ||
      ctx.tier == CompileTier::PGOUse) {
    EJitStructFieldPass AuditPass(registry_);
    AuditPass.initFromModule(M);
    auto Sites = AuditPass.instrumentMayConstLoadSites(M);
    if (ctx.tier == CompileTier::Instrumented)
      lastMayConstLoadSites_.append(Sites.begin(), Sites.end());
    else if (!ctx.mayConstLoadSites.empty())
      AuditInputSites = ctx.mayConstLoadSites;
    else
      AuditInputSites = std::move(Sites);
  } else if (!ctx.mayConstLoadSites.empty()) {
    AuditInputSites = ctx.mayConstLoadSites;
  } else {
    AuditInputSites = collectMayConstSites(M, registry_);
  }
#endif

  // Per-function specialization accounting for the INFO summary printed at
  // the end of runPipeline.
  SpecStatsMap Stats;

  // Phase 1 - specialize (common to all tiers): turn the period index and
  // every may_const field into a compile-time constant.
  //   (a) Substitute the ejit_period_arr_ind argument with its constant index.
  preReplacePeriodIndices(M, ctx, &Stats);
  EJIT_DIAG_DEBUG("pipeline phase1a done: preReplacePeriodIndices");
  runInstCombine(M);
  EJIT_DIAG_DEBUG("pipeline phase1b done: InstCombine");
  // Inlining is intentionally not run here: the AOT pre-optimization
  // (EJitRegisterBitcodePass: AlwaysInline + ModuleInliner(O2)) already expanded
  // callee bodies in the embedded bitcode, so their may_const GEP chains are
  // already traceable to the global.
  //   (c) Replace the may_const loads with their runtime constant values.
  runStructFieldPass(M, ctx, /*FinalRound=*/false, &Stats);
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
  runStructFieldPass(M, ctx, /*FinalRound=*/false, &Stats);
  EJIT_DIAG_DEBUG("pipeline phase1ef done: callee InstCombine+StructFieldPass");

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  const uint64_t AuditSpecializedMayConstLoads =
      ctx.tier != CompileTier::Instrumented
          ? collectMayConstSites(M, registry_).size()
          : 0;
#endif

  if (ctx.tier == CompileTier::Instrumented) {
    // The counter increment lowers to `atomicrmw add i64` (§5, InstrProfOpts
    // .Atomic). AOT bitcode compiled for generic AArch64 carries
    // "+outline-atomics" in its fn "target-features" attribute (the default
    // on Linux aarch64 toolchains), which overrides the JIT's global target
    // features and makes the backend outline the atomicrmw into an
    // __aarch64_ldadd8_relax libcall that does not exist on SRE / freestanding.
    // Replace either spelling with an explicit disable. Merely deleting
    // -outline-atomics can re-enable the target-machine default.
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      Attribute A = F.getFnAttribute("target-features");
      if (!A.isValid())
        continue;
      SmallVector<StringRef, 8> Feats;
      StringRef(A.getValueAsString()).split(Feats, ',');
      auto It = llvm::remove_if(Feats, [](StringRef S) {
        return S == "+outline-atomics" || S == "-outline-atomics";
      });
      if (It != Feats.end()) {
        Feats.erase(It, Feats.end());
        Feats.push_back("-outline-atomics");
        F.addFnAttr("target-features", join(Feats, ","));
      }
    }

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
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    // Scalar/loop-bound value sites (EJIT_VALUE_PROFILE.md §7.1): discover +
    // instrument AFTER the Gen/Lowering passes (so the CFG carries the same
    // critical-edge splits the Tier-2 Use phase will reproduce - the site
    // numbering must agree) and BEFORE captureCounterGlobals (which merges the
    // per-function site counts into the capture the compile driver reads).
    bool EnableValueProfile = true;
#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
    EnableValueProfile = !ctx.profileAuditOnly;
#endif
    if (EnableValueProfile) {
      for (Function &F : M.functions())
        if (!F.isDeclaration())
          runValueProfileOnFunction(F, FAM_, EJitValueProfileMode::Instrument,
                                    [this](StringRef name, uint32_t count) {
                                      recordScalarSiteCount(name, count);
                                    });
    }
#endif
    captureCounterGlobals(M);
    EJIT_DIAG_VERBOSE(
        "pipeline done (Tier-1) func=%s key=0x%016lx counters=%zu",
        ctx.fnName.c_str(), ctx.cacheKey, lastCounterNames_.size());
    return; // codegen; counters land in RW data
  }

  if (ctx.tier == CompileTier::PGOUse) {
    // Tier-2 (PGO Use): same prefix as Tier-1 (lightOpt) for hash match,
    // then annotate !prof from the in-memory profile, then the full
    // optimization pipeline (consumes !prof via BFI/BPI/PSI -> codegen block
    // placement). If profile synthesis failed (empty), fall back to Baseline.
    std::unique_ptr<Module> AuditModule;
    Module *ProfileModule = &M;
#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
    if (ctx.profileAuditOnly) {
      AuditModule = CloneModule(M);
      ProfileModule = AuditModule.get();
    }
#endif
    runLightOptPipeline(*ProfileModule);
    if (!ctx.profileData.empty()) {
      auto InMemFS = IntrusiveRefCntPtr<vfs::InMemoryFileSystem>(
          new vfs::InMemoryFileSystem());
      InMemFS->addFile("/ejit.prof", 0,
                       MemoryBuffer::getMemBufferCopy(ctx.profileData));
      ModulePassManager UseMPM;
      UseMPM.addPass(PGOInstrumentationUse(
          /*Filename=*/"/ejit.prof", /*Remap=*/"", /*IsCS=*/false,
          IntrusiveRefCntPtr<vfs::FileSystem>(InMemFS)));
      UseMPM.run(*ProfileModule, MAM_);
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
      auto Summaries = analyzeBranchProfiles(*ProfileModule, ctx.fnName);
      uint64_t Instructions = 0;
      uint32_t Branches = 0;
      uint32_t Profiled = 0;
      uint32_t Biased = 0;
      uint32_t Balanced = 0;
      uint32_t ZeroEdges = 0;
      uint64_t RootEntries = 0;
      for (const auto &Summary : Summaries) {
        Instructions += Summary.instructionCount;
        Branches += Summary.branchSites;
        Profiled += Summary.profiledSites;
        Biased += Summary.biasedSites95;
        Balanced += Summary.balancedSites60;
        ZeroEdges += Summary.zeroCountEdges;
        if (Summary.isRoot) {
          RootEntries = Summary.entryCount;
          AuditSampledEntries = Summary.entryCount;
        }
        EJIT_DIAG_DEBUG(
            "branch-audit-fn entry=%s fn=%s root=%u entries=%llu insts=%llu "
            "branches=%u profiled=%u biased95=%u balanced60=%u zero_edges=%u",
            ctx.fnName.c_str(), Summary.functionName.c_str(), Summary.isRoot,
            static_cast<unsigned long long>(Summary.entryCount),
            static_cast<unsigned long long>(Summary.instructionCount),
            Summary.branchSites, Summary.profiledSites, Summary.biasedSites95,
            Summary.balancedSites60, Summary.zeroCountEdges);
      }
      EJIT_DIAG(
          "branch-audit entry=%s key=0x%016llx root_entries=%llu funcs=%u "
          "insts=%llu branches=%u profiled=%u biased95=%u balanced60=%u "
          "zero_edges=%u",
          ctx.fnName.c_str(), static_cast<unsigned long long>(ctx.cacheKey),
          static_cast<unsigned long long>(RootEntries),
          static_cast<unsigned>(Summaries.size()),
          static_cast<unsigned long long>(Instructions), Branches, Profiled,
          Biased, Balanced, ZeroEdges);
#endif
    }
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
    EJitStructFieldPass::removeMayConstLoadInstrumentation(M);
#endif
#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
    if (ctx.profileAuditOnly) {
      // The clone consumed profile data solely for diagnostics. Keep the
      // publish module profile-free so audit-only mode is behaviorally the
      // same optimization pipeline as ejit_init() Baseline.
      clearAnalyses();
      runOptimizationPipeline(M, ctx.optLevel, CompileTier::Baseline);
#if defined(EJIT_DIAG_ENABLE)
      auto FinalSites = collectMayConstSites(M, registry_);
      recordMayConstBenefit(ctx, AuditInputSites, AuditSpecializedMayConstLoads,
                            FinalSites, AuditSampledEntries);
#endif
      EJIT_DIAG_VERBOSE("pipeline done (audit-only Baseline) func=%s "
                        "key=0x%016lx",
                        ctx.fnName.c_str(), ctx.cacheKey);
      return;
    }
#endif
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    // Scalar/loop-bound sites (EJIT_VALUE_PROFILE.md §7.1): annotate with
    // !ejit.vp metadata AFTER the Use phase (same post-split CFG as Tier-1's
    // discovery, so site numbering matches) and BEFORE the inliner (so the
    // metadata rides inlined instructions into their callers).
    if (!ctx.scalarValueSites.empty()) {
      for (Function &F : M.functions())
        if (!F.isDeclaration())
          runValueProfileOnFunction(F, FAM_, EJitValueProfileMode::Annotate);
    }
#endif
    // PGO-guided inline (§12 阶段3): profile-aware inlining of callees, after
    // PGOUse set ProfileSummary so the InlineAdvisor uses the profile. Requires
    // non-pre-inlined bitcode (PASS1 PGO mode, stage 3b) to have callees to
    // inline; a no-op on already-pre-inlined bitcode.
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    // Value profiling (EJIT_VALUE_PROFILE.md §6): official indirect-call
    // promotion over the !prof value metadata PGOInstrumentationUse annotated.
    // Runs before the PGO inliner so promoted hot direct calls can be inlined
    // during this same Tier-2 build.
    // A no-op on modules whose profile carries no indirect-call value data.
    {
      ModulePassManager IcpMPM;
      IcpMPM.addPass(PGOIndirectCallPromotion());
      IcpMPM.run(M, MAM_);
    }
#endif
    // Run the module inliner after ICP so the guarded direct hot call can be
    // inlined during this Tier-2 compilation.
    {
      ModulePassManager InlineMPM;
      InlineMPM.addPass(ModuleInlinerWrapperPass());
      InlineMPM.run(M, MAM_);
    }
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    // Guarded scalar/loop-bound specialization over the merged side table:
    // versions the qualifying loops (guard + cloned fallback) so the following
    // optimization pipeline sees the constant bound in the hot loop.
    if (!ctx.scalarValueSites.empty()) {
      EJitScalarValueSpecPass SpecPass(ctx.scalarValueSites);
      FunctionPassManager SpecFPM;
      SpecFPM.addPass(std::move(SpecPass));
      for (Function &F : M.functions())
        if (!F.isDeclaration())
          SpecFPM.run(F, FAM_);
    }
#endif
    runOptimizationPipeline(M, ctx.optLevel, ctx.tier);
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
    auto FinalSites = collectMayConstSites(M, registry_);
    recordMayConstBenefit(ctx, AuditInputSites, AuditSpecializedMayConstLoads,
                          FinalSites, AuditSampledEntries);
#endif
    EJIT_DIAG_VERBOSE("pipeline done (Tier-2) func=%s key=0x%016lx",
                      ctx.fnName.c_str(), ctx.cacheKey);
    return;
  }

  // Baseline (PGO off): the existing full specialization pipeline.
  runOptimizationPipeline(M, ctx.optLevel, ctx.tier, &Stats);
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  auto FinalSites = collectMayConstSites(M, registry_);
  recordMayConstBenefit(ctx, AuditInputSites, AuditSpecializedMayConstLoads,
                        FinalSites, AuditSampledEntries);
#endif
  // INFO: one summary line per function with specialization activity (or the
  // ejit_entry target). key=/func= join with the compile begin/OK/FAIL lines;
  // field semantics are documented on FuncSpecStats. Inactive auxiliary
  // callees are skipped - their all-zero lines would bury the interesting
  // functions in log noise on SRE's small ring buffer.
  for (Function &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    bool IsEntry =
        hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY);
    auto It = Stats.find(&F);
    if (It == Stats.end() && !IsEntry)
      continue;
    // Stats entries are only created together with a non-zero counter, so
    // every non-entry function in the map is active by construction.
    const FuncSpecStats &S = It != Stats.end() ? It->second : FuncSpecStats();
    EJIT_DIAG("spec summary key=0x%016lx func=%s pind_ok=%u pind_fail=%u "
              "pb_repl=%zu mc_repl=%zu mc_failed=%zu",
              ctx.cacheKey, F.getName().str().c_str(), S.PindOk, S.PindFail,
              S.PtrBaseReplaced, S.McReplaced, S.McFailedFinal);
  }
  EJIT_DIAG_VERBOSE("pipeline done func=%s key=0x%016lx", ctx.fnName.c_str(),
                    ctx.cacheKey);
}

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
void EJitOptimizer::recordMayConstBenefit(
    const SpecializationContext &ctx, ArrayRef<EJitMayConstLoadSite> InputSites,
    uint64_t SpecializedMayConstLoads,
    ArrayRef<EJitMayConstLoadSite> FinalSites, uint64_t SampledEntries) {
  const uint64_t InputMayConstLoads = InputSites.size();
  const uint64_t FinalMayConstLoads = FinalSites.size();
  uint64_t RuntimeHits = 0;
  uint64_t HitSites = 0;
  uint64_t RemovedRuntimeHits = 0;
  uint64_t RemovedHitSites = 0;
  DenseSet<uint64_t> SurvivingSiteIDs;
  SmallVector<bool, 16> UnknownFinalMatched(FinalSites.size(), false);
  for (const EJitMayConstLoadSite &Site : FinalSites)
    if (Site.siteId != 0)
      SurvivingSiteIDs.insert(Site.siteId);
  for (const EJitMayConstLoadSite &Site : InputSites) {
    RuntimeHits = saturatingAuditAdd(RuntimeHits, Site.runtimeHits);
    HitSites += Site.runtimeHits != 0;
    // A surviving clone keeps the original ID, so count a site as removed only
    // when every clone disappeared. If a transform rebuilt a load without
    // copying metadata, consume one matching field/source identity before
    // claiming removal. Completely unknown provenance is kept conservatively.
    bool Survives = Site.siteId == 0 || SurvivingSiteIDs.contains(Site.siteId);
    if (!Survives) {
      for (size_t I = 0; I < FinalSites.size(); ++I) {
        if (FinalSites[I].siteId != 0 || UnknownFinalMatched[I] ||
            !mayConstSitesCorrespond(Site, FinalSites[I]))
          continue;
        UnknownFinalMatched[I] = true;
        Survives = true;
        break;
      }
    }
    if (!Survives) {
      RemovedRuntimeHits =
          saturatingAuditAdd(RemovedRuntimeHits, Site.runtimeHits);
      RemovedHitSites += Site.runtimeHits != 0;
    }
  }

  EJitMayConstBenefitSample Sample;
  Sample.cacheKey = ctx.cacheKey;
  Sample.inputMayConstLoads = InputMayConstLoads;
  Sample.specializedMayConstLoads = SpecializedMayConstLoads;
  Sample.finalMayConstLoads = FinalMayConstLoads;
  Sample.runtimeHits = RuntimeHits;
  Sample.hitSites = HitSites;
  Sample.removedRuntimeHits = RemovedRuntimeHits;
  Sample.removedHitSites = RemovedHitSites;
  Sample.sampledEntries = SampledEntries;
  Sample.sampleCycles = ctx.mayConstSampleCycles;

  uint32_t Expected = 0;
  while (!mayConstBenefitLock_.compareExchange(Expected, 1))
    Expected = 0;
  auto &ByKey = mayConstBenefitSamples_[ctx.fnName];
  ByKey[ctx.cacheKey] = Sample;
  SmallVector<EJitMayConstBenefitSample, 16> Samples;
  Samples.reserve(ByKey.size());
  for (const auto &Entry : ByKey)
    Samples.push_back(Entry.second);
  mayConstBenefitLock_.storeRelease(0);

  EJitMayConstBenefitSummary Aggregate = summarizeMayConstBenefits(Samples);
  const int64_t Removed = static_cast<int64_t>(InputMayConstLoads) -
                          static_cast<int64_t>(FinalMayConstLoads);
  const int64_t Direct = static_cast<int64_t>(InputMayConstLoads) -
                         static_cast<int64_t>(SpecializedMayConstLoads);
  const int64_t Pipeline = static_cast<int64_t>(SpecializedMayConstLoads) -
                           static_cast<int64_t>(FinalMayConstLoads);
  EJIT_DIAG("mayconst-audit entry=%s key=0x%016llx tier=%u versions=%llu "
            "mayconst=%llu/%llu/%llu removed=%lld direct=%lld pipeline=%lld "
            "runtime_hits=%llu hit_sites=%llu removed_runtime_hits=%llu "
            "removed_hit_sites=%llu sampled_entries=%llu sample_cycles=%llu "
            "avg_removed=%lld "
            "weighted_permille=%lld min=%lld max=%lld",
            ctx.fnName.c_str(), static_cast<unsigned long long>(ctx.cacheKey),
            static_cast<unsigned>(ctx.tier),
            static_cast<unsigned long long>(Aggregate.versions),
            static_cast<unsigned long long>(InputMayConstLoads),
            static_cast<unsigned long long>(SpecializedMayConstLoads),
            static_cast<unsigned long long>(FinalMayConstLoads),
            static_cast<long long>(Removed), static_cast<long long>(Direct),
            static_cast<long long>(Pipeline),
            static_cast<unsigned long long>(RuntimeHits),
            static_cast<unsigned long long>(HitSites),
            static_cast<unsigned long long>(RemovedRuntimeHits),
            static_cast<unsigned long long>(RemovedHitSites),
            static_cast<unsigned long long>(SampledEntries),
            static_cast<unsigned long long>(ctx.mayConstSampleCycles),
            static_cast<long long>(Aggregate.averageRemoved),
            static_cast<long long>(Aggregate.weightedRemovedPermille),
            static_cast<long long>(Aggregate.minimumRemoved),
            static_cast<long long>(Aggregate.maximumRemoved));

  for (const EJitMayConstLoadSite &Site : InputSites) {
    if (Site.hasFieldOffset)
      EJIT_DIAG_DEBUG(
          "mayconst-site entry=%s fn=%s gv=%s offset=%llu hits=%llu "
          "src=%s:%u:%u",
          ctx.fnName.c_str(), Site.functionName.c_str(),
          Site.globalName.empty() ? "<unknown>" : Site.globalName.c_str(),
          static_cast<unsigned long long>(Site.fieldOffset),
          static_cast<unsigned long long>(Site.runtimeHits),
          Site.sourceFile.empty() ? "<unknown>" : Site.sourceFile.c_str(),
          Site.sourceLine, Site.sourceColumn);
    else
      EJIT_DIAG_DEBUG(
          "mayconst-site entry=%s fn=%s gv=%s offset=unknown hits=%llu "
          "src=%s:%u:%u",
          ctx.fnName.c_str(), Site.functionName.c_str(),
          Site.globalName.empty() ? "<unknown>" : Site.globalName.c_str(),
          static_cast<unsigned long long>(Site.runtimeHits),
          Site.sourceFile.empty() ? "<unknown>" : Site.sourceFile.c_str(),
          Site.sourceLine, Site.sourceColumn);
  }
}
#endif

bool EJitOptimizer::recordMayConstPublishedCode(StringRef Entry,
                                                uint64_t CacheKey,
                                                const void *CodeStart,
                                                uint64_t CodeBytes) {
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  const auto *Bytes = static_cast<const uint8_t *>(CodeStart);
  std::vector<uint64_t> Fingerprints;
  if (Bytes && CodeBytes <= std::numeric_limits<size_t>::max())
    Fingerprints = fingerprintPublishedHotICacheLines(
        ArrayRef<uint8_t>(Bytes, static_cast<size_t>(CodeBytes)));

  uint32_t Expected = 0;
  while (!mayConstBenefitLock_.compareExchange(Expected, 1))
    Expected = 0;
  auto EntryIt = mayConstBenefitSamples_.find(Entry);
  if (EntryIt == mayConstBenefitSamples_.end()) {
    mayConstBenefitLock_.storeRelease(0);
    return false;
  }
  auto VersionIt = EntryIt->second.find(CacheKey);
  if (VersionIt == EntryIt->second.end()) {
    mayConstBenefitLock_.storeRelease(0);
    return false;
  }
  VersionIt->second.publishedHotCodeBytes = CodeBytes;
  VersionIt->second.publishedHotLineFingerprints.assign(Fingerprints.begin(),
                                                        Fingerprints.end());
  mayConstBenefitLock_.storeRelease(0);
  return true;
#else
  (void)Entry;
  (void)CacheKey;
  (void)CodeStart;
  (void)CodeBytes;
  return false;
#endif
}

bool EJitOptimizer::printMayConstRanking() const {
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  struct RankingRow {
    std::string entry;
    EJitMayConstBenefitSummary summary;
  };
  SmallVector<RankingRow, 32> Rows;

  uint32_t Expected = 0;
  while (!mayConstBenefitLock_.compareExchange(Expected, 1))
    Expected = 0;
  Rows.reserve(mayConstBenefitSamples_.size());
  for (const auto &Entry : mayConstBenefitSamples_) {
    SmallVector<EJitMayConstBenefitSample, 16> Samples;
    Samples.reserve(Entry.second.size());
    for (const auto &Version : Entry.second)
      Samples.push_back(Version.second);
    Rows.push_back({Entry.first().str(), summarizeMayConstBenefits(Samples)});
  }
  mayConstBenefitLock_.storeRelease(0);

  if (Rows.empty()) {
    EJIT_DIAG_RAW("mayconst-ranking: no completed samples");
    return false;
  }
  llvm::sort(Rows, [](const RankingRow &L, const RankingRow &R) {
    if (L.summary.benefitPerMillionCyclesMilli !=
        R.summary.benefitPerMillionCyclesMilli)
      return L.summary.benefitPerMillionCyclesMilli >
             R.summary.benefitPerMillionCyclesMilli;
    if (L.summary.removedHitsPerEntryPermille !=
        R.summary.removedHitsPerEntryPermille)
      return L.summary.removedHitsPerEntryPermille >
             R.summary.removedHitsPerEntryPermille;
    if (L.summary.averageActiveSitesPermille !=
        R.summary.averageActiveSitesPermille)
      return L.summary.averageActiveSitesPermille >
             R.summary.averageActiveSitesPermille;
    if (L.summary.runtimeHits != R.summary.runtimeHits)
      return L.summary.runtimeHits > R.summary.runtimeHits;
    if (L.summary.averageRemoved != R.summary.averageRemoved)
      return L.summary.averageRemoved > R.summary.averageRemoved;
    return L.entry < R.entry;
  });

  EJIT_DIAG_RAW("mayconst-ranking entries=%u sort=benefit_per_mcycle_desc",
                static_cast<unsigned>(Rows.size()));
  for (size_t I = 0; I < Rows.size(); ++I) {
    const RankingRow &Row = Rows[I];
    const uint64_t AvgActiveSites =
        Row.summary.averageActiveSitesPermille;
    const uint64_t RemovedPerEntry =
        Row.summary.removedHitsPerEntryPermille;
    const uint64_t Benefit = Row.summary.benefitPerMillionCyclesMilli;
    const uint64_t Density = Row.summary.entryBenefitDensityMilli;
    const uint64_t PartialJitRatio =
        Row.summary.partialJitCandidatePermille;
    EJIT_DIAG_RAW(
        "rank=%u entry=%s versions=%llu benefit_per_mcycle=%llu.%03llu "
        "entry_benefit_density=%llu.%03llu hot_code_bytes=%llu "
        "hot_icache_lines=%llu fingerprinted_lines=%llu "
        "cross_version_matching_lines=%llu partial_jit_candidate_lines=%llu "
        "partial_jit_candidate_ratio=%llu.%01llu%% "
        "removed_per_entry=%llu.%03llu removed_runtime_hits=%llu "
        "sampled_entries=%llu sample_cycles=%llu avg_active_sites=%llu.%03llu "
        "hit_sites=%llu runtime_hits=%llu avg_removed=%lld "
        "total_removed=%lld min=%lld max=%lld",
        static_cast<unsigned>(I + 1), Row.entry.c_str(),
        static_cast<unsigned long long>(Row.summary.versions),
        static_cast<unsigned long long>(Benefit / 1000),
        static_cast<unsigned long long>(Benefit % 1000),
        static_cast<unsigned long long>(Density / 1000),
        static_cast<unsigned long long>(Density % 1000),
        static_cast<unsigned long long>(Row.summary.publishedHotCodeBytes),
        static_cast<unsigned long long>(Row.summary.publishedHotICacheLines),
        static_cast<unsigned long long>(
            Row.summary.fingerprintedHotICacheLines),
        static_cast<unsigned long long>(
            Row.summary.crossVersionMatchingICacheLines),
        static_cast<unsigned long long>(
            Row.summary.partialJitCandidateICacheLines),
        static_cast<unsigned long long>(PartialJitRatio / 10),
        static_cast<unsigned long long>(PartialJitRatio % 10),
        static_cast<unsigned long long>(RemovedPerEntry / 1000),
        static_cast<unsigned long long>(RemovedPerEntry % 1000),
        static_cast<unsigned long long>(Row.summary.removedRuntimeHits),
        static_cast<unsigned long long>(Row.summary.sampledEntries),
        static_cast<unsigned long long>(Row.summary.sampleCycles),
        static_cast<unsigned long long>(AvgActiveSites / 1000),
        static_cast<unsigned long long>(AvgActiveSites % 1000),
        static_cast<unsigned long long>(Row.summary.hitSites),
        static_cast<unsigned long long>(Row.summary.runtimeHits),
        static_cast<long long>(Row.summary.averageRemoved),
        static_cast<long long>(Row.summary.totalRemoved),
        static_cast<long long>(Row.summary.minimumRemoved),
        static_cast<long long>(Row.summary.maximumRemoved));
    ejitDiagPrintThrottle();
  }
  return true;
#else
  EJIT_DIAG_RAW("mayconst-ranking: branch audit not enabled in this build");
  return false;
#endif
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

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  // Value-profile capture (EJIT_VALUE_PROFILE.md §5.1): every function of the
  // module with its IR-PGO-name MD5 - the SAME hash the Tier-2 module symtab
  // (InstrProfSymtab::create) keys targets by, so IndirectCallPromotion can
  // resolve the merged profile values. Declarations are captured too: they
  // are potential external call targets the driver may resolve through the
  // registered user symbols.
  //
  // Also re-export every defined function: phase 1 internalized them (for
  // IPSCCP), but the ORC claims made at addIRModule came from the module as
  // loaded - where the Instrumented tier had already exported them
  // (EJitOrcEngine::loadBitcodeModule) - and a claimed symbol that the
  // emitted object defines only as a LOCAL would link as a null absolute.
  // Re-externalizing here (after the pipeline, nothing optimizes afterwards)
  // makes the emitted symbol GLOBAL again so the driver's ORC lookup of
  // internal targets resolves their real addresses.
  for (Function &F : M.functions()) {
    if (F.isIntrinsic() || !F.hasName() || F.getName().empty())
      continue;
    EJitVpFunctionInfo Info;
    Info.name = F.getName().str();
    Info.pgoHash = IndexedInstrProf::ComputeHash(getIRPGOFuncName(F));
    auto it = scalarSiteCountsByFunc_.find(Info.name);
    if (it != scalarSiteCountsByFunc_.end())
      Info.numScalarSites = it->second;
    lastVpFunctions_.push_back(std::move(Info));
    if (!F.isDeclaration() && F.hasLocalLinkage()) {
      // The IR verifier requires default visibility with external linkage;
      // phase 1 already set default visibility when internalizing.
      F.setVisibility(GlobalValue::DefaultVisibility);
      F.setLinkage(GlobalValue::ExternalLinkage);
    }
  }
#endif
}

void EJitOptimizer::preReplacePeriodIndices(
    Module &M, const SpecializationContext &ctx, SpecStatsMap *Stats) {
  LLVM_DEBUG(dbgs() << "ejit-optimizer: preReplacePeriodIndices, "
                    << ctx.dimensions.size() << " dim(s)\n");
  for (Function &F : M.functions()) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;

    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 1)
        continue;

      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR_IND)
        continue;

      // From here on the entry is a period-index substitution candidate. A
      // failure means the argument stays a runtime value - specialization
      // silently lost - so every failure path (including a tagged entry
      // truncated below three operands) logs at INFO.
      FuncSpecStats *FS = Stats ? &(*Stats)[&F] : nullptr;
      if (Sub->getNumOperands() < 3) {
        if (FS)
          ++FS->PindFail;
        EJIT_DIAG("period_arr_ind replace FAIL func=%s: malformed metadata "
                  "entry",
                  F.getName().str().c_str());
        continue;
      }
      auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
      auto *IdxC = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
      if (!PN || !IdxC) {
        if (FS)
          ++FS->PindFail;
        EJIT_DIAG("period_arr_ind replace FAIL func=%s: malformed metadata "
                  "entry",
                  F.getName().str().c_str());
        continue;
      }

      unsigned argIdx = static_cast<unsigned>(IdxC->getZExtValue());
      if (argIdx >= F.arg_size()) {
        if (FS)
          ++FS->PindFail;
        EJIT_DIAG("period_arr_ind replace FAIL func=%s: arg index %u out of "
                  "range (nargs=%u)",
                  F.getName().str().c_str(), argIdx,
                  static_cast<unsigned>(F.arg_size()));
        continue;
      }

      const SpecializationContext::DimInfo *Match = nullptr;
      for (const auto &dim : ctx.dimensions)
        if (dim.periodName == PN->getString()) {
          Match = &dim;
          break;
        }
      if (!Match) {
        if (FS)
          ++FS->PindFail;
        EJIT_DIAG("period_arr_ind replace FAIL func=%s: period '%s' not in "
                  "compile context",
                  F.getName().str().c_str(), PN->getString().str().c_str());
        continue;
      }
      if (FS)
        ++FS->PindOk;
      Argument *arg = F.getArg(argIdx);
      arg->replaceAllUsesWith(ConstantInt::get(arg->getType(), Match->cellIdx));
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

void EJitOptimizer::runStructFieldPass(Module &M,
                                       const SpecializationContext &ctx,
                                       bool FinalRound, SpecStatsMap *Stats) {
  SmallVector<EJitBoundPointerView, kEJitMaxBoundPointers> BoundPointers =
      ctx.boundPointers;
  if (!BoundPointers.empty()) {
    Function *Root = M.getFunction(ctx.fnName);
    if (Root) {
      for (EJitBoundPointerView &View : BoundPointers) {
        MDNode *MD = Root->getMetadata(MD_EJIT_METADATA);
        StringRef BoundPeriodName;
        if (MD)
          for (const MDOperand &Op : MD->operands()) {
            auto *Sub = dyn_cast<MDNode>(Op.get());
            if (!Sub || Sub->getNumOperands() < 3)
              continue;
            auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
            auto *Period = dyn_cast<MDString>(Sub->getOperand(1));
            auto *Index = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
            if (Tag && Period && Index &&
                Tag->getString() == TAG_EJIT_BOUND_PTR &&
                Index->getZExtValue() == View.argIndex) {
              BoundPeriodName = Period->getString();
              break;
            }
          }
        for (const auto &Dim : ctx.dimensions)
          if (Dim.periodName == BoundPeriodName) {
            View.periodInstance = Dim.cellIdx;
            break;
          }
      }
    }
  }
  EJitStructFieldPass structField(registry_, BoundPointers, ctx.fnName,
                                  verifySubstitution_, FinalRound);
  structField.initFromModule(M);
  for (Function &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    structField.run(F, FAM_);
    if (!Stats)
      continue;
    const EJitStructFieldPass::RunStats &RS = structField.lastStats();
    if (!RS.MayConstReplaced && !RS.PtrBaseReplaced &&
        !(FinalRound && RS.MayConstLoads))
      continue;
    FuncSpecStats &S = (*Stats)[&F];
    S.McReplaced += RS.MayConstReplaced;
    S.PtrBaseReplaced += RS.PtrBaseReplaced;
    if (FinalRound)
      S.McFailedFinal = RS.MayConstLoads - RS.MayConstReplaced;
  }
}

void EJitOptimizer::runStructFieldPass(Module &M) {
  SpecializationContext Empty;
  runStructFieldPass(M, Empty);
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
                                            CompileTier tier,
                                            SpecStatsMap *Stats) {
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
  // Final StructFieldPass round: whatever this round cannot replace is a
  // final specialization loss, so per-load failures log at INFO from here.
  runStructFieldPass(M, SpecializationContext(), /*FinalRound=*/true, Stats);
  for (Function &F : M.functions())
    if (!F.isDeclaration())
      cleanupFPM_.run(F, FAM_);
}
