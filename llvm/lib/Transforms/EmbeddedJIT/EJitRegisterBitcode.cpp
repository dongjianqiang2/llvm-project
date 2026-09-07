//===-- EJitRegisterBitcode.cpp - EmbeddedJIT Bitcode Extraction ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/Format.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"
#include "llvm/Support/Path.h"
#include <cctype>

using namespace llvm;
using namespace llvm::ejit;

extern cl::opt<bool> EnableEJitGlobalCtors;
extern cl::opt<std::string> EJitDumpBitcodeDir;
extern cl::opt<bool> EJitWarnNoSpecialization;
extern cl::opt<bool> EJitWarnUnusedDim;
extern cl::opt<bool> EJitReportMayConst;
extern cl::opt<unsigned> EJitWarnFewMayConst;
extern cl::opt<unsigned> EJitExternalizeMinInsts;

#define DEBUG_TYPE "ejit-register-bitcode"

/// Deterministic process-wide unique registration key for an internal
/// closure helper. Internal names are unique per module only — two TUs can
/// both define `static int helper()`. Prefix the sanitized module BASENAME
/// so the runtime's flat symbol table never binds a JIT'd helper to the
/// wrong TU, plus the full 64-bit hash of the RAW module path: the sanitized
/// basename alone can collide (`a-b.c` vs `a_b.c` both sanitize to `a_b_c`),
/// and distinct build variants of one path (-D variations) are not
/// distinguished (see the design doc threat model). The rename and the
/// registration are generated in the same pass run, so run-local
/// determinism is all the key requires. The extracted-bitcode rename and
/// the AOT-side registration both derive the key from this one function.
static std::string ejitStaticHelperKey(StringRef ModuleName,
                                       StringRef FuncName) {
  std::string Key = "ejit_static.";
  for (char C : sys::path::filename(ModuleName))
    Key.push_back(isalnum(static_cast<unsigned char>(C)) || C == '_' ? C : '_');
  Key += '.';
  raw_string_ostream OS(Key);
  OS << format_hex(static_cast<uint64_t>(hash_value(ModuleName)), 16,
                   /*Upper=*/false);
  OS.flush();
  Key += '.';
  Key += FuncName;
  return Key;
}

/// Registration key for an externalized closure helper: internal helpers
/// get their deterministic unique key, externally-linked helpers keep their
/// (already process-unique) name. Single source of truth shared by the
/// extracted-bitcode rename and both registration emitters, so the three
/// sites can never disagree.
static std::string ejitRegistrationKey(const Module &M, const Function &F) {
  return F.hasLocalLinkage() ? ejitStaticHelperKey(M.getName(), F.getName())
                             : F.getName().str();
}

/// Registration key for a const global variable that extractAndSerialize
/// externalizes out of the bitcode. Externally linked constants keep their
/// (already process-unique) name; internal constants (static const, private
/// string literals) are module-local only, so they get the same deterministic
/// "ejit_static.<TU basename>.<hash>.<name>" scheme as externalized helper
/// functions. Single source of truth shared by the extracted-bitcode rename
/// and both registration emitters.
///
/// The runtime symbol table is a flat per-process map (EJitOrcEngine
/// userSymbols): two TUs both defining `static const int table[]` would
/// silently bind the second registration to the first address, so the
/// module-unique prefix is mandatory for anything with local linkage. The
/// IR name is appended verbatim: names are unique within a module, so the
/// key is injective per TU (".str" and "_str" hash into the same suffix but
/// keep distinct keys) and no per-run collision handling is needed.
static std::string ejitGVRegistrationKey(const Module &M,
                                         const GlobalVariable &GV) {
  if (!GV.hasLocalLinkage())
    return GV.getName().str();
  return ejitStaticHelperKey(M.getName(), GV.getName());
}

static bool isEjitEntryFunction(const Function &F) {
  return hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY);
}

static void collectEntryFunctions(Module &M,
                                  SmallVectorImpl<Function *> &EntryFuncs) {
  for (Function &F : M.functions())
    if (isEjitEntryFunction(F))
      EntryFuncs.push_back(&F);
}

static const GlobalVariable *findRootGV(const Value *V, APInt &Offset,
                                        const DataLayout &DL);

/// Resolve a value to the underlying GlobalVariable it references, walking
/// through bitcasts, addrspacecasts and constant-offset GEP chains (mirrors
/// findRootGV). This is the single definition of "this operand references a
/// global", shared by the closure collector and both symbol-registration
/// emitters. Keeping them on one helper avoids the class of bug where a
/// global is kept in the extracted bitcode (as an external declaration) by
/// the collector but never registered by the emitters — which the JIT linker
/// then fails to resolve.
static GlobalVariable *rootGlobal(Value *V, const DataLayout &DL) {
  APInt Offset;
  return const_cast<GlobalVariable *>(findRootGV(V, Offset, DL));
}

static void collectReferencedGlobals(Function &F,
                                     SetVector<GlobalVariable *> &Globals) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      for (Value *Op : I.operands())
        if (auto *GV = rootGlobal(Op, DL))
          Globals.insert(GV);
}

static void computeTransitiveClosure(
    const SmallVectorImpl<Function *> &EntryFuncs,
    SetVector<Function *> &ClosureFuncs,
    SetVector<GlobalVariable *> &ClosureGlobals) {

  SmallVector<Function *, 16> Worklist(EntryFuncs.begin(), EntryFuncs.end());
  while (!Worklist.empty()) {
    Function *F = Worklist.pop_back_val();
    if (!ClosureFuncs.insert(F))
      continue;
    collectReferencedGlobals(*F, ClosureGlobals);
    for (BasicBlock &BB : *F)
      for (Instruction &I : BB)
        if (auto *CB = dyn_cast<CallBase>(&I))
          if (Function *Callee = CB->getCalledFunction())
            if (!Callee->isDeclaration() && !Callee->isIntrinsic())
              Worklist.push_back(Callee);
  }
}

/// Walk a GEP chain from a load's pointer operand down to the root
/// GlobalVariable, accumulating the total byte offset.
static const GlobalVariable *findRootGV(const Value *V, APInt &Offset,
                                         const DataLayout &DL) {
  Offset = APInt(DL.getPointerSizeInBits(0), 0);
  while (V) {
    V = V->stripPointerCasts();
    if (isa<GlobalVariable>(V))
      return cast<GlobalVariable>(V);
    auto *GEP = dyn_cast<GEPOperator>(V);
    if (!GEP)
      return nullptr;
    SmallVector<Value *, 4> IdxList;
    for (auto I = GEP->idx_begin(), E = GEP->idx_end(); I != E; ++I) {
      if (!isa<ConstantInt>(*I))
        return nullptr;
      IdxList.push_back(*I);
    }
    Offset += DL.getIndexedOffsetInType(GEP->getSourceElementType(), IdxList);
    V = GEP->getPointerOperand();
  }
  return nullptr;
}

/// Re-annotate loads with !ejit.may_const using GV-level offset metadata.
/// Optimization passes may drop per-load metadata; this restores it from
/// the !ejit.may_const_field entries on the GV's !ejit.metadata.
static void reAnnotateMayConst(Module &M) {
  const DataLayout &DL = M.getDataLayout();
  LLVMContext &Ctx = M.getContext();
  auto MayConstKind = Ctx.getMDKindID(MD_EJIT_MAY_CONST);

  // Build offset map from GV metadata
  DenseMap<const GlobalVariable *, SmallVector<uint64_t, 4>> mayConstMap;
  for (GlobalVariable &GV : M.globals()) {
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;
    SmallVector<uint64_t, 4> offsets;
    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;
      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_MAY_CONST_FIELD)
        continue;
      if (auto *CI = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(1)))
        offsets.push_back(CI->getZExtValue());
    }
    if (!offsets.empty())
      mayConstMap[&GV] = std::move(offsets);
  }
  if (mayConstMap.empty())
    return;

  // Re-annotate matching loads
  unsigned count = 0;
  for (Function &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *LI = dyn_cast<LoadInst>(&I);
        if (!LI || LI->hasMetadata(MayConstKind))
          continue;
        // Never folded, so never re-annotated.
        if (LI->isVolatile() || LI->isAtomic())
          continue;
        // The recorded offsets are element-relative, so match on the field
        // coordinate rather than the total offset from the global.
        const GlobalVariable *GV = nullptr;
        auto Off = ejitMayConstFieldOffset(LI->getPointerOperand(), DL, GV);
        if (!Off || !GV)
          continue;
        auto it = mayConstMap.find(GV);
        if (it == mayConstMap.end())
          continue;
        if (!is_contained(it->second, *Off))
          continue;
        // The offset only says where the load starts. A wider load straddles the
        // next field, which is free to change.
        TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
        if (AccessSize.isScalable() ||
            !ejitAccessFitsMayConstField(GV, *Off, AccessSize.getFixedValue(),
                                         DL))
          continue;
        LI->setMetadata(MayConstKind, MDNode::get(Ctx, {}));
        count++;
      }
    }
  }
  LLVM_DEBUG(dbgs() << "ejit-register-bitcode: re-annotated " << count
                    << " may_const load(s)\n");
}

/// Run pre-optimization on the extracted bitcode at AOT time to reduce
/// JIT compilation pressure. In debug/shared builds this is a no-op
/// (cyclic link dependency: LLVMPasses <-> LLVMEmbeddedJIT).
#ifdef NDEBUG
static void preOptimizeBitcode(Module &M) {
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // 1. Frontend-IR cleanup BEFORE inlining, mirroring the host O2 pipeline
  //    (EarlyFPM + GlobalCleanupPM both precede its inliner). Raw CodeGen IR
  //    keeps every C local as an alloca + store/load pair, which inflates the
  //    inliner's computed callee cost ~4x; without this round the embedded
  //    bitcode inlines far less than the same module compiled AOT - and the
  //    JIT pipeline runs no inliner of its own (EJitOptimizer relies on this
  //    AOT-time inlining), so every missed inline is a permanent call.
  {
    FunctionPassManager FPM;
    FPM.addPass(LowerExpectIntrinsicPass());
    FPM.addPass(SimplifyCFGPass());
    FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
    FPM.addPass(EarlyCSEPass());
    FPM.addPass(InstCombinePass());
    FPM.addPass(SimplifyCFGPass());
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        FPM.run(F, FAM);
  }

  // 2. Inline: AlwaysInline + cost-based inliner for small functions
  {
    ModulePassManager MPM;
    MPM.addPass(AlwaysInlinerPass());
    MPM.addPass(PB.buildModuleInlinerPipeline(
        llvm::OptimizationLevel::O2, ThinOrFullLTOPhase::None));
    MPM.run(M, MAM);
  }

  // 3. Mem2Reg: promote any allocas left after inlining (AlwaysInliner can
  // still emit fresh ones around inlined code) to SSA
  {
    FunctionPassManager FPM;
    FPM.addPass(PromotePass());
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        FPM.run(F, FAM);
  }

  // 4. EarlyCSE + InstCombine: simplify and fold redundant computations
  {
    FunctionPassManager FPM;
    FPM.addPass(EarlyCSEPass());
    FPM.addPass(InstCombinePass());
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        FPM.run(F, FAM);
  }

  // 5. SimplifyCFG: flatten branches, merge blocks
  {
    FunctionPassManager FPM;
    FPM.addPass(SimplifyCFGPass());
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        FPM.run(F, FAM);
  }

  // 6. Restore !ejit.may_const metadata that passes may have dropped
  reAnnotateMayConst(M);
}
#else
static void preOptimizeBitcode(Module &) {}
#endif

/// Dump the extracted bitcode module to EJitDumpBitcodeDir (if set) for
/// debugging — e.g. to confirm an ejit_entry function is emitted as a
/// definition rather than just a declaration. Parallel-safe under -j: the
/// filename embeds the process id (distinct per concurrent clang) and the
/// sanitized module name, so no two invocations share a file and writes
/// never serialize. Writes both .ll (readable: grep "define @Fn" vs
/// "declare @Fn") and .bc.
static void dumpExtractedBitcode(const Module &M, StringRef Dir) {
  if (Dir.empty())
    return;
  if (std::error_code EC = sys::fs::create_directories(Dir))
    return;
  std::string Stem;
  StringRef Name = M.getName();
  if (Name.empty())
    Name = "ejit_module";
  auto IsAlnum = [](char C) {
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
           (C >= '0' && C <= '9');
  };
  for (char C : Name)
    Stem.push_back(IsAlnum(C) ? C : '_');
  unsigned Pid = sys::Process::getProcessId();
  std::string Base = (Twine(Dir) + "/" + Twine(Pid) + "_" + Stem).str();

  std::error_code EC;
  raw_fd_ostream LL(Base + ".ll", EC);
  if (!EC)
    M.print(LL, nullptr);
  raw_fd_ostream BC(Base + ".bc", EC);
  if (!EC)
    WriteBitcodeToFile(M, BC);
}

/// Diagnostic: count globals carrying !ejit.metadata, and within that the
/// period_arr / may_const_field tags. Gated on EJitDumpBitcodeDir so it only
/// runs when the dump is enabled. Used to pinpoint where the struct-field
/// pass's GV metadata is lost during extraction (clone → preOptimize →
/// externalization).
static void logEJitGlobalMeta(const char *label, const Module &M) {
  if (EJitDumpBitcodeDir.empty())
    return;
  unsigned withMeta = 0, periodArr = 0, mayConstField = 0;
  for (const GlobalVariable &GV : M.globals()) {
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;
    ++withMeta;
    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;
      if (auto *Tag = dyn_cast<MDString>(Sub->getOperand(0))) {
        if (Tag->getString() == TAG_EJIT_PERIOD_ARR)
          ++periodArr;
        else if (Tag->getString() == TAG_EJIT_MAY_CONST_FIELD)
          ++mayConstField;
      }
    }
  }
  errs() << "ejit-register-bitcode: " << label
         << " globals=" << M.global_size() << " withMeta=" << withMeta
         << " periodArr=" << periodArr << " mayConstField=" << mayConstField
         << "\n";
}

namespace {

/// Per-function direct info used by the specialization diagnostic.
struct EjitFuncDiagInfo {
  bool HasMayConstLoad = false;
  unsigned MayConstCount = 0;        // # of !ejit.may_const loads (direct)
  unsigned MayConstInLoopCount = 0;  // subset sitting inside a loop
  SetVector<StringRef> RefsPeriodArr; // deduped period names referenced
  SmallVector<const Function *, 4> Callees;  // direct, defined, non-intrinsic
};

/// One ejit_entry's declared dimensions, parsed from the original module's
/// function metadata (robust to whether the extracted module's function
/// metadata survived clone + preOptimize).
struct EjitEntryDiag {
  std::string Name;
  // (period name, parameter index) per ejit_period_arr_ind declaration.
  SmallVector<std::pair<std::string, unsigned>, 4> DeclaredDims;
  // Period names an ejit_bound_ptr parameter is bound to. EJitWrapperGen
  // requires exactly one matching ejit_period_arr_ind for each, so the
  // dimension cannot be removed - #2 must not advise removing it.
  SmallVector<std::string, 4> BoundPtrPeriods;
};

} // namespace

/// Return true if \p BB lies on a CFG cycle (reachable from itself via a
/// non-empty path), i.e. it may execute more than once. This is a lightweight
/// loop-membership test that avoids pulling LoopInfo / PassBuilder (and their
/// shared-library link dependencies) into LLVMEmbeddedJIT. For natural loops
/// it agrees with LoopInfo; it also accepts irreducible cycles, which is fine
/// for an informational "may execute repeatedly" signal.
static bool isOnCfgCycle(const BasicBlock *BB) {
  SmallPtrSet<const BasicBlock *, 16> Visited;
  SmallVector<const BasicBlock *, 16> WL(successors(BB).begin(),
                                         successors(BB).end());
  while (!WL.empty()) {
    const BasicBlock *N = WL.pop_back_val();
    if (N == BB)
      return true;
    if (!Visited.insert(N).second)
      continue;
    append_range(WL, successors(N));
  }
  return false;
}

/// Collect all basic blocks of \p F that lie on a CFG cycle.
static void computeLoopBBs(Function &F,
                           SmallPtrSetImpl<const BasicBlock *> &LoopBBs) {
  for (const BasicBlock &BB : F)
    if (isOnCfgCycle(&BB))
      LoopBBs.insert(&BB);
}

/// Compute direct (pre-propagation) diagnostic info for \p F. \p LoopBBs is
/// optional (non-null only when the may_const count report is enabled).
static void
computeEjitFuncDiagInfo(Function &F, EjitFuncDiagInfo &Info, unsigned MayConstKind,
                        const SmallPtrSetImpl<const BasicBlock *> *LoopBBs) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  SetVector<GlobalVariable *> GVs;
  for (BasicBlock &BB : F) {
    const bool InLoop = LoopBBs && LoopBBs->count(&BB);
    for (Instruction &I : BB) {
      if (auto *Ld = dyn_cast<LoadInst>(&I))
        if (Ld->hasMetadata(MayConstKind)) {
          Info.HasMayConstLoad = true;
          ++Info.MayConstCount;
          if (InLoop)
            ++Info.MayConstInLoopCount;
        }
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (Function *Callee = CB->getCalledFunction())
          if (!Callee->isDeclaration() && !Callee->isIntrinsic())
            Info.Callees.push_back(Callee);
      // Collect referenced GVs for period-arr lookup in the same traversal.
      for (Value *Op : I.operands())
        if (auto *GV = rootGlobal(Op, DL))
          GVs.insert(GV);
    }
  }
  // Derive period-arr names from collected GVs.
  for (GlobalVariable *GV : GVs) {
    MDNode *GMD = GV->getMetadata(MD_EJIT_METADATA);
    if (!GMD)
      continue;
    for (const MDOperand &Op : GMD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;
      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR)
        continue;
      if (auto *PN = dyn_cast<MDString>(Sub->getOperand(1)))
        Info.RefsPeriodArr.insert(PN->getString());
    }
  }
}

/// AOT specialization diagnostics on the extracted bitcode (post-preOptimize),
/// which is exactly what the JIT will specialize.
///   #1: ejit_entry whose specialization closure reads no ejit_may_const field.
///   #2: ejit_entry that declares ejit_period_arr_ind(P) but whose closure
///       neither reads that parameter nor indexes an ejit_period_arr(P), and
///       has no ejit_bound_ptr bound to P.
/// The closure is the direct-call reachability within the extracted module.
/// External calls (declarations) and indirect calls (function pointers) do NOT
/// count: the JIT cannot inline them, so their may_const reads never enter this
/// entry's specialization. This keeps #1 sound (no false positives).
static void
runSpecializationDiagnostic(Module &Extracted,
                            const SmallVectorImpl<Function *> &EntryFuncs) {
  if (!EJitWarnNoSpecialization && !EJitWarnUnusedDim && !EJitReportMayConst &&
      !EJitWarnFewMayConst)
    return;

  SmallVector<EjitEntryDiag, 4> Entries;
  for (const Function *F : EntryFuncs) {
    EjitEntryDiag ED;
    ED.Name = F->getName().str();
    if (MDNode *MD = F->getMetadata(MD_EJIT_METADATA))
      for (const MDOperand &Op : MD->operands()) {
        auto *Sub = dyn_cast<MDNode>(Op.get());
        if (!Sub || Sub->getNumOperands() < 2)
          continue;
        auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
        if (!Tag)
          continue;
        if (Tag->getString() == TAG_EJIT_PERIOD_ARR_IND) {
          if (Sub->getNumOperands() < 3)
            continue;
          auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
          auto *IdxC = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
          if (PN && IdxC)
            ED.DeclaredDims.push_back(
                {PN->getString().str(),
                 static_cast<unsigned>(IdxC->getZExtValue())});
        } else if (Tag->getString() == TAG_EJIT_BOUND_PTR) {
          if (auto *PN = dyn_cast<MDString>(Sub->getOperand(1)))
            ED.BoundPtrPeriods.push_back(PN->getString().str());
        }
      }
    Entries.push_back(std::move(ED));
  }

  unsigned MayConstKind = Extracted.getContext().getMDKindID(MD_EJIT_MAY_CONST);

  // Direct info per defined function in the extracted module. Loop membership
  // (for the may_const count report) uses a lightweight CFG-cycle test so the
  // report does not pull LoopInfo / PassBuilder into LLVMEmbeddedJIT.
  DenseMap<const Function *, EjitFuncDiagInfo> Info;
  for (Function &F : Extracted) {
    if (F.isDeclaration())
      continue;
    SmallPtrSet<const BasicBlock *, 16> LoopBBs;
    bool NeedLoops = EJitReportMayConst || EJitWarnFewMayConst > 0;
    if (NeedLoops)
      computeLoopBBs(F, LoopBBs);
    computeEjitFuncDiagInfo(F, Info[&F], MayConstKind,
                            NeedLoops ? &LoopBBs : nullptr);
  }

  // Fixpoint: propagate HasMayConstLoad and RefsPeriodArr up the call graph.
  // The extracted module holds only the closure, so every function is reachable
  // from some entry; the monotonic fixpoint converges quickly.
  DenseMap<const Function *, bool> ClosureMC;
  DenseMap<const Function *, SetVector<StringRef>> ClosureRefs;
  for (auto &KV : Info) {
    ClosureMC[KV.first] = KV.second.HasMayConstLoad;
    ClosureRefs[KV.first] = KV.second.RefsPeriodArr;
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &KV : Info) {
      const Function *F = KV.first;
      for (const Function *Callee : KV.second.Callees) {
        if (!Info.count(Callee))
          continue;
        if (ClosureMC[Callee] && !ClosureMC[F]) {
          ClosureMC[F] = true;
          Changed = true;
        }
        for (StringRef P : ClosureRefs[Callee])
          if (ClosureRefs[F].insert(P))
            Changed = true;
      }
    }
  }

  // Emit diagnostics per entry (locate in the extracted module by name).
  for (const EjitEntryDiag &ED : Entries) {
    const Function *EF = Extracted.getFunction(ED.Name);
    if (!EF || EF->isDeclaration())
      continue;
    auto MCIt = ClosureMC.find(EF);
    auto RefIt = ClosureRefs.find(EF);
    if (MCIt == ClosureMC.end() || RefIt == ClosureRefs.end())
      continue;

    // #1: no ejit_may_const read in the specialization closure.
    if (EJitWarnNoSpecialization && !MCIt->second)
      errs() << "EJit warning: ejit_entry function '" << EF->getName()
             << "' reads no ejit_may_const field in its specialization "
                "closure; no JIT specialization value, consider removing "
                "ejit_entry\n";

    // #2: declared dimension neither consumed by the JIT nor required by
    // another mechanism. The runtime substitutes the index parameter with
    // the current period value and folds EVERY use
    // (EJitOptimizer::preReplacePeriodIndices), so a parameter read anywhere
    // in the closure - plain arithmetic, not just period-array indexing - is
    // a real specialization use. The dimension is also used when the closure
    // references the period array: its folded may_const values are refreshed
    // by recompiling when the dimension's period advances. And a dimension
    // bound to an ejit_bound_ptr parameter must stay declared: EJitWrapperGen
    // rejects a bound pointer without exactly one matching dimension. Warn
    // only when none of these hold.
    if (EJitWarnUnusedDim)
      for (const auto &[P, ArgIdx] : ED.DeclaredDims) {
        if (is_contained(ED.BoundPtrPeriods, P))
          continue;
        if (RefIt->second.count(P) != 0)
          continue;
        if (ArgIdx < EF->arg_size() && !EF->getArg(ArgIdx)->use_empty())
          continue;
        errs() << "EJit warning: ejit_entry function '" << EF->getName()
               << "' declares ejit_period_arr_ind('" << P
               << "') but its specialization closure neither reads that "
                  "parameter nor indexes an ejit_period_arr('"
               << P << "'); unused specialization dimension, consider "
                          "removing it\n";
      }
  }

  // Per-entry may_const read counts over the specialization closure (BFS).
  // Used by the info report and the few-may-const warning; both share the
  // same closure walk so they share one gated loop.
  if (EJitReportMayConst || EJitWarnFewMayConst > 0) {
    for (const EjitEntryDiag &ED : Entries) {
      const Function *EF = Extracted.getFunction(ED.Name);
      if (!EF || EF->isDeclaration())
        continue;
      // BFS the entry's direct-call closure within the extracted module.
      DenseSet<const Function *> Seen;
      SmallVector<const Function *, 8> WL{EF};
      unsigned K = 0, J = 0;
      while (!WL.empty()) {
        const Function *F = WL.pop_back_val();
        if (!Seen.insert(F).second)
          continue;
        auto It = Info.find(F);
        if (It == Info.end())
          continue;
        K += It->second.MayConstCount;
        J += It->second.MayConstInLoopCount;
        for (const Function *Callee : It->second.Callees)
          WL.push_back(Callee);
      }

      // Info report (not a warning): summary of all may-const reads.
      if (EJitReportMayConst)
        errs() << "EJit info: ejit_entry function '" << EF->getName() << "': "
               << K << " ejit_may_const read" << (K == 1 ? "" : "s") << " ("
               << J << " in loops)\n";

      // Warning #3: too few may-const reads for meaningful specialization.
      // A low count means the JIT has little to fold, but doesn't mean the
      // entry is misconfigured — the significance depends on what those loads
      // gate.  This only flags the count for manual review.
      // A load inside a loop is high specialization value regardless of
      // count — a single loop load executes thousands of times.  Only
      // warn when there are no loop loads AND the total is below threshold.
      if (EJitWarnFewMayConst > 0 && K < EJitWarnFewMayConst && J == 0)
        errs() << "EJit warning: ejit_entry function '" << EF->getName()
               << "' has only " << K << " ejit_may_const read"
               << (K == 1 ? "" : "s") << " in its specialization closure"
               << " (threshold: " << EJitWarnFewMayConst
               << "); low specialization surface, consider adding more"
                  " may-const fields\n";
    }
  }
}

static std::string extractAndSerialize(Module &M,
    const SetVector<Function *> &Funcs,
    const SetVector<GlobalVariable *> &Globals,
    const SmallVectorImpl<Function *> &EntryFuncs,
    const SetVector<Function *> &ToExternalize) {

  auto Extracted = CloneModule(M);

  // Preserve the original entry name for its own funcIndex/bitcode lookup,
  // but carry a process-unique wrapper key for local entries. The JIT applies
  // this key only when another entry's specialization externalizes this one.
  for (Function *F : EntryFuncs) {
    if (!F->hasLocalLinkage())
      continue;
    if (Function *Cur = Extracted->getFunction(F->getName()))
      Cur->addFnAttr(ATTR_EJIT_WRAPPER_SYMBOL, ejitRegistrationKey(M, *F));
  }

  DenseSet<StringRef> FuncNames;
  for (Function *F : Funcs)
    FuncNames.insert(F->getName());

  DenseSet<StringRef> GlobalNames;
  for (GlobalVariable *GV : Globals)
    GlobalNames.insert(GV->getName());

  SmallVector<Function *, 16> FuncsToDelete;
  for (Function &F : Extracted->functions())
    if (!FuncNames.count(F.getName()))
      FuncsToDelete.push_back(&F);
  for (Function *F : FuncsToDelete) {
    if (F->isDeclaration())
      continue; // Keep declarations (intrinsics, external refs)
    F->replaceAllUsesWith(UndefValue::get(F->getType()));
    F->deleteBody();
    F->eraseFromParent();
  }

  SmallVector<GlobalVariable *, 16> GVToDelete;
  for (GlobalVariable &GV : Extracted->globals())
    if (!GlobalNames.count(GV.getName()))
      GVToDelete.push_back(&GV);
  for (GlobalVariable *GV : GVToDelete) {
    if (GV->isDeclaration())
      continue; // Keep declarations (external refs)
    GV->replaceAllUsesWith(UndefValue::get(GV->getType()));
    GV->eraseFromParent();
  }

  // Pre-optimize the extracted bitcode to reduce JIT compilation pressure.
  // InstCombine + Mem2Reg + SimplifyCFG folds constant chains, promotes
  // allocas, and cleans up dead branches before serialization.
  logEJitGlobalMeta("extract-after-clone", *Extracted);
  preOptimizeBitcode(*Extracted);
  logEJitGlobalMeta("extract-after-preOpt", *Extracted);

  // Specialization diagnostics on the post-preOptimize extracted module (the
  // exact bitcode the JIT will specialize). Must run before the extern
  // conversion below so GV definitions (and their !ejit.metadata) are intact.
  runSpecializationDiagnostic(*Extracted, EntryFuncs);

  // Externalize the closure helpers that generateSymbolRegisters /
  // generateRegistryTable register on the AOT side: turn each surviving
  // (i.e. not fully preopt-inlined) helper into an external declaration so
  // its body is not serialized into the bitcode. Internal helpers are
  // renamed to their deterministic registration key (their AOT symbol is
  // invisible to the JIT linker); externally-linked helpers keep their
  // already process-unique name. Runs before the internalize step below so
  // the original linkage is still visible here.
  unsigned Externalized = 0;
  for (Function *F : ToExternalize) {
    Function *Cur = Extracted->getFunction(F->getName());
    if (!Cur || Cur->isDeclaration())
      continue; // fully inlined by preOptimizeBitcode
    bool WasLocal = Cur->hasLocalLinkage();
    Cur->deleteBody();
    Cur->setVisibility(GlobalValue::DefaultVisibility);
    Cur->setLinkage(GlobalValue::ExternalLinkage);
    // The declaration is now resolved from outside the module (AOT-side
    // registration), so it must not be marked dso_local. InternalLinkage
    // implies dso_local and changing the linkage does not clear it.
    Cur->setDSOLocal(false);
    if (WasLocal)
      Cur->setName(ejitRegistrationKey(M, *F));
    ++Externalized;
  }
  LLVM_DEBUG(dbgs() << "ejit-register-bitcode: externalized " << Externalized
                    << " of " << ToExternalize.size()
                    << " closure helper(s) in bitcode\n");

  // Convert kept global definitions to external declarations so the JIT
  // linker resolves them from the host process — including constants.
  // Constant definitions in the extracted bitcode materialize as JIT-side
  // .rodata: every adrp+ldr that survives preOptimizeBitcode then reads a
  // private copy instead of the AOT original. Externalizing every surviving
  // const definition (closures whose loads folded away lose their dead
  // definition entirely) resolves the loads to the AOT image's own rodata,
  // so the JIT object carries no constant pool of its own.
  //
  // Internal constants (static const, private string literals) are renamed
  // to their deterministic registration key — module-local names are not
  // process-unique and the runtime's flat symbol table would collide across
  // TUs. External constants keep their unique name. The rename and the
  // registration both derive from ejitGVRegistrationKey, so they cannot
  // disagree.
  for (GlobalVariable &GV : Extracted->globals()) {
    if (GV.isDeclaration())
      continue;
    // Period variables carry !ejit.metadata: PASS2 registers them under their
    // ORIGINAL name (ejit_register_period_array / ejit_register_static_var)
    // and the JIT optimizer looks the array up by the bitcode-declaration
    // name (getArrayInfo). Renaming one to its ejit_static.* key would
    // break that lookup, and registering it under the key would break the
    // period registry — so period variables keep their definition and name
    // exactly as before this externalization. A const period array is not a
    // JIT rodata copy anyway: it is the specialization array itself.
    if (GV.isConstant() && !GV.hasMetadata(MD_EJIT_METADATA)) {
      // Capture the key before dropping the linkage: internal constants are
      // renamed to it and the registration emitters must use the same value.
      // ejitGVRegistrationKey only reads the module name and the GV name, so
      // calling it on the clone's GV against the original module is fine.
      bool WasLocal = GV.hasLocalLinkage();
      std::string Key = ejitGVRegistrationKey(M, GV);
      GV.setInitializer(nullptr);
      GV.setVisibility(GlobalValue::DefaultVisibility);
      GV.setLinkage(GlobalValue::ExternalLinkage);
      // InternalLinkage implies dso_local and changing the linkage does not
      // clear it (same pitfall the closure-helper externalization below
      // fixes); a dso_local declaration drives PC-relative addressing and
      // GOT lowering against a definition that no longer exists in this
      // module.
      GV.setDSOLocal(false);
      if (WasLocal)
        GV.setName(Key);
      continue;
    }
    if (GV.hasMetadata(MD_EJIT_METADATA))
      continue; // Period variable: keep its definition (see above).
    GV.setInitializer(nullptr);
    GV.setLinkage(GlobalValue::ExternalLinkage);
  }
  // Pre-internalize non-entry definitions so the JIT's IRMaterializationUnit
  // does not advertise them in MR->getSymbols().  The JIT-side
  // runInterproceduralPropagation also internalizes them (for IPSCCP), but
  // that runs inside the IR transform callback — after the MU has already
  // been created with the original symbol claim set.  If a helper is
  // advertised as an external definition in the MU but codegen emits it as
  // STB_LOCAL (because the JIT-side internalize ran after the MU was fixed),
  // JITLink maps STB_LOCAL to Scope::Local and excludes it from
  // InternedResult, breaking the ORC invariant that every claimed symbol has
  // a matching definition.  Doing it here, before serialization, makes the
  // embedded bitcode self-consistent: the MU only claims entry-point symbols
  // (which still have external linkage), and the JIT-side internalize pass
  // becomes a no-op for helpers (they already have local linkage).
  for (Function &F : Extracted->functions()) {
    if (F.isDeclaration() || F.hasLocalLinkage())
      continue;
    if (hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY))
      continue;
    F.setVisibility(GlobalValue::DefaultVisibility);
    F.setLinkage(GlobalValue::InternalLinkage);
  }

  // Move the entry definitions to the head of the function list, keeping their
  // relative order. This is about the ALIGNMENT of the specialized entry, not
  // about ordering for its own sake:
  //
  //   * The JIT TargetMachine leaves function-sections off, so codegen emits
  //     every function into one .text in module order, and AArch64 emits no
  //     inter-function padding — a function that is not first starts at an
  //     arbitrary 4-byte offset.
  //   * EJitCodePoolManager::allocateCode floors each allocation's alignment at
  //     Options::minCodeAlign (64B, raised to the seal page size under
  //     immediate 4K sealing), so only the FIRST function of .text inherits it.
  //
  // A specialization compiles exactly one entry and the JIT deletes the other
  // entries' bodies (isolateSpecializationEntry), so whichever entry is being
  // compiled ends up first in .text and starts on a cache-line (or page)
  // boundary rather than straddling one. Purely a layout change: function order
  // within a module carries no semantics.
  {
    SmallVector<Function *, 4> EntryDefs;
    for (Function &F : Extracted->functions())
      if (!F.isDeclaration() &&
          hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY))
        EntryDefs.push_back(&F);

    auto &FnList = Extracted->getFunctionList();
    auto InsertPt = FnList.begin();
    for (Function *F : EntryDefs) {
      // splice() asserts when the insertion point is the node being moved, and
      // an entry already in place needs no move — just step past it. EntryDefs
      // is in list order, so InsertPt is never at end() here.
      if (&*InsertPt == F) {
        ++InsertPt;
        continue;
      }
      FnList.splice(InsertPt, FnList, F->getIterator());
    }
  }

  logEJitGlobalMeta("extract-after-extern", *Extracted);

  // Optionally dump the extracted module for debugging (e.g. to confirm an
  // ejit_entry function is emitted as a definition, not a declaration).
  // Parallel-safe: filename embeds PID + module name (see dumpExtractedBitcode).
  if (!EJitDumpBitcodeDir.empty())
    dumpExtractedBitcode(*Extracted, EJitDumpBitcodeDir);

  std::string Bitcode;
  raw_string_ostream OS(Bitcode);
  WriteBitcodeToFile(*Extracted, OS);
  OS.flush();
  return Bitcode;
}

static GlobalVariable *embedBitcode(Module &M, const std::string &Bitcode) {
  LLVMContext &Ctx = M.getContext();
  SmallVector<uint8_t, 0> Bytes;
  Bytes.reserve(Bitcode.size());
  for (char C : Bitcode)
    Bytes.push_back(static_cast<uint8_t>(C));

  auto *ArrTy = ArrayType::get(Type::getInt8Ty(Ctx), Bitcode.size());
  auto *Const = ConstantDataArray::get(Ctx, Bytes);
  auto *GV = new GlobalVariable(M, ArrTy, true, GlobalValue::InternalLinkage,
                                Const, GV_EJIT_BITCODE);
  GV->setAlignment(Align(1));
  // Bitcode lives in default section (.rodata for const); no custom section
  // needed — bare-metal environments may not support custom ELF sections.
  return GV;
}

static void collectFunctionsFromConstant(Constant *C,
                                         SmallPtrSetImpl<Function *> &Funcs);

/// Collect external symbols (functions + globals) referenced by the
/// closure and generate ejit_register_symbol calls so the JIT can resolve
/// them without dlsym — suitable for bare-metal embedded environments.
static void generateSymbolRegisters(
    Module &M,
    const SmallVectorImpl<Function *> &EntryFuncs,
    const SetVector<Function *> &ClosureFuncs,
    const SetVector<GlobalVariable *> &ClosureGlobals,
    const SetVector<Function *> &ToExternalize,
    Function *AutoReg) {
  LLVMContext &Ctx = M.getContext();
  const DataLayout &DL = M.getDataLayout();
  auto *VoidTy = Type::getVoidTy(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);

  M.getOrInsertFunction(FN_REGISTER_SYMBOL,
      FunctionType::get(VoidTy, {PtrTy, PtrTy}, false));

  std::set<std::string> registered;

  auto isPeriodVar = [&](GlobalVariable &GV) -> bool {
    return GV.hasMetadata(MD_EJIT_METADATA);
  };

  BasicBlock *BB = &AutoReg->getEntryBlock();
  Instruction *InsertBefore = BB->getTerminator();

  // Nested ejit_entry calls are specialization boundaries. PASS3 rewrites
  // these Function objects in place into their AOT wrappers after this pass,
  // so the addresses recorded here ultimately point at the wrappers. The JIT
  // externalizes every entry except the one currently being specialized and
  // resolves calls to those declarations through this table.
  for (Function *F : EntryFuncs) {
    std::string Name = ejitRegistrationKey(M, *F);
    if (registered.insert(Name).second) {
      IRBuilder<> Builder(InsertBefore);
      Builder.CreateCall(M.getFunction(FN_REGISTER_SYMBOL),
                         {Builder.CreateGlobalString(Name),
                          Builder.CreateBitCast(F, PtrTy)});
    }
  }

  for (Function *F : ClosureFuncs) {
    for (BasicBlock &Blk : *F) {
      for (Instruction &I : Blk) {
        // External function calls
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (Function *Callee = CI->getCalledFunction()) {
            if (Callee->isDeclaration() && !Callee->isIntrinsic()) {
              std::string Name = Callee->getName().str();
              if (registered.insert(Name).second) {
                IRBuilder<> Builder(InsertBefore);
                Builder.CreateCall(M.getFunction(FN_REGISTER_SYMBOL),
                    {Builder.CreateGlobalString(Name),
                     Builder.CreateBitCast(Callee, PtrTy)});
              }
            }
          }
        }
        // Global variable references. Every closure global the collector
        // kept is resolved from the host process at JIT link time: const
        // definitions are externalized by extractAndSerialize (no JIT-side
        // copy — loads resolve to the AOT image's own rodata), and period
        // variables were always external. Dropping any of them leaves an
        // unresolved external that fails JITLink. Internal constants are
        // registered under their deterministic ejit_static.* key, matching
        // the extracted-bitcode rename. Resolve through bitcasts/GEPs via
        // rootGlobal so registration matches what collectReferencedGlobals
        // kept in the extracted bitcode.
        for (Use &U : I.operands()) {
          auto *GV = rootGlobal(U.get(), DL);
          if (!GV)
            continue;
          if (GV->isDeclaration() || !isPeriodVar(*GV)) {
            std::string Name = ejitGVRegistrationKey(M, *GV);
            if (registered.insert(Name).second) {
              IRBuilder<> Builder(InsertBefore);
              Builder.CreateCall(M.getFunction(FN_REGISTER_SYMBOL),
                  {Builder.CreateGlobalString(Name),
                   Builder.CreateBitCast(GV, PtrTy)});
            }
          }
        }
      }
    }
  }

  // Scan GlobalVariable initializers for function pointers stored in
  // constant aggregates (e.g., const arrays of structs with fn_ptr
  // fields used as indirect-call tables).  The instruction-level scan
  // above only catches direct calls and direct GV operand references;
  // indirect calls through loaded function pointers are missed because
  // CI->getCalledFunction() returns nullptr.
  for (GlobalVariable *GV : ClosureGlobals) {
    if (!GV->hasInitializer())
      continue;
    SmallPtrSet<Function *, 8> FuncsInInit;
    collectFunctionsFromConstant(GV->getInitializer(), FuncsInInit);
    for (Function *F : FuncsInInit) {
      if (!F->isDeclaration() || F->isIntrinsic())
        continue;
      std::string Name = F->getName().str();
      if (registered.insert(Name).second) {
        IRBuilder<> Builder(InsertBefore);
        Builder.CreateCall(M.getFunction("ejit_register_symbol"),
            {Builder.CreateGlobalString(Name),
             Builder.CreateBitCast(F, PtrTy)});
      }
    }
  }

  // Register externalized closure helpers under the same deterministic keys
  // extractAndSerialize renamed them to in the bitcode. The address is the
  // AOT original — taking it also keeps the body alive under --gc-sections.
  for (Function *F : ToExternalize) {
    std::string Key = ejitRegistrationKey(M, *F);
    if (registered.insert(Key).second) {
      IRBuilder<> Builder(InsertBefore);
      Builder.CreateCall(M.getFunction(FN_REGISTER_SYMBOL),
                         {Builder.CreateGlobalString(Key),
                          Builder.CreateBitCast(F, PtrTy)});
    }
  }
}

/// Recursively walk a Constant (initializer of a GlobalVariable) and
/// collect all Function declarations reachable through constant
/// aggregates, structs, and expressions.  This discovers indirect-call
/// targets stored in jump tables / callback arrays that are missed by
/// the instruction-level direct-call scan.
static void collectFunctionsFromConstant(Constant *C,
                                         SmallPtrSetImpl<Function *> &Funcs) {
  if (auto *F = dyn_cast<Function>(C)) {
    Funcs.insert(F);
    return;
  }
  // Stop at GlobalValues (other than Function, which is handled above)
  // to avoid following references to other global variables.
  if (isa<GlobalValue>(C))
    return;
  for (Value *Op : C->operands())
    collectFunctionsFromConstant(cast<Constant>(Op), Funcs);
}

static void
generateRegistryTable(Module &M, const SmallVectorImpl<Function *> &EntryFuncs,
                      const SetVector<Function *> &ClosureFuncs,
                      const SetVector<GlobalVariable *> &ClosureGlobals,
                      const SetVector<Function *> &ToExternalize,
                      GlobalVariable *BitcodeGV);

static void generateRegisterCall(Module &M, GlobalVariable *BitcodeGV,
                                 const SmallVectorImpl<Function *> &EntryFuncs,
                                 const SetVector<Function *> &ClosureFuncs,
                                 const SetVector<GlobalVariable *> &ClosureGlobals,
                                 const SetVector<Function *> &ToExternalize) {
  LLVMContext &Ctx = M.getContext();
  auto *VoidTy = Type::getVoidTy(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  M.getOrInsertFunction(FN_REGISTER_BITCODE,
      FunctionType::get(VoidTy, {PtrTy, PtrTy, I64Ty}, false));

  Function *AutoReg = M.getFunction(FN_AUTO_REGISTER);
  if (!AutoReg) {
    AutoReg = Function::Create(FunctionType::get(VoidTy, false),
                               GlobalValue::InternalLinkage,
                               FN_AUTO_REGISTER, &M);
    BasicBlock::Create(Ctx, "entry", AutoReg);
    ReturnInst::Create(Ctx, &AutoReg->getEntryBlock());
  }

  BasicBlock *EntryBB = &AutoReg->getEntryBlock();
  Instruction *Ret = EntryBB->getTerminator();
  FunctionCallee Callee = M.getFunction(FN_REGISTER_BITCODE);

  for (Function *F : EntryFuncs) {
    IRBuilder<> Builder(Ret);
    Builder.CreateCall(Callee, {
        Builder.CreateGlobalString(F->getName()),
        Builder.CreateBitCast(BitcodeGV, PtrTy),
        ConstantInt::get(I64Ty, BitcodeGV->getValueType()->getArrayNumElements())
    });
  }

  // Auto-register external symbols referenced by the closure so the JIT
  // can resolve them without manual ejit_register_symbol calls.
  generateSymbolRegisters(M, EntryFuncs, ClosureFuncs, ClosureGlobals,
                          ToExternalize, AutoReg);

  if (EnableEJitGlobalCtors)
    appendToGlobalCtors(M, AutoReg, EJIT_CTOR_PRIORITY);

  // Always build the static registry table for bare-metal / testing fallback.
  generateRegistryTable(M, EntryFuncs, ClosureFuncs, ClosureGlobals,
                        ToExternalize, BitcodeGV);
}

/// Emit this translation unit's bitcode registry entries as a private array in
/// the ".ejit_bitcode" section. The linker concatenates these across all TUs;
/// a linker script brackets the section so ejit_init() can walk the
/// [__start_ejit_bitcode, __stop_ejit_bitcode) range on bare-metal where global
/// constructors are unavailable.
static void
generateRegistryTable(Module &M, const SmallVectorImpl<Function *> &EntryFuncs,
                      const SetVector<Function *> &ClosureFuncs,
                      const SetVector<GlobalVariable *> &ClosureGlobals,
                      const SetVector<Function *> &ToExternalize,
                      GlobalVariable *BitcodeGV) {
  LLVMContext &Ctx = M.getContext();
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  // Struct: { i32 type, ptr name1, ptr name2, ptr data, i64 size }
  StructType *EntryTy = StructType::get(
      Ctx, {I32Ty, PtrTy, PtrTy, PtrTy, I64Ty}, /*isPacked=*/false);

  SmallVector<Constant *, 16> Entries;

  // Bitcode entries — use CreateGlobalStringPtr to avoid name clashes
  // with existing functions in the module.
  for (Function *F : EntryFuncs) {
    Constant *NameStr = ConstantDataArray::getString(Ctx, F->getName(), true);
    auto *NameGV = new GlobalVariable(M, NameStr->getType(), true,
        GlobalValue::PrivateLinkage, NameStr, ".ejit.str.");
    Entries.push_back(ConstantStruct::get(EntryTy, {
        ConstantInt::get(I32Ty, EJIT_REG_BITCODE),           // EJIT_REG_BITCODE
        ConstantExpr::getBitCast(NameGV, PtrTy),             // name1 string
        ConstantPointerNull::get(PtrTy),                     // name2 = NULL
        ConstantExpr::getBitCast(BitcodeGV, PtrTy),          // bitcode data ptr
        ConstantInt::get(I64Ty,
            BitcodeGV->getValueType()->getArrayNumElements()),// bitcode size
    }));
  }

  // Symbol entries for external references
  SmallPtrSet<const Function *, 8> SymbolsDone;
  auto addSymbol = [&](const Function *F, bool RequireDeclaration = true,
                       StringRef RegistrationName = {}) {
    if (!F->isIntrinsic() && (!RequireDeclaration || F->isDeclaration())) {
      if (SymbolsDone.insert(F).second) {
        StringRef Name = RegistrationName.empty() ? F->getName()
                                                   : RegistrationName;
        Constant *NameStr = ConstantDataArray::getString(Ctx, Name, true);
        auto *NameGV = new GlobalVariable(M, NameStr->getType(), true,
            GlobalValue::PrivateLinkage, NameStr, ".ejit.str.");
        Entries.push_back(ConstantStruct::get(EntryTy, {
            ConstantInt::get(I32Ty, EJIT_REG_SYMBOL),                      // EJIT_REG_SYMBOL
            ConstantExpr::getBitCast(NameGV, PtrTy),         // name1 string
            ConstantPointerNull::get(PtrTy),
            ConstantExpr::getBitCast(const_cast<Function *>(F), PtrTy),
            ConstantInt::get(I64Ty, 0),
        }));
      }
    }
  };

  // PASS3 later turns these functions into wrappers in place, so these
  // constants resolve to wrapper addresses in the final AOT object.
  for (Function *F : EntryFuncs)
    addSymbol(F, /*RequireDeclaration=*/false, ejitRegistrationKey(M, *F));
  for (Function *F : ClosureFuncs) {
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        if (const CallBase *CB = dyn_cast<CallBase>(&I))
          if (Function *Callee = CB->getCalledFunction())
            addSymbol(Callee);
      }
    }
  }

  // Also collect external function declarations referenced through
  // GlobalVariable initializers (e.g., function pointers stored in
  // constant struct arrays used as indirect-call targets).  The
  // instruction scanning loop above only catches direct calls.
  for (GlobalVariable *GV : ClosureGlobals) {
    if (!GV->hasInitializer())
      continue;
    SmallPtrSet<Function *, 8> FuncsInInit;
    collectFunctionsFromConstant(GV->getInitializer(), FuncsInInit);
    for (Function *F : FuncsInInit)
      addSymbol(F);
  }

  // Global variable symbol entries. Resolve through bitcasts/GEPs via
  // rootGlobal so registration matches what collectReferencedGlobals kept in
  // the extracted bitcode.
  SmallPtrSet<const GlobalVariable *, 4> GVsDone;
  const DataLayout &DL = M.getDataLayout();
  for (Function *F : ClosureFuncs) {
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        for (const Value *Op : I.operands()) {
          const GlobalVariable *GV =
              rootGlobal(const_cast<Value *>(Op), DL);
          // Every closure global is registered: const definitions are
          // externalized in the extracted bitcode and must resolve to the
          // AOT original at JIT link time; extern const / extern mut were
          // always declarations. Internal constants register under their
          // deterministic ejit_static.* key, matching the extracted-bitcode
          // rename (same source of truth as generateSymbolRegisters).
          if (!GV || GV->getName().starts_with("llvm."))
            continue;
          if (!GVsDone.insert(GV).second)
            continue;
          std::string Key = ejitGVRegistrationKey(M, *GV);
          Constant *NameStr = ConstantDataArray::getString(Ctx, Key, true);
          auto *NameGV = new GlobalVariable(M, NameStr->getType(), true,
              GlobalValue::PrivateLinkage, NameStr, ".ejit.str.");
          Entries.push_back(ConstantStruct::get(EntryTy, {
              ConstantInt::get(I32Ty, EJIT_REG_SYMBOL),                // EJIT_REG_SYMBOL
              ConstantExpr::getBitCast(NameGV, PtrTy),   // name1 string
              ConstantPointerNull::get(PtrTy),
              ConstantExpr::getBitCast(
                  const_cast<GlobalVariable *>(GV), PtrTy),
              ConstantInt::get(I64Ty, 0),
          }));
        }
      }
    }
  }

  // Externalized closure helpers: same keys as generateSymbolRegisters, so
  // the bare-metal section walk (no ctors) resolves them identically.
  for (Function *F : ToExternalize) {
    std::string Key = ejitRegistrationKey(M, *F);
    Constant *NameStr = ConstantDataArray::getString(Ctx, Key, true);
    auto *NameGV = new GlobalVariable(M, NameStr->getType(), true,
        GlobalValue::PrivateLinkage, NameStr, ".ejit.str.");
    Entries.push_back(ConstantStruct::get(EntryTy, {
        ConstantInt::get(I32Ty, EJIT_REG_SYMBOL),            // EJIT_REG_SYMBOL
        ConstantExpr::getBitCast(NameGV, PtrTy),             // name1: key
        ConstantPointerNull::get(PtrTy),
        ConstantExpr::getBitCast(F, PtrTy),                  // AOT original
        ConstantInt::get(I64Ty, 0),
    }));
  }

  // No sentinel entry: the runtime iterates the linker-provided
  // [__start_ejit_bitcode, __stop_ejit_bitcode) range over the dedicated
  // section, so each translation unit contributes only its own entries.
  if (Entries.empty())
    return;

  ArrayType *ArrayTy = ArrayType::get(EntryTy, Entries.size());
  Constant *ArrayInit = ConstantArray::get(ArrayTy, Entries);

  // Private linkage + a dedicated section. Every TU emits its own *local*
  // array into ".ejit_bitcode"; the linker concatenates them across TUs. The
  // leading-dot name is the conventional ELF spelling but is NOT a valid C
  // identifier, so the linker does NOT auto-synthesize __start_/__stop_ — a
  // linker script must bracket the section (see ejit_registry.ld). A fixed
  // *external* symbol here would instead produce "duplicate symbol" link
  // errors as soon as more than one TU defines ejit_entry functions.
  // llvm.used keeps the array alive under --gc-sections.
  auto *GV = new GlobalVariable(M, ArrayTy, /*isConstant=*/true,
                                GlobalValue::PrivateLinkage, ArrayInit,
                                ".ejit.registry.bitcode");
  GV->setSection(SECT_EJIT_BITCODE);
  GV->setAlignment(M.getDataLayout().getABITypeAlign(EntryTy));
  appendToUsed(M, {GV});
}

PreservedAnalyses
EJitRegisterBitcodePass::run(Module &M, ModuleAnalysisManager &) {
  LLVM_DEBUG(dbgs() << "ejit-register-bitcode: running on " << M.getName() << "\n");
  SmallVector<Function *, 4> EntryFuncs;
  collectEntryFunctions(M, EntryFuncs);
  if (EntryFuncs.empty()) {
    LLVM_DEBUG(dbgs() << "ejit-register-bitcode: no entry functions, skip\n");
    return PreservedAnalyses::all();
  }

  SetVector<Function *> ClosureFuncs;
  SetVector<GlobalVariable *> ClosureGlobals;
  computeTransitiveClosure(EntryFuncs, ClosureFuncs, ClosureGlobals);
  LLVM_DEBUG(dbgs() << "ejit-register-bitcode: closure " << ClosureFuncs.size()
                    << " funcs, " << ClosureGlobals.size() << " globals\n");
  if (ClosureFuncs.empty())
    return PreservedAnalyses::all();

  // Helpers to externalize: non-entry closure functions whose bodies cost
  // more than the AOT registration record that replaces them. Computed on
  // the raw module so extractAndSerialize and generateSymbolRegisters /
  // generateRegistryTable all agree on the same set.
  SetVector<Function *> ToExternalize;
  for (Function *F : ClosureFuncs)
    if (!isEjitEntryFunction(*F) &&
        F->getInstructionCount() >= EJitExternalizeMinInsts)
      ToExternalize.insert(F);

  std::string Bitcode =
      extractAndSerialize(M, ClosureFuncs, ClosureGlobals, EntryFuncs,
                          ToExternalize);
  GlobalVariable *BitcodeGV = embedBitcode(M, Bitcode);
  generateRegisterCall(M, BitcodeGV, EntryFuncs, ClosureFuncs, ClosureGlobals,
                       ToExternalize);

  return PreservedAnalyses::none();
}
