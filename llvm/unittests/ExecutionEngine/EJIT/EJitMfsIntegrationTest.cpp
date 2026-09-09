//===-- EJitMfsIntegrationTest.cpp - real MFS publication cycle ----------===//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ProfileData/InstrProfWriter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

// Only the SRE boundary is simulated. These have the repository's declared
// public primitive signatures and perform actual host RW/NX <-> RX changes.
extern "C" {
alignas(2097152) unsigned char __ejit_code_start[39845888];
alignas(2097152) unsigned char __ejit_cold_start[2097152];
}
asm(".global __ejit_code_end\n"
    ".set __ejit_code_end, __ejit_code_start + 39845888\n"
    ".global __ejit_cold_end\n"
    ".set __ejit_cold_end, __ejit_cold_start + 2097152\n");

namespace {
struct PlatformState {
  bool FailOwnerCold = false;
  bool FailPeerCold = false;
  std::vector<std::pair<uint32_t, uintptr_t>> Seals;
  std::vector<std::pair<void *, size_t>> Mappings;
  bool isCold(uintptr_t PC) const {
    auto Base = reinterpret_cast<uintptr_t>(__ejit_cold_start);
    return PC >= Base && PC - Base < sizeof(__ejit_cold_start);
  }
} Platform;
} // namespace

extern "C" void *SRE_MemDbgAlloc(unsigned int, unsigned char, unsigned long N,
                                 const char *, unsigned int) {
  void *P = mmap(nullptr, N, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (P == MAP_FAILED)
    return nullptr;
  Platform.Mappings.emplace_back(P, N);
  return P;
}
extern "C" unsigned split_2m_to_4k(unsigned long long, unsigned long long) {
  return 0; // Host mappings already use 4KiB pages.
}
extern "C" unsigned enable_rw(unsigned, unsigned long long VA) {
  return mprotect(reinterpret_cast<void *>(VA), 4096, PROT_READ | PROT_WRITE)
             ? 1u
             : 0u;
}
extern "C" unsigned enable_ex(unsigned, unsigned long long VA) {
  const auto Core = EJitCoreId::current();
  Platform.Seals.emplace_back(Core, VA);
  if (Platform.isCold(VA) && ((Core == 6 && Platform.FailOwnerCold) ||
                              (Core == 16 && Platform.FailPeerCold)))
    return 1;
  auto *P = reinterpret_cast<char *>(VA);
  if (mprotect(P, 4096, PROT_READ | PROT_EXEC))
    return 1;
  __builtin___clear_cache(P, P + 4096);
  return 0;
}

namespace {
using EntryFn = int (*)(unsigned, unsigned char *);

std::string makeBitcode(unsigned Version) {
  LLVMContext C;
  Module M("mfs-real-cycle", C);
  IRBuilder<> B(C);
  auto *FT =
      FunctionType::get(B.getInt32Ty(), {B.getInt32Ty(), B.getPtrTy()}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "entry", M);
  F->setMetadata(
      MD_EJIT_METADATA,
      MDNode::get(C, {MDNode::get(C, {MDString::get(C, TAG_EJIT_ENTRY)})}));
  auto *Entry = BasicBlock::Create(C, "entry", F);
  auto *Hot = BasicBlock::Create(C, "hot", F);
  auto *Cold = BasicBlock::Create(C, "cold", F);
  B.SetInsertPoint(Entry);
  B.CreateCondBr(B.CreateICmpEQ(F->getArg(0), B.getInt32(0)), Hot, Cold);
  B.SetInsertPoint(Hot);
  B.CreateRet(B.getInt32(Version));
  B.SetInsertPoint(Cold);
  for (unsigned I = 0; I != 96; ++I) {
    auto *P = B.CreateGEP(B.getInt8Ty(), F->getArg(1), B.getInt64(I));
    B.CreateStore(B.getInt8(I + Version), P)->setVolatile(true);
  }
  B.CreateRet(B.getInt32(-static_cast<int>(Version)));
  std::string Bytes;
  raw_string_ostream OS(Bytes);
  WriteBitcodeToFile(M, OS);
  return Bytes;
}

struct Cycle {
  EJitRuntimeState Runtime;
  std::unique_ptr<EJitOrcEngine> Engine;
  std::unique_ptr<EJitSharedTaskPoolState> Shared =
      std::make_unique<EJitSharedTaskPoolState>();
  EJitSharedTaskPool Pool;
  uintptr_t Profc = 0, Profd = 0;
  void *LastT2 = nullptr;
  bool OmitProfile = false;
  bool MismatchProfile = false;
  std::string LastProfile;
  std::string ErrorText;
  std::vector<uint64_t> T1Counts;

  bool initialize() {
    Platform.FailOwnerCold = Platform.FailPeerCold = false;
    Platform.Seals.clear();
    if (mprotect(__ejit_code_start, sizeof(__ejit_code_start),
                 PROT_READ | PROT_WRITE) ||
        mprotect(__ejit_cold_start, sizeof(__ejit_cold_start),
                 PROT_READ | PROT_WRITE))
      return false;
    EJitCoreId::setCurrentForTest(6);
    auto E = EJitOrcEngine::Create(Config{}, Runtime.getRegistry(), Runtime);
    if (!E) {
      ErrorText = toString(E.takeError());
      return false;
    }
    Engine = std::move(*E);
    Pool.bind(Shared.get());
    Pool.setMode(EJitCompileMode::Async);
    Pool.setCodeSharingEnabled(true);
    Pool.setSealMode(true);
    Pool.setPgoEnabled(true, 64);
    Pool.setCompiler(
        [](void *Ctx, const EJitCompileRequest &Req, void **Out) {
          return static_cast<Cycle *>(Ctx)->compile(Req, Out);
        },
        this);
    Pool.setCodeRangeProvider(
        [](void *Ctx, const void *Fn, EJitCompiledCodeInfo *Out) {
          auto &E = *static_cast<Cycle *>(Ctx)->Engine;
          return E.findCodeRange(Fn, *Out) || E.findPendingCodeRange(Fn, *Out);
        },
        this);
    Pool.setCodeBatchCallbacks(
        [](void *Ctx, const void *Fn) {
          return static_cast<Cycle *>(Ctx)->Engine->isCodeReady(Fn);
        },
        [](void *Ctx) {
          return !errorToBool(
              static_cast<Cycle *>(Ctx)->Engine->flushPendingCode());
        },
        this);
    Pool.setCodeBatchPoolFlushCallback(
        [](void *Ctx, uint32_t Id) {
          return !errorToBool(
              static_cast<Cycle *>(Ctx)->Engine->flushPendingCode(Id));
        },
        this);
    Pool.setSplitPoolCallback(
        [](void *, uintptr_t Base, uint64_t Size) {
          return split_2m_to_4k(Base, Size) == 0;
        },
        nullptr);
    Pool.setSealPageCallback(
        [](void *, uintptr_t VA) { return enable_ex(1, VA) == 0; }, nullptr);
    Pool.setEnableRwPageCallback(
        [](void *, uintptr_t VA) { return enable_rw(1, VA) == 0; }, nullptr);
    if (Pool.init() != EJitSharedTaskPool::InitResult::BecameOwner)
      return false;
    Pool.setInstanceEnabled(0, 0, true);
    return true;
  }

  bool compile(const EJitCompileRequest &Req, void **Out) {
    SpecializationContext C;
    C.fnName = "entry";
    C.cacheKey = Req.versions[0] + 1;
    C.optLevel = OptimizationLevel::L2;
    const bool Use = decodeReqTier(Req.funcIndex) == kEJitTierPgoUse;
    C.tier = Use ? CompileTier::PGOUse : CompileTier::Instrumented;
    if (Use && !OmitProfile)
      C.profileData = synthesizeProfileBuffer({{"entry", Profc, Profd}});
    if (Use && MismatchProfile) {
      InstrProfWriter Writer;
      cantFail(Writer.mergeProfileKind(InstrProfKind::IRInstrumentation));
      Writer.addRecord(NamedInstrProfRecord("entry", 0, {64, 0}), 1,
                       [](Error E) { cantFail(std::move(E)); });
      C.profileData = Writer.writeBuffer()->getBuffer().str();
    }
    if (Use)
      LastProfile = C.profileData;
    Engine->setActiveContext(&C);
    auto ResetContext = [&] { Engine->setActiveContext(nullptr); };
    if (auto Err = Engine->loadBitcodeModule(makeBitcode(Req.versions[0] + 1),
                                             C.cacheKey, "entry",
                                             Use ? 0u : kEJitFarPoolId)) {
      ErrorText = toString(std::move(Err));
      ResetContext();
      return false;
    }
    auto Fn = Engine->lookup(C.cacheKey, "entry");
    if (!Fn) {
      ErrorText = toString(Fn.takeError());
      ResetContext();
      return false;
    }
    *Out = *Fn;
    if (!Use) {
      auto Ctr = Engine->lookup(C.cacheKey, "__profc_entry");
      auto Data = Engine->lookup(C.cacheKey, "__profd_entry");
      if (!Ctr || !Data) {
        if (!Ctr)
          ErrorText = toString(Ctr.takeError());
        if (!Data)
          ErrorText += toString(Data.takeError());
        ResetContext();
        return false;
      }
      Profc = reinterpret_cast<uintptr_t>(*Ctr);
      Profd = reinterpret_cast<uintptr_t>(*Data);
    } else {
      LastT2 = *Out;
    }
    ResetContext();
    return true;
  }

  EJitSharedCacheSlot *slot() {
    for (auto &B : Shared->buckets)
      for (auto &S : B.slots)
        if (S.funcIndex == 1 &&
            S.state.loadAcquire() ==
                static_cast<uint32_t>(EJitSharedSlotState::Ready))
          return &S;
    return nullptr;
  }
  EJitSharedTaskPool::CompileOrGetResult get() {
    EJitDimPair D{0, 0};
    return Pool.compileOrGet(1, &D, 1, nullptr);
  }
  bool snapshotT1Counts() {
    if (!Profc || !Profd)
      return false;
    uint32_t NumCounters = 0;
    std::memcpy(&NumCounters, reinterpret_cast<const void *>(Profd + 48),
                sizeof(NumCounters));
    if (NumCounters == 0 || NumCounters > 64)
      return false;
    const auto *Counters = reinterpret_cast<const uint64_t *>(Profc);
    T1Counts.clear();
    for (uint32_t I = 0; I != NumCounters; ++I)
      T1Counts.push_back(__atomic_load_n(&Counters[I], __ATOMIC_RELAXED));
    return true;
  }
  bool train(unsigned ColdSamples = 0) {
    if (ColdSamples > 64)
      return false;
    EJitCoreId::setCurrentForTest(16);
    if (get().status != EJitCompileOrGetStatus::EnqueuedPending)
      return false;
    EJitCoreId::setCurrentForTest(6);
    if (!Pool.pollOne())
      return false;
    unsigned char Data[96] = {};
    EJitCoreId::setCurrentForTest(16);
    for (unsigned I = 0; I != 64; ++I) {
      auto Hit = get();
      if (Hit.status != EJitCompileOrGetStatus::CacheHit)
        return false;
      reinterpret_cast<EntryFn>(Hit.fnPtr)(I < ColdSamples ? 1 : 0, Data);
      if (Hit.hasReadToken)
        Pool.releaseRead(Hit.bucketIndex);
    }
    if (!snapshotT1Counts())
      return false;
    EJitCoreId::setCurrentForTest(6);
    return Pool.pollOne() && LastT2;
  }
  ~Cycle() { EJitCoreId::resetForTest(); }
};
} // namespace

TEST(EJitMfsIntegration, RealTwoVersionsFirstColdAndPeerFailureRetry) {
  Cycle C;
  ASSERT_TRUE(C.initialize()) << C.ErrorText;
  uintptr_t PreviousHot = 0, PreviousCold = 0;
  for (unsigned Version = 0; Version != 2; ++Version) {
    EJitCoreId::setCurrentForTest(16);
    EXPECT_EQ(C.get().status, EJitCompileOrGetStatus::EnqueuedPending);
    EJitCoreId::setCurrentForTest(6);
    ASSERT_TRUE(C.Pool.pollOne()) << C.ErrorText;
    ASSERT_NE(C.slot(), nullptr) << C.ErrorText;
    EXPECT_EQ(C.slot()->poolId, kEJitFarPoolId);
    unsigned char Data[96] = {};
    int Expected = 0;
    EJitCoreId::setCurrentForTest(16);
    for (unsigned I = 0; I != 64; ++I) {
      auto Hit = C.get();
      ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
      Expected = reinterpret_cast<EntryFn>(Hit.fnPtr)(0, Data);
      if (Hit.hasReadToken)
        C.Pool.releaseRead(Hit.bucketIndex);
    }
    ASSERT_TRUE(C.snapshotT1Counts());
    ASSERT_EQ(C.T1Counts.size(), 2u);
    EXPECT_EQ(C.T1Counts[0], 64u);
    EXPECT_EQ(C.T1Counts[1], 0u);
    EJitCoreId::setCurrentForTest(6);
    ASSERT_TRUE(C.Pool.pollOne()) << C.ErrorText;
    ASSERT_NE(C.LastT2, nullptr) << C.ErrorText;
    EXPECT_FALSE(C.Engine->isCodeReady(C.LastT2));
    EJitCompiledCodeInfo Pending;
    ASSERT_TRUE(C.Engine->findPendingCodeRange(C.LastT2, Pending));
    ASSERT_TRUE(Pending.cold.valid());
    EXPECT_EQ(Pending.poolId, 0u);
    EXPECT_NE(Pending.codeStart, PreviousHot);
    EXPECT_NE(Pending.cold.codeStart, PreviousCold);
    PreviousHot = Pending.codeStart;
    PreviousCold = Pending.cold.codeStart;
    ASSERT_TRUE(C.Pool.flushCodeBatch()) << C.ErrorText;
    ASSERT_TRUE(C.Engine->isCodeReady(C.LastT2));
    ASSERT_EQ(C.slot()->tier.loadRelaxed(), kEJitTierPgoUse);

    EJitCoreId::setCurrentForTest(16);
    Platform.FailPeerCold = true;
    auto Failed = C.get();
    EXPECT_TRUE(Failed.readyButNotShareable);
    EXPECT_FALSE(Failed.hasReadToken);
    EXPECT_EQ(C.slot()->executableCoreMask.loadAcquire() & (uint64_t{1} << 16),
              0u);
    Platform.FailPeerCold = false;
    auto Hit = C.get();
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
    auto Fn = reinterpret_cast<EntryFn>(Hit.fnPtr);
    EXPECT_EQ(Fn(0, Data), Expected);
    EXPECT_EQ(Fn(1, Data), -Expected); // first cold execution, after T2 publish
    for (unsigned I = 0; I != 96; ++I)
      EXPECT_EQ(Data[I], static_cast<unsigned char>(I + Expected));
    for (unsigned I = 0; I != 64; ++I) {
      EXPECT_EQ(Fn(0, Data), Expected);
      EXPECT_EQ(Fn(1, Data), -Expected);
    }
    if (Hit.hasReadToken)
      C.Pool.releaseRead(Hit.bucketIndex);
    C.Pool.setInstanceEnabled(0, 0, false);
    C.Pool.setInstanceEnabled(0, 0, true);
  }
}

TEST(EJitMfsIntegration, MissingMismatchAndOneOf64ProfilesStayUnsplit) {
  for (unsigned Control = 0; Control != 3; ++Control) {
    Cycle C;
    C.OmitProfile = Control == 0;
    C.MismatchProfile = Control == 1;
    ASSERT_TRUE(C.initialize()) << C.ErrorText;
    ASSERT_TRUE(C.train(/*ColdSamples=*/Control == 2 ? 1u : 0u)) << C.ErrorText;
    ASSERT_EQ(C.T1Counts.size(), 2u);
    EXPECT_EQ(C.T1Counts[0], 64u);
    EXPECT_EQ(C.T1Counts[1], Control == 2 ? 1u : 0u);
    EJitCompiledCodeInfo Pending;
    ASSERT_TRUE(C.Engine->findPendingCodeRange(C.LastT2, Pending));
    EXPECT_TRUE(Pending.cold.empty()) << "control=" << Control;
    EXPECT_EQ(C.Engine->getTieredCodePoolStats().cold.usedBytes, 0u);
    ASSERT_TRUE(C.Pool.flushCodeBatch());
    EJitCoreId::setCurrentForTest(16);
    auto Hit = C.get();
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
    unsigned char Data[96] = {};
    auto Fn = reinterpret_cast<EntryFn>(Hit.fnPtr);
    EXPECT_EQ(Fn(1, Data), -Fn(0, Data));
    if (Hit.hasReadToken)
      C.Pool.releaseRead(Hit.bucketIndex);
  }
}

TEST(EJitMfsIntegration, ColdOwnerFailureNeverPublishesAndCanRetry) {
  Cycle C;
  ASSERT_TRUE(C.initialize()) << C.ErrorText;
  ASSERT_TRUE(C.train()) << C.ErrorText;
  void *FailedT2 = C.LastT2;
  EJitCompiledCodeInfo Pending;
  ASSERT_TRUE(C.Engine->findPendingCodeRange(FailedT2, Pending));
  ASSERT_TRUE(Pending.cold.valid());
  Platform.FailOwnerCold = true;
  EXPECT_FALSE(C.Pool.flushCodeBatch());
  EXPECT_FALSE(C.Engine->isCodeReady(FailedT2));
  ASSERT_NE(C.slot(), nullptr);
  EXPECT_EQ(C.slot()->tier.loadRelaxed(), kEJitTierInstrumented);
  EXPECT_EQ(C.Pool.classifyTier2PC(Pending.codeStart), 0u);
  Platform.FailOwnerCold = false;
  unsigned char Data[96] = {};
  for (unsigned I = 0; I != 192 && !C.Pool.pendingPublishCount(); ++I) {
    EJitCoreId::setCurrentForTest(16);
    auto Hit = C.get();
    if (Hit.status == EJitCompileOrGetStatus::CacheHit) {
      reinterpret_cast<EntryFn>(Hit.fnPtr)(0, Data);
      if (Hit.hasReadToken)
        C.Pool.releaseRead(Hit.bucketIndex);
    }
    EJitCoreId::setCurrentForTest(6);
    C.Pool.pollOne();
  }
  ASSERT_GT(C.Pool.pendingPublishCount(), 0u) << C.ErrorText;
  ASSERT_TRUE(C.Pool.flushCodeBatch());
  ASSERT_EQ(C.slot()->tier.loadRelaxed(), kEJitTierPgoUse);
  EJitCoreId::setCurrentForTest(16);
  auto Hit = C.get();
  ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
  auto Fn = reinterpret_cast<EntryFn>(Hit.fnPtr);
  EXPECT_EQ(Fn(1, Data), -Fn(0, Data));
  if (Hit.hasReadToken)
    C.Pool.releaseRead(Hit.bucketIndex);
}

TEST(EJitMfsIntegration, EmitActualAArch64BigEndianMfsOnOffObjects) {
  const char *Directory = std::getenv("EJIT_MFS_ARTIFACT_DIR");
  if (!Directory)
    GTEST_SKIP() << "Set EJIT_MFS_ARTIFACT_DIR to retain BE validation objects";
  Cycle C;
  ASSERT_TRUE(C.initialize()) << C.ErrorText;
  ASSERT_TRUE(C.train()) << C.ErrorText;
  ASSERT_FALSE(C.LastProfile.empty());
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  std::string Error;
  const auto *Target =
      TargetRegistry::lookupTarget("aarch64_be-none-elf", Error);
  ASSERT_NE(Target, nullptr) << Error;
  for (bool Split : {false, true}) {
    TargetOptions Options;
    Options.EnableMachineFunctionSplitter = Split;
    std::unique_ptr<TargetMachine> TM(Target->createTargetMachine(
        Triple("aarch64_be-none-elf"), "generic", "", Options, Reloc::Static,
        CodeModel::Small, CodeGenOptLevel::Default));
    ASSERT_NE(TM, nullptr);
    LLVMContext Context;
    const std::string Input = makeBitcode(2);
    auto M =
        cantFail(parseBitcodeFile(MemoryBufferRef(Input, "be-mfs"), Context));
    M->setTargetTriple(TM->getTargetTriple());
    M->setDataLayout(TM->createDataLayout());
    EJitOptimizer Optimizer(C.Runtime.getRegistry());
    SpecializationContext Spec;
    Spec.fnName = "entry";
    Spec.tier = CompileTier::PGOUse;
    Spec.optLevel = OptimizationLevel::L2;
    Spec.profileData = C.LastProfile;
    Optimizer.runPipeline(*M, Spec);
    Optimizer.clearAnalyses();
    ASSERT_TRUE(
        M->getFunction("entry")->hasFnAttribute("ejit-mfs-zero-count-only"));
    SmallVector<char, 0> Bytes;
    raw_svector_ostream OS(Bytes);
    legacy::PassManager PM;
    ASSERT_FALSE(
        TM->addPassesToEmitFile(PM, OS, nullptr, CodeGenFileType::ObjectFile));
    PM.run(*M);
    ASSERT_GT(Bytes.size(), 6u);
    EXPECT_EQ(static_cast<unsigned char>(Bytes[5]), 2u); // ELF EI_DATA=MSB
    auto Object = cantFail(object::ObjectFile::createObjectFile(
        MemoryBufferRef(StringRef(Bytes.data(), Bytes.size()), "be-mfs.o")));
    unsigned ColdSections = 0;
    for (const auto &Section : Object->sections())
      if (cantFail(Section.getName()).starts_with(".text.split.")) {
        ++ColdSections;
        EXPECT_GT(Section.getSize(), 0u);
      }
    EXPECT_EQ(ColdSections != 0, Split);
    std::error_code EC;
    const auto Path =
        std::string(Directory) +
        (Split ? "/mfs-on-aarch64_be.o" : "/mfs-off-aarch64_be.o");
    raw_fd_ostream File(Path, EC, sys::fs::OF_None);
    ASSERT_FALSE(EC) << EC.message();
    File.write(Bytes.data(), Bytes.size());
  }
}
