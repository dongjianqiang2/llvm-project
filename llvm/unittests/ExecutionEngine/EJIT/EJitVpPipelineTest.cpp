//===-- EJitVpPipelineTest.cpp - end-to-end value-profile pipeline test ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Exercises the whole value-profile chain inside the JIT optimizer without
//  ORC: Tier-1 runPipeline (PGO Gen + InstrProfiling lowering + scalar
//  instrumentation + capture), the driver-side merge path (aggregation with
//  verified target mapping + profile synthesis), and the Tier-2 runPipeline
//  (PGOUse + ICP + guarded scalar specialization + optimization pipeline).
//
//  Counter/value payloads are FABRICATED into the addresses the Tier-1 module
//  would have exposed through ORC (the driver glue itself is covered by the
//  EJitPgoTest OrcLookup tests); the profile read-back is done with the
//  official IndexedInstrProfReader.
//
//  Compiled only under EJIT_SRE_PGO_VALUE_PROFILE; empty otherwise.
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_SRE_PGO_VALUE_PROFILE

#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/ProfileData/InstrProfWriter.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Transforms/Instrumentation/PGOInstrumentation.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

static void markEJitEntry(Function &F) {
  LLVMContext &Ctx = F.getContext();
  MDNode *Entry = MDNode::get(Ctx, {MDString::get(Ctx, TAG_EJIT_ENTRY)});
  F.setMetadata(MD_EJIT_METADATA, MDNode::get(Ctx, {Entry}));
}

// Builds: @vp_callee (indirect-call target),
// @vp_entry(i32 %bound, i32 %size, i32 %sel): dynamic indirect call via a
// global slot, dynamic-size memcpy, and a loop bound by %bound.
static std::unique_ptr<Module> makeVpModule(LLVMContext &Ctx,
                                            bool AddCleanupFixtures = false) {
  auto M = std::make_unique<Module>("vp_pipeline_test", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);

  // Callee (noinline so it survives as a promotable target).
  auto *Callee =
      Function::Create(FunctionType::get(I32, {I32}, false),
                       GlobalValue::ExternalLinkage, "vp_callee", M.get());
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Callee);
    IRBuilder<> B(BB);
    Value *V = B.CreateMul(Callee->getArg(0), ConstantInt::get(I32, 3));
    B.CreateRet(B.CreateAdd(V, ConstantInt::get(I32, 1)));
  }

  // Function-pointer slot. MUTABLE (non-constant) so phase-1 IPSCCP cannot
  // fold the load and devirtualize the call before Gen - mirrors the runtime
  // function-pointer tables the board test exercises.
  auto *Slot = new GlobalVariable(
      *M, PointerType::getUnqual(M->getContext()), /*isConstant=*/false,
      GlobalValue::ExternalLinkage, Callee, "vp_slot");

  Function *DeadArg = nullptr;
  Function *Externalized = nullptr;
  if (AddCleanupFixtures) {
    auto *Sink =
        new GlobalVariable(*M, I32, false, GlobalValue::InternalLinkage,
                           ConstantInt::get(I32, 0), "vp_cleanup_sink");
    DeadArg = Function::Create(FunctionType::get(I32, {I32}, false),
                               GlobalValue::InternalLinkage,
                               "vp_cleanup_dead_arg", M.get());
    DeadArg->addFnAttr(Attribute::NoInline);
    {
      BasicBlock *BB = BasicBlock::Create(Ctx, "entry", DeadArg);
      IRBuilder<> B(BB);
      StoreInst *Store = B.CreateStore(ConstantInt::get(I32, 1), Sink);
      Store->setVolatile(true);
      B.CreateRet(ConstantInt::get(I32, 7));
    }

    // This models a #156 closure helper: the body is externalized into the
    // AOT image and must remain a declaration in the extracted module.
    Externalized = Function::Create(FunctionType::get(I32, {I32}, false),
                                    GlobalValue::ExternalLinkage,
                                    "vp_cleanup_externalized", M.get());
    Externalized->setDSOLocal(false);

    auto *Dead = Function::Create(FunctionType::get(I32, {}, false),
                                  GlobalValue::InternalLinkage,
                                  "vp_cleanup_dead_callee", M.get());
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Dead);
    IRBuilder<> B(BB);
    B.CreateRet(ConstantInt::get(I32, 11));
  }

  auto *Entry =
      Function::Create(FunctionType::get(I32, {I32, I32, I32}, false),
                       GlobalValue::ExternalLinkage, "vp_entry", M.get());
  markEJitEntry(*Entry);
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Entry);
    IRBuilder<> B(BB);
    Value *Fp = B.CreateLoad(PointerType::getUnqual(M->getContext()), Slot);
    Value *Arg0 = Entry->getArg(0);
    Value *Sel = Entry->getArg(2);
    // Dynamic indirect call (target constant, callee expression dynamic).
    Value *V = B.CreateCall(Callee->getFunctionType(), Fp, {Sel});
    if (AddCleanupFixtures) {
      V = B.CreateAdd(V, B.CreateCall(DeadArg, {Arg0}), "cleanup_dead_arg_v");
      V = B.CreateAdd(V, B.CreateCall(Externalized, {Arg0}),
                      "cleanup_externalized_v");
    }

    // Dynamic-size memcpy on alloca buffers; the destination IS read below so
    // no optimization may remove the memop site before Gen.
    auto *Dst = B.CreateAlloca(I32, ConstantInt::get(I32, 8));
    auto *Src = B.CreateAlloca(I32, ConstantInt::get(I32, 8));
    B.CreateStore(V, Src);
    Value *Size = B.CreateMul(B.CreateZExt(Entry->getArg(1), B.getInt64Ty()),
                              ConstantInt::get(B.getInt64Ty(), 4));
    B.CreateMemCpy(Dst, Align(4), Src, Align(4), Size);
    Value *Dst0 = B.CreateLoad(I32, Dst);
    Value *VObs = B.CreateAdd(V, Dst0); // keeps the memcpy observable

    // Loop bound by %bound (the scalar/loop-bound site).
    BasicBlock *Hdr = BasicBlock::Create(Ctx, "loop", Entry);
    BasicBlock *Latch = BasicBlock::Create(Ctx, "latch", Entry);
    BasicBlock *Exit = BasicBlock::Create(Ctx, "exit", Entry);
    B.CreateBr(Hdr);
    B.SetInsertPoint(Hdr);
    PHINode *I = B.CreatePHI(I32, 2, "i");
    PHINode *Acc = B.CreatePHI(I32, 2, "acc");
    I->addIncoming(ConstantInt::get(I32, 0), BB);
    Acc->addIncoming(VObs, BB);
    Value *AccN = B.CreateAdd(Acc, I);
    Value *IN = B.CreateAdd(I, ConstantInt::get(I32, 1));
    Value *Cmp = B.CreateICmpSLT(IN, Arg0);
    B.CreateCondBr(Cmp, Latch, Exit);
    B.SetInsertPoint(Latch);
    I->addIncoming(IN, Latch);
    Acc->addIncoming(AccN, Latch);
    B.CreateBr(Hdr);
    B.SetInsertPoint(Exit);
    B.CreateRet(AccN);
  }
  return M;
}

// Read FuncHash + NumCounters + NumValueSites[] from the generated
// __profd_<name> global of a Tier-1 module (the same layout the merge reads
// at runtime). FuncHash here is the CFG hash - the value the runtime record
// hooks use to key indirect-call/memop sites (the scalar sites use the
// IR-PGO-name hash instead, EJIT_VALUE_PROFILE.md §5.2).
struct ProfdLayout {
  uint64_t nameRef = 0;
  uint64_t funcHash = 0;
  uint32_t numCounters = 0;
  uint16_t numValueSites[3] = {0, 0, 0};
};
static bool readProfdLayout(Module &M, StringRef funcName, ProfdLayout &out) {
  GlobalVariable *GV = nullptr;
  for (GlobalVariable &G : M.globals())
    if (G.getName().starts_with("__profd_") &&
        G.getName().ends_with(funcName)) {
      GV = &G;
      break;
    }
  if (!GV || !GV->hasInitializer())
    return false;
  Constant *Init = GV->getInitializer();
  auto *Struct = dyn_cast<ConstantStruct>(Init);
  if (!Struct || Struct->getNumOperands() < 8)
    return false;
  out.nameRef = cast<ConstantInt>(Struct->getOperand(0))->getZExtValue();
  out.funcHash = cast<ConstantInt>(Struct->getOperand(1))->getZExtValue();
  out.numCounters = static_cast<uint32_t>(
      cast<ConstantInt>(Struct->getOperand(6))->getZExtValue());
  auto *NS = dyn_cast<ConstantDataSequential>(Struct->getOperand(7));
  if (!NS || NS->getNumElements() < 3)
    return false;
  for (unsigned i = 0; i < 3; ++i)
    out.numValueSites[i] = static_cast<uint16_t>(
        cast<ConstantInt>(NS->getElementAsConstant(i))->getZExtValue());
  return true;
}

struct Analyses {
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  Analyses() {
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  }
};

} // namespace

// Isolated ICP probe: a module with one indirect call, !prof value metadata
// attached exactly like PGOInstrumentationUse does, then the official
// PGOIndirectCallPromotion pass. No profile summary => PSI hot-count gate off,
// isolating the promotion machinery.
TEST(EJitVpPipeline, IcpPromotesWithValueMetadata) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("icp_probe", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *Callee =
      Function::Create(FunctionType::get(I32, {I32}, false),
                       GlobalValue::ExternalLinkage, "icp_tgt", M.get());
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Callee);
    IRBuilder<> B(BB);
    B.CreateRet(Callee->getArg(0));
  }
  auto *Slot =
      new GlobalVariable(*M, PointerType::getUnqual(M->getContext()), false,
                         GlobalValue::ExternalLinkage, Callee, "icp_slot");
  auto *Entry =
      Function::Create(FunctionType::get(I32, {I32}, false),
                       GlobalValue::ExternalLinkage, "icp_entry", M.get());
  CallInst *Call = nullptr;
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Entry);
    IRBuilder<> B(BB);
    Value *Fp = B.CreateLoad(PointerType::getUnqual(M->getContext()), Slot);
    Call = B.CreateCall(Callee->getFunctionType(), Fp, {Entry->getArg(0)});
    B.CreateRet(Call);
  }

  // Attach the value metadata the Use phase would have annotated.
  const uint64_t tgtHash =
      IndexedInstrProf::ComputeHash(getIRPGOFuncName(*Callee));
  NamedInstrProfRecord Rec("icp_entry", 0xABCDu, {});
  Rec.reserveSites(IPVK_IndirectCallTarget, 1);
  Rec.addValueData(IPVK_IndirectCallTarget, 0, {{tgtHash, 990}}, nullptr);
  annotateValueSite(*M, *Call, Rec, IPVK_IndirectCallTarget, 0,
                    /*MaxNumAnnotations=*/16);

  // Replicate the pipeline's PSI hot-count gate: synthesize a profile whose
  // function count is modest and install its summary on the module; the
  // promotion must survive the gate (value-site total >= hot threshold).
  {
    InstrProfWriter W;
    consumeError(W.mergeProfileKind(InstrProfKind::IRInstrumentation));
    NamedInstrProfRecord R2("icp_entry", 0xABCDu, {990, 990});
    W.addRecord(std::move(R2), 1, [](Error) {});
    auto Buf = W.writeBuffer();
    auto MB = MemoryBuffer::getMemBuffer(Buf->getBuffer());
    auto Reader = cantFail(IndexedInstrProfReader::create(std::move(MB)));
    ProfileSummary &Sum = Reader->getSummary(/*UseCS=*/false);
    M->setProfileSummary(Sum.getMD(Ctx), ProfileSummary::PSK_Instr);
    ProfileSummaryInfo PSI(*M);
    ASSERT_TRUE(PSI.hasProfileSummary());
    ASSERT_TRUE(PSI.isHotCount(990));
  }

  // Run the official promotion pass over the module.
  Analyses A;
  ModulePassManager MPM;
  MPM.addPass(PGOIndirectCallPromotion());
  MPM.run(*M, A.MAM);

  std::string IR;
  raw_string_ostream OS(IR);
  M->print(OS, nullptr);
  EXPECT_NE(IR.find("icmp eq ptr %1, @icp_tgt"), std::string::npos) << IR;
  EXPECT_NE(IR.find("call i32 @icp_tgt"), std::string::npos) << IR;
}

// Regression for the review finding "ICP address table cannot resolve
// module-internal targets": the Tier-1 capture resolves every surviving
// module function's address through ORC lookup, but internal-linkage functions
// (C `static` targets - the common indirect-call case) are normally excluded
// from the JITDylib symbol table. The Instrumented transform re-exports and
// claims surviving targets after cleanup, while dead locals are never claimed.
TEST(EJitVpPipeline, OrcLookupResolvesInternalTargets) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("vp_orc_internal", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  // Internal linkage (C static): exactly the shape ORC normally skips.
  auto *Callee = Function::Create(FunctionType::get(I32, {I32}, false),
                                  GlobalValue::InternalLinkage,
                                  "vp_internal_tgt", M.get());
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Callee);
    IRBuilder<> B(BB);
    B.CreateRet(B.CreateAdd(Callee->getArg(0), ConstantInt::get(I32, 1)));
  }
  // This definition is removed by the common GlobalDCE. It must not be
  // preclaimed by ORC, otherwise JITLink reports MissingSymbolDefinitions
  // when the transformed module no longer defines it.
  auto *Dead = Function::Create(FunctionType::get(I32, {}, false),
                                GlobalValue::InternalLinkage,
                                "vp_internal_dead", M.get());
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Dead);
    IRBuilder<> B(BB);
    B.CreateRet(ConstantInt::get(I32, 42));
  }
  // An originally external helper is also internalized before addIRModule.
  // This catches stale claims that would otherwise be created for a
  // non-local definition and then invalidated by the optimizer.
  auto *ExternalDead = Function::Create(FunctionType::get(I32, {}, false),
                                        GlobalValue::ExternalLinkage,
                                        "vp_external_dead", M.get());
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", ExternalDead);
    IRBuilder<> B(BB);
    B.CreateRet(ConstantInt::get(I32, 43));
  }
  auto *Slot = new GlobalVariable(*M, PointerType::getUnqual(M->getContext()),
                                  false, GlobalValue::InternalLinkage, Callee,
                                  "vp_internal_slot");
  auto *Entry = Function::Create(FunctionType::get(I32, {I32}, false),
                                 GlobalValue::ExternalLinkage,
                                 "vp_internal_entry", M.get());
  markEJitEntry(*Entry);
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Entry);
    IRBuilder<> B(BB);
    Value *Fp = B.CreateLoad(PointerType::getUnqual(M->getContext()), Slot);
    B.CreateRet(
        B.CreateCall(Callee->getFunctionType(), Fp, {Entry->getArg(0)}));
  }

  std::string bitcode;
  {
    raw_string_ostream OS(bitcode);
    WriteBitcodeToFile(*M, OS);
    OS.flush();
  }
  ASSERT_FALSE(bitcode.empty());

  EJitRuntimeState state;
  Config cfg;
  auto engineOrErr = EJitOrcEngine::Create(cfg, state.getRegistry(), state);
  ASSERT_TRUE(static_cast<bool>(engineOrErr));
  auto engine = std::move(*engineOrErr);
  SpecializationContext ctx;
  ctx.fnName = "vp_internal_entry";
  ctx.cacheKey = 0x77;
  ctx.tier = CompileTier::Instrumented;
  engine->setActiveContext(&ctx);
  ASSERT_FALSE(errorToBool(
      engine->loadBitcodeModule(bitcode, 0x77, "vp_internal_entry")));
  ASSERT_TRUE(static_cast<bool>(engine->lookup(0x77, "vp_internal_entry")));

  // The capture must carry the internal callee, and - the point of this test
  // - its address must resolve through ORC lookup so the driver's verified
  // target table gets an entry for it.
  bool sawCallee = false;
  for (const EJitVpFunctionInfo &info : engine->getLastVpFunctions())
    if (info.name == "vp_internal_tgt") {
      sawCallee = true;
      EXPECT_NE(info.pgoHash, 0u);
    }
  EXPECT_TRUE(sawCallee);
  for (const EJitVpFunctionInfo &info : engine->getLastVpFunctions())
    EXPECT_NE(info.name, "vp_internal_dead");
  for (const EJitVpFunctionInfo &info : engine->getLastVpFunctions())
    EXPECT_NE(info.name, "vp_external_dead");
  auto calleeOrErr = engine->lookup(0x77, "vp_internal_tgt");
  ASSERT_TRUE(static_cast<bool>(calleeOrErr))
      << "ORC lookup of an internal-linkage target must succeed in the "
         "Instrumented tier (post-cleanup re-export + claim)";
  EXPECT_NE(*calleeOrErr, nullptr);
}

TEST(EJitVpPipeline, Tier1CaptureToTier2GuardedSpecialization) {
  LLVMContext Ctx;

  // --- Tier-1: instrument + capture ----------------------------------------
  std::unique_ptr<Module> M1 = makeVpModule(Ctx, /*AddCleanupFixtures=*/true);

  PeriodArrayRegistry reg;
  EJitOptimizer opt(reg);
  SpecializationContext sc;
  sc.fnName = "vp_entry";
  sc.tier = CompileTier::Instrumented;
  opt.runPipeline(*M1, sc);

  // NOTE: the hashes must come from the CAPTURE (getLastVpFunctions), not a
  // recomputation on the post-pipeline module - phase-1 internalization
  // changes the IR-level PGO name (internal linkage adds the module-name
  // prefix), and the capture records the hash computed at that state, before
  // it re-exports the functions for ORC symbol resolution. The production
  // driver consumes exactly these captured values.
  uint64_t calleeHash = 0, entryHash = 0;
  bool sawCallee = false, sawEntry = false;
  for (const EJitVpFunctionInfo &info : opt.getLastVpFunctions()) {
    if (info.name == "vp_callee") {
      calleeHash = info.pgoHash;
      sawCallee = true;
    }
    if (info.name == "vp_entry") {
      entryHash = info.pgoHash;
      sawEntry = true;
    }
  }
  ASSERT_TRUE(sawCallee && sawEntry);

  // Tier-1 must contain the scalar instrumentation call.
  {
    bool sawRecord = false;
    for (Function &F : *M1)
      for (BasicBlock &BB : F)
        for (Instruction &I : BB)
          if (auto *CI = dyn_cast<CallInst>(&I))
            if (CI->getCalledFunction() &&
                CI->getCalledFunction()->getName() == "ejit_vp_record_scalar")
              sawRecord = true;
    EXPECT_TRUE(sawRecord);
  }

  // The capture must carry every function with its IR-PGO-name hash; the
  // scalar site count of the entry function must be 1.
  const EJitVpFunctionInfo *entryInfo = nullptr;
  for (const EJitVpFunctionInfo &info : opt.getLastVpFunctions()) {
    if (info.name == "vp_entry") {
      entryInfo = &info;
      break;
    }
  }
  ASSERT_TRUE(entryInfo);
  EXPECT_EQ(entryInfo->pgoHash, entryHash);
  EXPECT_EQ(entryInfo->numScalarSites, 1u);

  // The shared cleanup prefix must remove the unreferenced local callee,
  // reduce the local helper signature in both Gen and Use, and preserve the
  // externalized #156-style declaration.
  EXPECT_EQ(M1->getFunction("vp_cleanup_dead_callee"), nullptr);
  Function *DeadArg = M1->getFunction("vp_cleanup_dead_arg");
  ASSERT_NE(DeadArg, nullptr);
  EXPECT_EQ(DeadArg->arg_size(), 0u);
  Function *Externalized = M1->getFunction("vp_cleanup_externalized");
  ASSERT_NE(Externalized, nullptr);
  EXPECT_TRUE(Externalized->isDeclaration());
  bool SawDeadArgCall = false;
  for (BasicBlock &BB : *M1->getFunction("vp_entry"))
    for (Instruction &I : BB)
      if (auto *CI = dyn_cast<CallInst>(&I))
        if (CI->getCalledFunction() == DeadArg) {
          SawDeadArgCall = true;
          EXPECT_EQ(CI->arg_size(), 0u);
        }
  EXPECT_TRUE(SawDeadArgCall);

  // --- Merge: fabricate the runtime payloads the driver would read ---------
  ProfdLayout layout;
  ASSERT_TRUE(readProfdLayout(*M1, "vp_entry", layout));
  ASSERT_GE(layout.numValueSites[0], 1u);
  ASSERT_GE(layout.numValueSites[1], 1u);

  // Fabricated edge counters: all 990 so the profile summary's hot-count
  // percentile threshold equals the value-site total (the annotateValueSite
  // total is the sum of the top-N value counts, 990 here) - ICP promotes a
  // site only when that total is "hot" relative to the summary.
  std::vector<uint64_t> counters(layout.numCounters, 990);
  struct alignas(8) FakeProfd {
    uint64_t nameRef = 0, funcHash = 0, counterPtr = 0, bitmapPtr = 0,
             functionPtr = 0, values = 0;
    uint32_t numCounters = 0;
    uint16_t numValueSites[3] = {0, 0, 0};
    uint32_t numBitmapBytes = 0;
  } pd;
  // The record's FuncHash is the CFG hash the Tier-1 __profd_ carries (the
  // runtime hooks key IC/memop sites by it; the scalar sites use the name
  // hash instead - EJIT_VALUE_PROFILE.md §5.2).
  pd.nameRef = entryHash;
  pd.funcHash = layout.funcHash;
  pd.numCounters = layout.numCounters;
  pd.numValueSites[0] = layout.numValueSites[0];
  pd.numValueSites[1] = layout.numValueSites[1];
  pd.numValueSites[2] = layout.numValueSites[2];

  PgoCounterRef refs[] = {{"vp_entry",
                           reinterpret_cast<uintptr_t>(counters.data()),
                           reinterpret_cast<uintptr_t>(&pd)}};

  // Verified target table: the runtime address 0x4000 maps to the callee's
  // IR-PGO-name hash (what ICP resolves through the module symtab).
  constexpr uintptr_t kFakeAddr = 0x4000;
  PgoValueTarget targets[] = {{kFakeAddr, calleeHash}};
  PgoValueFunction funcs[] = {{layout.funcHash, /*pgoNameHash=*/entryHash,
                               layout.numValueSites[0], layout.numValueSites[1],
                               /*numScalarSites=*/1}};
  EJitVpSiteSample samples[] = {
      {ejitVpSiteKey(layout.nameRef, kEJitVpIndirectCall, 0),
       1000,
       {kFakeAddr, 0},
       {990, 0}},
      {ejitVpSiteKey(layout.nameRef, kEJitVpMemOpSize, 0),
       1000,
       {64, 0},
       {990, 0}},
      {ejitVpSiteKey(entryHash, kEJitVpScalar, 0), 1000, {100, 0}, {990, 0}},
  };

  SmallVector<PgoValueSite, 4> valueSites;
  SmallVector<PgoScalarSite, 4> scalarSites;
  ASSERT_TRUE(
      aggregateValueSamples(samples, funcs, targets, valueSites, scalarSites));
  ASSERT_EQ(valueSites.size(), 2u);
  ASSERT_EQ(scalarSites.size(), 1u);
  std::string profile = synthesizeProfileBuffer(refs, valueSites);
  ASSERT_FALSE(profile.empty());

  // The synthesized profile must round-trip through the official reader with
  // edge AND value data (one legal profile).
  {
    std::unique_ptr<MemoryBuffer> Buf = MemoryBuffer::getMemBuffer(profile);
    auto Reader = cantFail(IndexedInstrProfReader::create(std::move(Buf)));
    auto RecOrErr = Reader->getInstrProfRecord("vp_entry", layout.funcHash);
    ASSERT_TRUE(static_cast<bool>(RecOrErr));
    NamedInstrProfRecord Rec = std::move(*RecOrErr);
    EXPECT_EQ(Rec.Hash, layout.funcHash);
    EXPECT_EQ(Rec.Counts.size(), layout.numCounters);
    EXPECT_EQ(Rec.getNumValueSites(IPVK_IndirectCallTarget),
              layout.numValueSites[0]);
    EXPECT_EQ(Rec.getNumValueSites(IPVK_MemOPSize), layout.numValueSites[1]);
    EXPECT_EQ(Rec.getValueArrayForSite(IPVK_IndirectCallTarget, 0)[0].Value,
              calleeHash);
  }

  // --- Tier-2: PGOUse + ICP + guarded scalar specialization -----------------
  std::unique_ptr<Module> M2 = makeVpModule(Ctx, /*AddCleanupFixtures=*/true);
  EJitOptimizer opt2(reg);
  SpecializationContext sc2;
  sc2.fnName = "vp_entry";
  sc2.tier = CompileTier::PGOUse;
  sc2.profileData = profile;
  sc2.scalarValueSites.assign(scalarSites.begin(), scalarSites.end());
  opt2.runPipeline(*M2, sc2);

  std::string IR;
  {
    raw_string_ostream OS(IR);
    M2->print(OS, nullptr);
  }
  // The scalar guard must survive the whole optimization pipeline (the hot
  // and cold paths differ, so it cannot be folded away).
  EXPECT_NE(IR.find("icmp eq i32 %0, 100"), std::string::npos);
  // ICP produced the target guard, then the module inliner consumed the hot
  // direct call. The generic indirect fallback remains elsewhere in the IR.
  EXPECT_NE(IR.find("icmp eq ptr %3, @vp_callee"), std::string::npos) << IR;
  EXPECT_EQ(IR.find("call i32 @vp_callee"), std::string::npos) << IR;
  // The generic fallback (cold clone) must still exist; its runtime-bound
  // dependency is covered semantically by the LLJIT equivalence tests in
  // EJitVpScalarSpecTest (the O2 pipeline may re-form the cold loop too).
  EXPECT_NE(IR.find(".vp.cold"), std::string::npos);
  EXPECT_EQ(M2->getFunction("vp_cleanup_dead_callee"), nullptr);
  Function *DeadArg2 = M2->getFunction("vp_cleanup_dead_arg");
  ASSERT_NE(DeadArg2, nullptr);
  EXPECT_EQ(DeadArg2->arg_size(), 0u);
  Function *Externalized2 = M2->getFunction("vp_cleanup_externalized");
  ASSERT_NE(Externalized2, nullptr);
  EXPECT_TRUE(Externalized2->isDeclaration());
  // Constant propagation evidence in the hot path: the 100-trip loop either
  // folds to its closed form (sum 0..99 = 4950, when the pipeline fully
  // unrolls) or, when unrolling is legitimately declined, keeps a comparison
  // against the CONSTANT bound. Both prove the hot path no longer depends on
  // the runtime bound - never fabricated as profit, only reported.
  bool hotConstPropagated =
      IR.find(", 4950") != std::string::npos ||
      IR.find("icmp slt i32 %i.next, 100") != std::string::npos;
  EXPECT_TRUE(hotConstPropagated) << IR;
}

#endif // EJIT_SRE_PGO_VALUE_PROFILE
