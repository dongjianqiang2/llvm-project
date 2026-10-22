//===-- EJitGlobalLayoutIntegrationTest.cpp - Real ORC/PGO layout tests
//-----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitCompileDriver.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#if defined(__linux__) && defined(EJIT_SRE_CODE_POOL)
#include <cstdint>
#include <sys/mman.h>
extern "C" {
alignas(2097152) unsigned char ejitLayoutTestNear[16 * 1024 * 1024];
void *SRE_MemDbgAlloc(unsigned int, unsigned char, unsigned long size,
                      const char *, unsigned int) {
  void *P = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return P == MAP_FAILED ? nullptr : P;
}
unsigned split_2m_to_4k(unsigned long long, unsigned long long) { return 0; }
unsigned enable_rw(unsigned, unsigned long long address) {
  return mprotect(reinterpret_cast<void *>(address), 4096,
                  PROT_READ | PROT_WRITE) == 0
             ? 0
             : 1;
}
unsigned enable_ex(unsigned, unsigned long long address) {
  auto *P = reinterpret_cast<char *>(address);
  __builtin___clear_cache(P, P + 4096);
  return mprotect(P, 4096, PROT_READ | PROT_EXEC) == 0 ? 0 : 1;
}
}
asm(".global __ejit_code_start\n"
    ".set __ejit_code_start, ejitLayoutTestNear\n"
    ".global __ejit_code_end\n"
    ".set __ejit_code_end, ejitLayoutTestNear + 16777216\n");
#endif

extern "C" int layout_host_callback(int x) { return x * 3 + 1; }

using namespace llvm;
using namespace llvm::ejit;
namespace {
std::string realBusinessBitcode(unsigned count, bool helperLoop,
                                bool boundConfig = false) {
  LLVMContext C;
  Module M("global-layout-business", C);
  M.setTargetTriple(Triple("aarch64-unknown-linux-gnu"));
  auto *Ty =
      FunctionType::get(Type::getInt32Ty(C), {Type::getInt32Ty(C)}, false);
  auto *Helper =
      Function::Create(Ty, GlobalValue::InternalLinkage, "helper", M);
  Helper->addFnAttr(Attribute::NoInline);
  IRBuilder<> H(BasicBlock::Create(C, "entry", Helper));
  if (helperLoop) {
    auto *Counter = H.CreateAlloca(H.getInt32Ty());
    H.CreateStore(H.getInt32(0), Counter, true);
    auto *Cond = BasicBlock::Create(C, "cond", Helper);
    auto *Body = BasicBlock::Create(C, "loop", Helper);
    auto *Exit = BasicBlock::Create(C, "exit", Helper);
    H.CreateBr(Cond);
    H.SetInsertPoint(Cond);
    auto *I = H.CreateLoad(H.getInt32Ty(), Counter, true);
    H.CreateCondBr(H.CreateICmpSLT(I, Helper->getArg(0)), Body, Exit);
    H.SetInsertPoint(Body);
    H.CreateStore(H.CreateAdd(I, H.getInt32(1)), Counter, true);
    H.CreateBr(Cond);
    H.SetInsertPoint(Exit);
  }
  H.CreateRet(H.CreateAdd(H.CreateMul(Helper->getArg(0), H.getInt32(3)),
                          H.getInt32(1)));
  SmallVector<Type *, 3> Args{Type::getInt32Ty(C), PointerType::getUnqual(C)};
  if (boundConfig)
    Args.push_back(PointerType::getUnqual(C));
  auto *RootTy = FunctionType::get(Type::getInt32Ty(C), Args, false);
  auto *Host = Function::Create(Ty, GlobalValue::ExternalLinkage,
                                "layout_host_callback", M);
  for (unsigned I = 0; I < count; ++I) {
    auto *F = Function::Create(RootTy, GlobalValue::ExternalLinkage,
                               "layout_" + std::to_string(I), M);
    auto *Entry = MDNode::get(C, {MDString::get(C, TAG_EJIT_ENTRY)});
    SmallVector<Metadata *, 2> Meta{Entry};
    if (boundConfig)
      Meta.push_back(MDNode::get(C, {
          MDString::get(C, TAG_EJIT_BOUND_PTR), MDString::get(C, "cfg"),
          ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(C), 2))}));
    F->setMetadata(MD_EJIT_METADATA, MDNode::get(C, Meta));
    auto *Start = BasicBlock::Create(C, "entry", F);
    auto *Positive = BasicBlock::Create(C, "positive", F);
    auto *Negative = BasicBlock::Create(C, "negative", F);
    IRBuilder<> B(Start);
    B.CreateCondBr(B.CreateICmpSGT(F->getArg(0), B.getInt32(0)), Positive,
                   Negative);
    B.SetInsertPoint(Positive);
    auto *Indirect = B.CreateCall(Ty, F->getArg(1), {F->getArg(0)});
    auto *Sum = B.CreateAdd(B.CreateCall(Helper, {F->getArg(0)}), Indirect);
    auto *Result = B.CreateAdd(Sum, B.getInt32(I));
    if (boundConfig)
      Result = B.CreateAdd(Result, B.CreateLoad(B.getInt32Ty(), F->getArg(2)));
    B.CreateRet(Result);
    B.SetInsertPoint(Negative);
    auto *Cold = B.CreateAdd(B.CreateCall(Helper, {F->getArg(0)}),
                             B.CreateCall(Host, {F->getArg(0)}));
    B.CreateRet(B.CreateSub(B.getInt32(I), Cold));
  }
  std::string BC;
  raw_string_ostream OS(BC);
  WriteBitcodeToFile(M, OS);
  return BC;
}

void runProductionLayout(bool pgo, unsigned start, bool permuted,
                         unsigned arrival = 0, bool helperLoop = false,
                         bool auditOnly = false, bool seedDefaults = false) {
#if !defined(__linux__) || !defined(EJIT_SRE_CODE_POOL) ||                     \
    !defined(EJIT_CODE_POOL_BATCHED_PUBLISH) ||                                \
    !defined(EJIT_SRE_SHARED_TASKPOOL)
  GTEST_SKIP() << "requires Linux and production batched shared code pool";
#else
  ASSERT_EQ(mprotect(ejitLayoutTestNear, sizeof(ejitLayoutTestNear),
                     PROT_READ | PROT_WRITE),
            0);
  EJitFuncRegistry::instance().reset();
  Config config;
  config.enablePgo = pgo && !auditOnly;
  config.enableProfileAudit = auditOnly;
  config.globalLayoutBudgets.maxCollectionPolls = 40000;
  const unsigned threshold = seedDefaults ? 64u : 8u;
  const unsigned count = pgo ? 20 : 26;
  std::vector<unsigned> rank;
  for (unsigned I = 0; I < count; ++I)
    rank.push_back(permuted ? (I * 7) % count : I);
  for (unsigned I : rank) {
    EJitLayoutTarget Target;
    Target.entryName = "layout_" + std::to_string(I);
    config.globalLayout.push_back(Target);
  }
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  EJitRuntimeState Runtime;
  EJitModuleLoader Loader;
  std::string BC = realBusinessBitcode(count, helperLoop);
  for (unsigned I = 0; I < count; ++I)
    ASSERT_TRUE(Loader.registerBitcode(
        "layout_" + std::to_string(I),
        reinterpret_cast<const uint8_t *>(BC.data()), BC.size()));
  EJitCompileDriver Driver(config, Runtime, Loader);
  Driver.registerSymbol("layout_host_callback",
                        reinterpret_cast<void *>(&layout_host_callback));
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  EJitVpStatsOut VpBefore{};
  ejitVpStatsSnapshot(VpBefore);
#endif
  auto *Pool = Driver.sharedTaskPool();
  Pool->bind(Shared.get());
  Pool->setWorkerHooks(nullptr, nullptr, nullptr);
  if (!seedDefaults)
    Pool->setPgoEnabled(pgo, 8, 4);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  std::vector<unsigned> executed(count, 0);
  unsigned maxActive = 0, genuineSamples = 0;
  bool observedDrain = false, observedWave = false;
  for (unsigned Step = 0;
       Step < 12000 &&
       Pool->globalLayoutState() != EJitGlobalLayoutState::Complete &&
       Pool->globalLayoutState() != EJitGlobalLayoutState::Failed;
       ++Step) {
    unsigned I = arrival == 1   ? count - 1 - (Step % count)
                 : arrival == 2 ? (Step * 7 + start) % count
                                : (Step + start) % count;
    auto R = Pool->compileOrGet(I, nullptr, 0, nullptr);
    if (R.fnPtr) {
      if (pgo) {
        EXPECT_GE(R.bucketIndex, 0x8000u);
        EXPECT_TRUE(R.hasReadToken);
      }
      // Drive the worker while the real return lease is still held.
      uint32_t ReadyBefore = Shared->globalLayoutReady.loadAcquire();
      Pool->workerPollOnce();
      if (pgo && ReadyBefore == Shared->globalLayoutReady.loadAcquire() &&
          Shared->inFlight[I].loadAcquire() != 0)
        observedDrain = true;
      auto Fn = reinterpret_cast<int (*)(int, int (*)(int))>(R.fnPtr);
      EXPECT_EQ(Fn(10, &layout_host_callback), 62 + static_cast<int>(I));
      ++executed[I];
      ++genuineSamples;
      Pool->releaseRead(R.bucketIndex);
    }
    Pool->workerPollOnce();
    EJitSharedDiagnostics D;
    Pool->getDiagnostics(D);
    maxActive = std::max(maxActive, D.pgoActiveFunctionCount);
    ASSERT_LE(D.pgoActiveFunctionCount, 4u);
    if (Shared->globalLayoutReady.loadAcquire() < count) {
      if ((Step % 3) == 0) {
        EXPECT_TRUE(Pool->flushCodeBatch());
        EXPECT_EQ(Pool->globalLayoutState(), EJitGlobalLayoutState::Collecting);
        EXPECT_EQ(Shared->globalLayoutPublished.loadAcquire(), 0u);
      }
      EXPECT_EQ(Driver.getJitEngine()->getTieredCodePoolStats().near.usedBytes,
                0u)
          << "final code allocated before every artifact was ready";
    }
    if (Shared->globalLayoutReady.loadAcquire() >= 4 &&
        std::count_if(executed.begin(), executed.end(),
                      [](unsigned n) { return n != 0; }) > 4)
      observedWave = true;
  }
  ASSERT_EQ(Pool->globalLayoutState(), EJitGlobalLayoutState::Complete)
      << "error=" << Shared->globalLayoutError.loadAcquire()
      << "rank=" << Shared->globalLayoutErrorRank.loadAcquire()
      << "ready=" << Shared->globalLayoutReady.loadAcquire();
  EXPECT_EQ(Shared->globalLayoutPublished.loadAcquire(), count);
  EXPECT_EQ(Shared->globalLayoutArtifactBytes.loadAcquire(), 0u);
  uintptr_t Previous = 0;
  for (unsigned I : rank) {
    auto R = Pool->compileOrGet(I, nullptr, 0, nullptr);
    ASSERT_NE(R.fnPtr, nullptr);
    uintptr_t Address = reinterpret_cast<uintptr_t>(R.fnPtr);
    EXPECT_GT(Address, Previous);
    Previous = Address;
    EJitCompiledCodeInfo Range{};
    ASSERT_TRUE(Driver.getJitEngine()->findCodeRange(R.fnPtr, Range));
    EXPECT_EQ(Range.poolKind, EJitCodePoolKind::Near);
    EXPECT_GE(Address, Range.codeStart);
    EXPECT_LT(Address - Range.codeStart, Range.codeSize);
    auto Fn = reinterpret_cast<int (*)(int, int (*)(int))>(R.fnPtr);
    EXPECT_EQ(Fn(10, &layout_host_callback), 62 + static_cast<int>(I));
    EXPECT_EQ(Fn(-10, &layout_host_callback), static_cast<int>(I) + 58);
    Pool->releaseRead(R.bucketIndex);
  }
  if (pgo) {
    EXPECT_EQ(maxActive, 4u);
    EXPECT_GE(genuineSamples, 20u * threshold);
    EXPECT_TRUE(observedDrain);
    EXPECT_TRUE(observedWave);
    for (unsigned Count : executed)
      EXPECT_EQ(Count, threshold);
    EXPECT_EQ(Shared->pgoActiveFunctionCount.loadAcquire(), 0u);
    EXPECT_EQ(Shared->pgoCompletedFunctions.loadAcquire(), 20u);
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    EJitVpStatsOut VpAfter{};
    ejitVpStatsSnapshot(VpAfter);
    if (!auditOnly)
      EXPECT_GT(VpAfter.icValueSites, VpBefore.icValueSites);
    EXPECT_EQ(Driver.layoutPreparedValueSamples(),
              auditOnly ? 0u : 20u * threshold)
        << "each of 20 real 8-hit indirect-call profiles must survive sibling "
           "snapshots";
#endif
  }
  Driver.stopSharedTaskPool();
#endif
}

// Incomplete-manifest contract: functions that are registered but NOT listed
// keep compiling through the ordinary paths. During collection their Baseline
// misses park in the deferred batch backlog; at the collective point they
// compile AFTER the manifest bodies, the single seal covers everything, and
// they publish through the ordinary flush once Complete.
void runUnlistedLayout(unsigned listed, unsigned registered) {
#if !defined(__linux__) || !defined(EJIT_SRE_CODE_POOL) ||                     \
    !defined(EJIT_CODE_POOL_BATCHED_PUBLISH) ||                                \
    !defined(EJIT_SRE_SHARED_TASKPOOL)
  GTEST_SKIP() << "requires Linux and production batched shared code pool";
#else
  ASSERT_EQ(mprotect(ejitLayoutTestNear, sizeof(ejitLayoutTestNear),
                     PROT_READ | PROT_WRITE),
            0);
  EJitFuncRegistry::instance().reset();
  Config config;
  config.enablePgo = false;
  config.enableProfileAudit = false;
  config.globalLayoutBudgets.maxCollectionPolls = 40000;
  for (unsigned I = 0; I < listed; ++I) {
    EJitLayoutTarget Target;
    Target.entryName = "layout_" + std::to_string(I);
    config.globalLayout.push_back(Target);
  }
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  EJitRuntimeState Runtime;
  EJitModuleLoader Loader;
  std::string BC = realBusinessBitcode(registered, false);
  for (unsigned I = 0; I < registered; ++I)
    ASSERT_TRUE(Loader.registerBitcode(
        "layout_" + std::to_string(I),
        reinterpret_cast<const uint8_t *>(BC.data()), BC.size()));
  EJitCompileDriver Driver(config, Runtime, Loader);
  Driver.registerSymbol("layout_host_callback",
                        reinterpret_cast<void *>(&layout_host_callback));
  auto *Pool = Driver.sharedTaskPool();
  Pool->bind(Shared.get());
  Pool->setWorkerHooks(nullptr, nullptr, nullptr);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  bool sawParkedUnlisted = false;
  for (unsigned Step = 0;
       Step < 12000 &&
       Pool->globalLayoutState() != EJitGlobalLayoutState::Complete &&
       Pool->globalLayoutState() != EJitGlobalLayoutState::Failed;
       ++Step) {
    // Reverse arrival: the unlisted functions miss FIRST, so their batch
    // parks are deterministically in place before the manifest completes.
    unsigned I = registered - 1 - (Step % registered);
    Pool->compileOrGet(I, nullptr, 0, nullptr);
    Pool->workerPollOnce();
    if (Shared->globalLayoutReady.loadAcquire() < listed)
      sawParkedUnlisted |=
          Shared->globalLayoutUnlistedReady.loadAcquire() != 0;
  }
  ASSERT_EQ(Pool->globalLayoutState(), EJitGlobalLayoutState::Complete)
      << "error=" << Shared->globalLayoutError.loadAcquire()
      << "rank=" << Shared->globalLayoutErrorRank.loadAcquire();
  EXPECT_TRUE(sawParkedUnlisted)
      << "unlisted misses must park during collection";
  EXPECT_EQ(Shared->globalLayoutPublished.loadAcquire(), listed);
  EXPECT_EQ(Shared->globalLayoutUnlistedReady.loadAcquire(), 0u);
  EXPECT_EQ(Shared->globalLayoutUnlistedAppended.loadAcquire(),
            registered - listed);
  // The collective appended the unlisted bodies owner-private; the ordinary
  // flush publishes them now that the layout is Complete.
  for (unsigned Step = 0; Step < 1000 && !Pool->flushCodeBatch(); ++Step)
    Pool->workerPollOnce();
  uintptr_t LastListed = 0;
  for (unsigned I = 0; I < listed; ++I) {
    auto R = Pool->compileOrGet(I, nullptr, 0, nullptr);
    ASSERT_NE(R.fnPtr, nullptr);
    uintptr_t Address = reinterpret_cast<uintptr_t>(R.fnPtr);
    EXPECT_GT(Address, LastListed);
    LastListed = Address;
    EJitCompiledCodeInfo Range{};
    ASSERT_TRUE(Driver.getJitEngine()->findCodeRange(R.fnPtr, Range));
    EXPECT_EQ(Range.poolKind, EJitCodePoolKind::Near);
    auto Fn = reinterpret_cast<int (*)(int, int (*)(int))>(R.fnPtr);
    EXPECT_EQ(Fn(10, &layout_host_callback), 62 + static_cast<int>(I));
    Pool->releaseRead(R.bucketIndex);
  }
  for (unsigned I = listed; I < registered; ++I) {
    auto R = Pool->compileOrGet(I, nullptr, 0, nullptr);
    ASSERT_NE(R.fnPtr, nullptr);
    uintptr_t Address = reinterpret_cast<uintptr_t>(R.fnPtr);
    EXPECT_GT(Address, LastListed)
        << "unlisted layout_" << I << " must append after the manifest region";
    EJitCompiledCodeInfo Range{};
    ASSERT_TRUE(Driver.getJitEngine()->findCodeRange(R.fnPtr, Range));
    EXPECT_EQ(Range.poolKind, EJitCodePoolKind::Near);
    auto Fn = reinterpret_cast<int (*)(int, int (*)(int))>(R.fnPtr);
    EXPECT_EQ(Fn(10, &layout_host_callback), 62 + static_cast<int>(I));
    Pool->releaseRead(R.bucketIndex);
  }
  Driver.stopSharedTaskPool();
#endif
}
} // namespace
TEST(EJitGlobalLayoutIntegration, BaselineFromH) {
  runProductionLayout(false, 7, false);
}
TEST(EJitGlobalLayoutIntegration,
     IncompleteManifestInitializesAndAppendsUnlisted) {
  runUnlistedLayout(5, 8);
}

static void runOrdinaryLayoutPgo(unsigned Arrival, bool ExhaustProfileBudget = false) {
#if !defined(__linux__) || !defined(EJIT_SRE_CODE_POOL) || \
    !defined(EJIT_CODE_POOL_BATCHED_PUBLISH) || !defined(EJIT_SRE_SHARED_TASKPOOL)
  GTEST_SKIP() << "requires Linux and production batched shared code pool";
#else
  ASSERT_EQ(mprotect(ejitLayoutTestNear, sizeof(ejitLayoutTestNear),
                     PROT_READ | PROT_WRITE), 0);
  EJitFuncRegistry::instance().reset();
  Config Config;
  Config.enablePgo = true;
  Config.enableProfileAudit = false;
  Config.globalLayoutBudgets.maxCollectionPolls = Arrival == 2 ? 180 : 20000;
  if (ExhaustProfileBudget)
    Config.globalLayoutBudgets.maxProfileBytes = 1;
  for (unsigned I = 0; I < 2; ++I) {
    EJitLayoutTarget Target;
    Target.entryName = "layout_" + std::to_string(I);
    Config.globalLayout.push_back(Target);
  }
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  EJitRuntimeState Runtime;
  EJitModuleLoader Loader;
  std::string BC = realBusinessBitcode(4, false);
  for (unsigned I = 0; I < 4; ++I)
    ASSERT_TRUE(Loader.registerBitcode("layout_" + std::to_string(I),
        reinterpret_cast<const uint8_t *>(BC.data()), BC.size()));
  EJitCompileDriver Driver(Config, Runtime, Loader);
  Driver.registerSymbol("layout_host_callback",
                        reinterpret_cast<void *>(&layout_host_callback));
  auto *Pool = Driver.sharedTaskPool();
  Pool->bind(Shared.get());
  Pool->setWorkerHooks(nullptr, nullptr, nullptr);
  Pool->setPgoEnabled(true, 64, 4);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  unsigned Samples[4] = {};
  uintptr_t Bodies[4] = {};
  for (unsigned Step = 0; Step < (ExhaustProfileBudget ? 1600u : 12000u); ++Step) {
    const auto Phase = Pool->globalLayoutState();
    const bool Terminal = Phase == EJitGlobalLayoutState::Complete ||
                          Phase == EJitGlobalLayoutState::Failed;
    unsigned I = Terminal ? Step % 4 : Arrival == 0 ? (Step + 2) % 3
                                     : Arrival == 1 ? Step % 2 : 0;
    auto R = Pool->compileOrGet(I, nullptr, 0, nullptr);
    if (R.fnPtr) {
      const bool Sample = R.hasReadToken && (R.bucketIndex & 0x8000u);
      Pool->workerPollOnce(); // Last sample deliberately has not executed yet.
      auto Fn = reinterpret_cast<int (*)(int, int (*)(int))>(R.fnPtr);
      EXPECT_EQ(Fn(10, &layout_host_callback), 62 + static_cast<int>(I));
      if (Sample)
        ++Samples[I];
      else
        Bodies[I] = reinterpret_cast<uintptr_t>(R.fnPtr);
      Pool->releaseRead(R.bucketIndex);
    }
    Pool->workerPollOnce();
    ASSERT_LE(Shared->pgoActiveFunctionCount.loadAcquire(), 4u);
    if (Bodies[0] && Bodies[1] && Bodies[2] && Bodies[3] &&
        Shared->pgoActiveFunctionCount.loadAcquire() == 0 &&
        Pool->pendingCount() == 0)
      break;
  }
  if (ExhaustProfileBudget) {
    EXPECT_EQ(Pool->globalLayoutState(), EJitGlobalLayoutState::Failed);
    EXPECT_EQ(Shared->counters.tier2Compiles.loadRelaxed(), 0u);
    EXPECT_EQ(Shared->pgoCompletedFunctions.loadAcquire(), 0u);
    EXPECT_EQ(Samples[0], 64u);
    for (auto Body : Bodies)
      EXPECT_EQ(Body, 0u) << "a consumed snapshot cannot become partial T2";
    Driver.stopSharedTaskPool();
    return;
  }
  EXPECT_EQ(Pool->globalLayoutState(), Arrival == 2
      ? EJitGlobalLayoutState::Failed : EJitGlobalLayoutState::Complete);
  for (unsigned I = 0; I < 4; ++I) {
    EXPECT_EQ(Samples[I], 64u) << "root=" << I;
    EXPECT_NE(Bodies[I], 0u) << "root=" << I;
  }
  if (Arrival != 2) {
    EXPECT_LT(Bodies[0], Bodies[1]);
    EXPECT_LT(Bodies[1], Bodies[2]);
    EXPECT_LT(Bodies[1], Bodies[3]);
  }
  EXPECT_EQ(Shared->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(Shared->counters.tier2Compiles.loadRelaxed(), 4u);
  EXPECT_EQ(Shared->pgoCompletedFunctions.loadAcquire(), 4u);
  EXPECT_EQ(Shared->counters.compileFailed.loadRelaxed(), 0u);
  EXPECT_EQ(Shared->counters.publishFailed.loadRelaxed(), 0u);
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  EXPECT_FALSE(ejitVpIsArmed()) << "every layout and ordinary round must close";
  EXPECT_EQ(Driver.layoutPreparedValueSamples(), 4u * 64u)
      << "all sibling VP snapshots and failed-layout retries retain full samples";
#endif
  Driver.stopSharedTaskPool();
#endif
}

TEST(EJitGlobalLayoutIntegration, UnlistedPgo64WithRealReturnLeases) {
  runOrdinaryLayoutPgo(0);
}
TEST(EJitGlobalLayoutIntegration, LateUnlistedPgo64AfterComplete) {
  runOrdinaryLayoutPgo(1);
}
TEST(EJitGlobalLayoutIntegration, OrdinaryPgo64AfterMissingMemberFailure) {
  runOrdinaryLayoutPgo(2);
}
TEST(EJitGlobalLayoutIntegration, ProfileBudgetFailureNeverPublishesPartialT2) {
  runOrdinaryLayoutPgo(1, true);
}

TEST(EJitGlobalLayoutIntegration, ParkedBoundIdentitiesKeepSeparateProfiles) {
#if !defined(__linux__) || !defined(EJIT_SRE_CODE_POOL) || \
    !defined(EJIT_CODE_POOL_BATCHED_PUBLISH) || !defined(EJIT_SRE_SHARED_TASKPOOL)
  GTEST_SKIP();
#else
  ASSERT_EQ(mprotect(ejitLayoutTestNear, sizeof(ejitLayoutTestNear),
                     PROT_READ | PROT_WRITE), 0);
  EJitFuncRegistry::instance().reset();
  Config Config;
  Config.enablePgo = true;
  Config.enableProfileAudit = false;
  EJitLayoutTarget Target;
  Target.entryName = "layout_0";
  Config.globalLayout.push_back(Target);
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  EJitRuntimeState Runtime;
  EJitModuleLoader Loader;
  std::string BC = realBusinessBitcode(2, false, true);
  for (unsigned I = 0; I < 2; ++I)
    ASSERT_TRUE(Loader.registerBitcode("layout_" + std::to_string(I),
        reinterpret_cast<const uint8_t *>(BC.data()), BC.size()));
  EJitCompileDriver Driver(Config, Runtime, Loader);
  Driver.registerSymbol("layout_host_callback",
                        reinterpret_cast<void *>(&layout_host_callback));
  auto *Pool = Driver.sharedTaskPool();
  Pool->bind(Shared.get());
  Pool->setWorkerHooks(nullptr, nullptr, nullptr);
  Pool->setPgoEnabled(true, 64, 4);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  int Backing[4] = {100, 200, 300, 400};
  unsigned Samples[4] = {};
  uintptr_t Bodies[4] = {};
  auto Call = [&](unsigned I) {
    unsigned F = I == 3 ? 0 : 1;
    EJitBoundPtrDescriptor Bound{&Backing[I], sizeof(int), 2};
    auto R = Pool->compileOrGet(F, nullptr, 0, nullptr, &Bound, 1);
    if (R.fnPtr) {
      EXPECT_EQ(reinterpret_cast<int (*)(int, int (*)(int), int *)>(R.fnPtr)(
                    10, &layout_host_callback, &Backing[I]),
                62 + static_cast<int>(F) + Backing[I]);
      if (R.hasReadToken && (R.bucketIndex & 0x8000u))
        ++Samples[I];
      else
        Bodies[I] = reinterpret_cast<uintptr_t>(R.fnPtr);
      Pool->releaseRead(R.bucketIndex);
    }
    Pool->workerPollOnce();
  };
  // Three drained identities share funcIndex/dims but not bound backing. They
  // all wait for the still-cold manifest member before any T2 consumes profiles.
  for (unsigned I = 0; I < 3; ++I) {
    for (unsigned Step = 0; Step < 200; ++Step)
      Call(I);
    ASSERT_EQ(Samples[I], 64u);
    ASSERT_EQ(Shared->globalLayoutUnlistedReady.loadAcquire(), I + 1);
  }
  for (unsigned Step = 0; Step < 1000; ++Step) {
    Call(Step % 4);
    if (Bodies[0] && Bodies[1] && Bodies[2] && Bodies[3])
      break;
  }
  EXPECT_EQ(Pool->globalLayoutState(), EJitGlobalLayoutState::Complete);
  EXPECT_EQ(Shared->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(Shared->pgoCompletedFunctions.loadAcquire(), 4u);
  for (unsigned I = 0; I < 4; ++I) {
    EXPECT_EQ(Samples[I], 64u);
    EXPECT_NE(Bodies[I], 0u);
    if (I != 3)
      EXPECT_GT(Bodies[I], Bodies[3]);
  }
  EXPECT_EQ(Shared->counters.compileFailed.loadRelaxed(), 0u);
  EXPECT_EQ(Shared->counters.publishFailed.loadRelaxed(), 0u);
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  EXPECT_EQ(Driver.layoutPreparedValueSamples(), 256u);
  EXPECT_FALSE(ejitVpIsArmed());
#endif
  Driver.stopSharedTaskPool();
#endif
}
TEST(EJitGlobalLayoutIntegration, BaselineFromZ) {
  runProductionLayout(false, 25, false);
}
TEST(EJitGlobalLayoutIntegration, ExpandedManifestRankOverridesDimensionSort) {
  runProductionLayout(false, 7, true);
}
TEST(EJitGlobalLayoutIntegration, FourAdmissionsTwentyRealSampledTargets) {
  runProductionLayout(true, 7, false);
}

TEST(EJitGlobalLayoutIntegration, BaselineFromA) {
  runProductionLayout(false, 0, false);
}
TEST(EJitGlobalLayoutIntegration, BaselineReverseArrivals) {
  runProductionLayout(false, 25, false, 1);
}
TEST(EJitGlobalLayoutIntegration, BaselinePermutedArrivals) {
  runProductionLayout(false, 7, false, 2);
}
TEST(EJitGlobalLayoutIntegration,
     FourSlotsPrivateHelperScalarAndIndirectProfiles) {
  runProductionLayout(true, 7, false, 0, true);
}

namespace {
int layout_mutable_probe_backing = 123;
std::string privateProbeBitcode() {
  LLVMContext C;
  Module M("private-data-layout", C);
  M.setTargetTriple(Triple("aarch64-unknown-linux-gnu"));
  auto *G = new GlobalVariable(
      M, Type::getInt32Ty(C), false, GlobalValue::InternalLinkage,
      ConstantInt::get(Type::getInt32Ty(C), 1), "layout_mutable_probe");
  auto *F =
      Function::Create(FunctionType::get(Type::getInt32Ty(C), false),
                       GlobalValue::ExternalLinkage, "private_data_probe", M);
  auto *Tag = MDNode::get(C, {MDString::get(C, TAG_EJIT_ENTRY)});
  F->setMetadata(MD_EJIT_METADATA, MDNode::get(C, {Tag}));
  IRBuilder<> B(BasicBlock::Create(C, "entry", F));
  B.CreateRet(B.CreateLoad(B.getInt32Ty(), G, true));
  std::string BC;
  raw_string_ostream OS(BC);
  WriteBitcodeToFile(M, OS);
  return BC;
}
} // namespace

TEST(EJitGlobalLayoutIntegration, PrivateMutableBackingIsNeverCopied) {
#if !defined(EJIT_SRE_CODE_POOL) || !defined(EJIT_CODE_POOL_BATCHED_PUBLISH)
  GTEST_SKIP();
#else
  Config C;
  C.enableProfileAudit = false;
  PeriodArrayRegistry Registry;
  EJitRuntimeState Runtime;
  auto E = EJitOrcEngine::Create(C, Registry, Runtime);
  ASSERT_TRUE(static_cast<bool>(E));
  SpecializationContext S;
  S.fnName = "private_data_probe";
  S.globalLayoutCompilation = true;
  (*E)->setActiveContext(&S);
  auto O = (*E)->prepareBitcodeModule(privateProbeBitcode(), 0, S.fnName);
  (*E)->setActiveContext(nullptr);
  EXPECT_FALSE(static_cast<bool>(O));
  if (!O)
    consumeError(O.takeError());
  EXPECT_EQ((*E)->getTieredCodePoolStats().total.usedBytes, 0u);
#endif
}

TEST(EJitGlobalLayoutIntegration, RegisteredMutableBackingRemainsObservable) {
#if !defined(EJIT_SRE_CODE_POOL) || !defined(EJIT_CODE_POOL_BATCHED_PUBLISH)
  GTEST_SKIP();
#else
  Config C;
  C.enableProfileAudit = false;
  PeriodArrayRegistry Registry;
  Registry.registerStaticVar("layout_mutable_probe",
                             &layout_mutable_probe_backing);
  EJitRuntimeState Runtime;
  auto E = EJitOrcEngine::Create(C, Registry, Runtime);
  ASSERT_TRUE(static_cast<bool>(E));
  SpecializationContext S;
  S.fnName = "private_data_probe";
  S.globalLayoutCompilation = true;
  (*E)->setActiveContext(&S);
  auto O = (*E)->prepareBitcodeModule(privateProbeBitcode(), 0, S.fnName);
  (*E)->setActiveContext(nullptr);
  ASSERT_TRUE(static_cast<bool>(O));
  EXPECT_EQ((*E)->getTieredCodePoolStats().near.usedBytes, 0u);
  auto F = (*E)->linkPreparedObject(**O);
  ASSERT_TRUE(static_cast<bool>(F));
  EXPECT_FALSE((*E)->isCodeReady(*F));
  auto Err = (*E)->flushPendingCode();
  ASSERT_FALSE(static_cast<bool>(Err));
  auto Fn = reinterpret_cast<int (*)()>(*F);
  layout_mutable_probe_backing = 123;
  EXPECT_EQ(Fn(), 123);
  layout_mutable_probe_backing = 7;
  EXPECT_EQ(Fn(), 7); // Not the bitcode initializer, not a private duplicate.
#endif
}

TEST(EJitGlobalLayoutIntegration,
     ShutdownDefersUntilActualReturnLeaseReleased) {
#if !defined(__linux__) || !defined(EJIT_SRE_CODE_POOL) ||                     \
    !defined(EJIT_CODE_POOL_BATCHED_PUBLISH) ||                                \
    !defined(EJIT_SRE_SHARED_TASKPOOL)
  GTEST_SKIP();
#else
  EJitFuncRegistry::instance().reset();
  Config C;
  C.enablePgo = true;
  C.globalLayoutBudgets.maxCollectionPolls = 64;
  for (unsigned I = 0; I < 20; ++I) {
    EJitLayoutTarget T;
    T.entryName = "layout_" + std::to_string(I);
    C.globalLayout.push_back(T);
  }
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  EJitRuntimeState Runtime;
  EJitModuleLoader Loader;
  auto BC = realBusinessBitcode(20, false);
  for (unsigned I = 0; I < 20; ++I)
    ASSERT_TRUE(Loader.registerBitcode(
        "layout_" + std::to_string(I),
        reinterpret_cast<const uint8_t *>(BC.data()), BC.size()));
  EJitCompileDriver Driver(C, Runtime, Loader);
  Driver.registerSymbol("layout_host_callback",
                        reinterpret_cast<void *>(&layout_host_callback));
  auto *P = Driver.sharedTaskPool();
  P->bind(Shared.get());
  P->setWorkerHooks(nullptr, nullptr, nullptr);
  P->setPgoEnabled(true, 8, 4);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  P->compileOrGet(0, nullptr, 0, nullptr);
  P->workerPollOnce();
  auto R = P->compileOrGet(0, nullptr, 0, nullptr);
  ASSERT_NE(R.fnPtr, nullptr);
  auto Fn = reinterpret_cast<int (*)(int, int (*)(int))>(R.fnPtr);
  EXPECT_EQ(Fn(10, &layout_host_callback), 62);
  uint32_t Generation = Shared->generation.loadAcquire();
  P->ownerShutdown();
  EXPECT_TRUE(P->isOwner());
  EXPECT_TRUE(Driver.hasJitEngine());
  EXPECT_EQ(Shared->generation.loadAcquire(), Generation);
  EXPECT_EQ(Shared->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Stopping));
  P->releaseRead(R.bucketIndex);
  P->ownerShutdown();
  EXPECT_FALSE(P->isOwner());
  EXPECT_FALSE(Driver.hasJitEngine());
#endif
}

TEST(EJitGlobalLayoutIntegration, RealReadOnlyGraphsCrossMultipleTwoMiBPools) {
#if !defined(__linux__) || !defined(EJIT_SRE_CODE_POOL) ||                     \
    !defined(EJIT_CODE_POOL_BATCHED_PUBLISH) ||                                \
    !defined(EJIT_SRE_SHARED_TASKPOOL)
  GTEST_SKIP();
#else
  ASSERT_EQ(mprotect(ejitLayoutTestNear, sizeof(ejitLayoutTestNear),
                     PROT_READ | PROT_WRITE),
            0);
  EJitFuncRegistry::instance().reset();
  constexpr unsigned Count = 8, Bytes = 512 * 1024;
  LLVMContext Ctx;
  Module M("large-read-only-layout", Ctx);
  M.setTargetTriple(Triple("aarch64-unknown-linux-gnu"));
  for (unsigned I = 0; I < Count; ++I) {
    std::vector<uint8_t> Data(Bytes);
    for (unsigned J = 0; J < Bytes; ++J)
      Data[J] = static_cast<uint8_t>((J * 37 + I * 11) % 251);
    auto *Init = ConstantDataArray::get(Ctx, Data);
    auto *G = new GlobalVariable(M, Init->getType(), true,
                                 GlobalValue::InternalLinkage, Init,
                                 "table_" + std::to_string(I));
    G->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    auto *F = Function::Create(
        FunctionType::get(Type::getInt32Ty(Ctx), {Type::getInt32Ty(Ctx)},
                          false),
        GlobalValue::ExternalLinkage, "large_" + std::to_string(I), M);
    auto *Tag = MDNode::get(Ctx, {MDString::get(Ctx, TAG_EJIT_ENTRY)});
    F->setMetadata(MD_EJIT_METADATA, MDNode::get(Ctx, {Tag}));
    IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
    auto *Index = B.CreateAnd(F->getArg(0), B.getInt32(Bytes - 1));
    auto *P = B.CreateInBoundsGEP(Init->getType(), G, {B.getInt32(0), Index});
    B.CreateRet(
        B.CreateZExt(B.CreateLoad(B.getInt8Ty(), P, true), B.getInt32Ty()));
  }
  std::string BC;
  raw_string_ostream OS(BC);
  WriteBitcodeToFile(M, OS);
  Config Config;
  Config.enableProfileAudit = false;
  for (unsigned I = 0; I < Count; ++I) {
    EJitLayoutTarget T;
    T.entryName = "large_" + std::to_string(I);
    Config.globalLayout.push_back(T);
  }
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  EJitRuntimeState Runtime;
  EJitModuleLoader Loader;
  for (unsigned I = 0; I < Count; ++I)
    ASSERT_TRUE(Loader.registerBitcode(
        "large_" + std::to_string(I),
        reinterpret_cast<const uint8_t *>(BC.data()), BC.size()));
  EJitCompileDriver Driver(Config, Runtime, Loader);
  auto *P = Driver.sharedTaskPool();
  P->bind(Shared.get());
  P->setWorkerHooks(nullptr, nullptr, nullptr);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  for (unsigned I = 0; I < Count; ++I) {
    P->compileOrGet((I + 3) % Count, nullptr, 0, nullptr);
    P->workerPollOnce();
    EXPECT_EQ(Driver.getJitEngine()->getTieredCodePoolStats().near.usedBytes,
              0u);
  }
  P->workerPollOnce();
  ASSERT_EQ(P->globalLayoutState(), EJitGlobalLayoutState::Complete)
      << Shared->globalLayoutError.loadAcquire();
  auto Stats = Driver.getJitEngine()->getTieredCodePoolStats();
  EXPECT_GT(Stats.near.poolCount, 1u);
  uintptr_t Previous = 0;
  for (unsigned I = 0; I < Count; ++I) {
    auto R = P->compileOrGet(I, nullptr, 0, nullptr);
    ASSERT_NE(R.fnPtr, nullptr);
    EXPECT_GT(reinterpret_cast<uintptr_t>(R.fnPtr), Previous);
    Previous = reinterpret_cast<uintptr_t>(R.fnPtr);
    auto Fn = reinterpret_cast<unsigned (*)(unsigned)>(R.fnPtr);
    EXPECT_EQ(Fn(123), (123 * 37 + I * 11) % 251);
    P->releaseRead(R.bucketIndex);
  }
  Driver.stopSharedTaskPool();
#endif
}

namespace {
void runCellTrpBoundLayout(bool pgo, bool refresh = false) {
#if !defined(__linux__) || !defined(EJIT_CODE_POOL_BATCHED_PUBLISH) ||         \
    !defined(EJIT_SRE_SHARED_TASKPOOL)
  GTEST_SKIP();
#else
  ASSERT_EQ(mprotect(ejitLayoutTestNear, sizeof(ejitLayoutTestNear),
                     PROT_READ | PROT_WRITE),
            0);
  EJitFuncRegistry::instance().reset();
  EJitLifecycleRegistry::instance().reset();
  ASSERT_EQ(EJitLifecycleRegistry::instance().resolveAssign("cell"), 0u);
  ASSERT_EQ(EJitLifecycleRegistry::instance().resolveAssign("trp"), 1u);
  LLVMContext C;
  Module M("cell-trp-bound-layout", C);
  M.setTargetTriple(Triple("aarch64-unknown-linux-gnu"));
  auto *Ty = FunctionType::get(
      Type::getInt32Ty(C),
      {Type::getInt32Ty(C), Type::getInt32Ty(C), PointerType::getUnqual(C)},
      false);
  for (unsigned I = 0; I < 2; ++I) {
    auto *F = Function::Create(Ty, GlobalValue::ExternalLinkage,
                               "axes_" + std::to_string(I), M);
    IRBuilder<> B(BasicBlock::Create(C, "entry", F));
    auto *V = B.CreateLoad(B.getInt32Ty(), F->getArg(2));
    B.CreateRet(
        B.CreateAdd(V, B.CreateAdd(B.CreateMul(F->getArg(0), B.getInt32(10)),
                                   B.CreateAdd(F->getArg(1), B.getInt32(I)))));
    auto Meta = [&](const char *tag, const char *name, unsigned arg) {
      return MDNode::get(C, {MDString::get(C, tag), MDString::get(C, name),
                             ConstantAsMetadata::get(B.getInt32(arg))});
    };
    F->setMetadata(
        MD_EJIT_METADATA,
        MDNode::get(C, {MDNode::get(C, {MDString::get(C, TAG_EJIT_ENTRY)}),
                        Meta(TAG_EJIT_PERIOD_ARR_IND, "cell", 0),
                        Meta(TAG_EJIT_PERIOD_ARR_IND, "trp", 1),
                        Meta(TAG_EJIT_BOUND_PTR, "cfg", 2)}));
  }
  std::string BC;
  raw_string_ostream OS(BC);
  WriteBitcodeToFile(M, OS);
  EJitRuntimeState Runtime;
  EJitModuleLoader Loader;
  for (unsigned I = 0; I < 2; ++I)
    ASSERT_TRUE(Loader.registerBitcode(
        "axes_" + std::to_string(I),
        reinterpret_cast<const uint8_t *>(BC.data()), BC.size()));
  int backing[8] = {100, 101, 102, 103, 104, 105, 106, 107};
  // Rank intentionally interleaves functions/cells/TRPs, not dimension-major.
  const unsigned order[8] = {6, 1, 4, 3, 0, 7, 2, 5};
  Config config;
  config.enablePgo = pgo;
  config.enableProfileAudit = false;
  config.globalLayoutBudgets.maxCollectionPolls = 10000;
  for (unsigned J : order) {
    EJitLayoutTarget T;
    T.entryName = "axes_" + std::to_string(J / 4);
    // Zero-initialized default policy captures cold-init/activate live
    // versions.
    T.identity.versions[0] = 0;
    T.identity.versions[1] = 0;
    T.identity.numDims = 2;
    T.identity.dims[0] = {0, (J % 4) / 2};
    T.identity.dims[1] = {1, J % 2};
    T.identity.boundCount = 1;
    T.identity.boundPointers[0].argIndex = 2;
    T.identity.boundPointers[0].rawPtr = &backing[J];
    T.identity.boundPointers[0].size = sizeof(int);
    config.globalLayout.push_back(T);
  }
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  EJitCompileDriver Driver(config, Runtime, Loader);
  auto *Pool = Driver.sharedTaskPool();
  Pool->bind(Shared.get());
  Pool->setWorkerHooks(nullptr, nullptr, nullptr);
  Pool->setPgoEnabled(pgo, 8, 4);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  for (uint32_t D = 0; D < 2; ++D)
    for (uint32_t I = 0; I < 2; ++I)
      ASSERT_TRUE(Pool->setInstanceEnabled(D, I, true));

  for (unsigned Step = 0;
       Step < 3000 &&
       Pool->globalLayoutState() != EJitGlobalLayoutState::Complete &&
       Pool->globalLayoutState() != EJitGlobalLayoutState::Failed;
       ++Step) {
    unsigned J = (Step + 3) % 8;
    const auto &T =
        config.globalLayout[std::find(std::begin(order), std::end(order), J) -
                            std::begin(order)];
    auto R = Pool->compileOrGet(J / 4, T.identity.dims, 2, nullptr,
                                T.identity.boundPointers, 1);
    if (R.fnPtr) {
      EXPECT_EQ(reinterpret_cast<int (*)(int, int, int *)>(R.fnPtr)(
                    (J % 4) / 2, J % 2, &backing[J]),
                backing[J] + 10 * ((J % 4) / 2) + J % 2 + J / 4);
      Pool->releaseRead(R.bucketIndex);
    }
    Pool->workerPollOnce();
    if (Shared->globalLayoutReady.loadAcquire() < 8)
      EXPECT_EQ(Driver.getJitEngine()->getTieredCodePoolStats().near.usedBytes,
                0u);
  }
  ASSERT_EQ(Pool->globalLayoutState(), EJitGlobalLayoutState::Complete)
      << "error=" << Shared->globalLayoutError.loadAcquire()
      << "rank=" << Shared->globalLayoutErrorRank.loadAcquire();
  uintptr_t previous = 0;
  for (const auto &T : config.globalLayout) {
    unsigned J =
        static_cast<const int *>(T.identity.boundPointers[0].rawPtr) - backing;
    auto R = Pool->compileOrGet(J / 4, T.identity.dims, 2, nullptr,
                                T.identity.boundPointers, 1);
    ASSERT_NE(R.fnPtr, nullptr);
    EXPECT_GT(reinterpret_cast<uintptr_t>(R.fnPtr), previous);
    previous = reinterpret_cast<uintptr_t>(R.fnPtr);
    EXPECT_EQ(reinterpret_cast<int (*)(int, int, int *)>(R.fnPtr)(
                  (J % 4) / 2, J % 2, &backing[J]),
              backing[J] + 10 * ((J % 4) / 2) + J % 2 + J / 4);
    Pool->releaseRead(R.bucketIndex);
  }
  if (refresh) {
    auto Targets = config.globalLayout;
    int replacementBacking = 777;
    auto stale = Targets;
    stale[0].identity.layoutVersionPolicy = 1;
    EXPECT_FALSE(Driver.refreshGlobalLayout(stale));
    ASSERT_EQ(Pool->globalLayoutState(), EJitGlobalLayoutState::Complete);
    for (unsigned Round = 0; Round < 2; ++Round) {
      uint32_t oldGen = Shared->generation.loadAcquire();
      uint64_t oldFingerprint = Shared->registrationFingerprint.loadAcquire();
      auto oldBytes =
          Driver.getJitEngine()->getTieredCodePoolStats().near.usedBytes;
      if (Round == 0) {
        ASSERT_TRUE(Pool->setInstanceEnabled(0, 1, false));
        Pool->workerPollOnce();
        ASSERT_TRUE(Pool->setInstanceEnabled(0, 1, true));
      } else {
        std::reverse(Targets.begin(), Targets.end());
        Targets.front().identity.boundPointers[0].rawPtr = &replacementBacking;
        ASSERT_TRUE(Driver.refreshGlobalLayout(Targets));
      }
      Pool->workerPollOnce();
      EXPECT_GT(Shared->generation.loadAcquire(), oldGen);
      if (Round == 0)
        EXPECT_EQ(Shared->registrationFingerprint.loadAcquire(),
                  oldFingerprint);
      else
        EXPECT_NE(Shared->registrationFingerprint.loadAcquire(),
                  oldFingerprint);
      for (unsigned Step = 0;
           Step < 4000 &&
           Pool->globalLayoutState() != EJitGlobalLayoutState::Complete &&
           Pool->globalLayoutState() != EJitGlobalLayoutState::Failed;
           ++Step) {
        const auto &T = Targets[(Step + 3) % Targets.size()];
        unsigned J = (T.entryName == "axes_1" ? 4u : 0u) +
                     2 * T.identity.dims[0].instanceId +
                     T.identity.dims[1].instanceId;
        auto R = Pool->compileOrGet(J / 4, T.identity.dims, 2, nullptr,
                                    T.identity.boundPointers, 1);
        if (R.fnPtr) {
          EXPECT_EQ(
              reinterpret_cast<int (*)(int, int, int *)>(R.fnPtr)(
                  (J % 4) / 2, J % 2,
                  const_cast<int *>(static_cast<const int *>(
                      T.identity.boundPointers[0].rawPtr))),
              *static_cast<const int *>(T.identity.boundPointers[0].rawPtr) +
                  10 * ((J % 4) / 2) + J % 2 + J / 4);
          Pool->releaseRead(R.bucketIndex);
        }
        Pool->workerPollOnce();
        if (Shared->globalLayoutReady.loadAcquire() < 8)
          EXPECT_EQ(
              Driver.getJitEngine()->getTieredCodePoolStats().near.usedBytes,
              oldBytes);
      }
      ASSERT_EQ(Pool->globalLayoutState(), EJitGlobalLayoutState::Complete)
          << "error=" << Shared->globalLayoutError.loadAcquire();
      EXPECT_EQ(Shared->globalLayoutPublished.loadAcquire(), 8u);
      uintptr_t last = 0;
      for (const auto &T : Targets) {
        unsigned J = (T.entryName == "axes_1" ? 4u : 0u) +
                     2 * T.identity.dims[0].instanceId +
                     T.identity.dims[1].instanceId;
        auto R = Pool->compileOrGet(J / 4, T.identity.dims, 2, nullptr,
                                    T.identity.boundPointers, 1);
        ASSERT_NE(R.fnPtr, nullptr);
        EXPECT_GT(reinterpret_cast<uintptr_t>(R.fnPtr), last);
        last = reinterpret_cast<uintptr_t>(R.fnPtr);
        auto *ptr = const_cast<int *>(
            static_cast<const int *>(T.identity.boundPointers[0].rawPtr));
        EXPECT_EQ(reinterpret_cast<int (*)(int, int, int *)>(R.fnPtr)(
                      (J % 4) / 2, J % 2, ptr),
                  *ptr + 10 * ((J % 4) / 2) + J % 2 + J / 4);
        Pool->releaseRead(R.bucketIndex);
        auto wrong = T.identity.boundPointers[0];
        int other = 1;
        wrong.rawPtr = &other;
        auto rejected = Pool->tryCacheHit(J / 4, T.identity.dims, 2, &wrong, 1);
        EXPECT_NE(rejected.status, EJitCompileOrGetStatus::CacheHit);
      }
    }
  }
  Driver.stopSharedTaskPool();
  EJitLifecycleRegistry::instance().reset();
#endif
}
} // namespace
TEST(EJitGlobalLayoutIntegration, RealCellTrpBoundBaseline) {
  runCellTrpBoundLayout(false);
}
TEST(EJitGlobalLayoutIntegration, RealCellTrpBoundPgo) {
  runCellTrpBoundLayout(true);
}

TEST(EJitGlobalLayoutIntegration, AuditOnlySamplesAndOrdersFinalBaseline) {
  runProductionLayout(true, 7, false, 0, false, true);
}
TEST(EJitGlobalLayoutIntegration,
     ColdCellTrpBaselineRebuildsAndReordersInProcess) {
  runCellTrpBoundLayout(false, true);
}
TEST(EJitGlobalLayoutIntegration, ColdCellTrpPgoRebuildsAndReordersInProcess) {
  runCellTrpBoundLayout(true, true);
}
TEST(EJitGlobalLayoutIntegration,
     RefreshWaitsForActualT1ReturnWithoutRestartingOwner) {
#if !defined(__linux__) || !defined(EJIT_SRE_CODE_POOL) ||                     \
    !defined(EJIT_CODE_POOL_BATCHED_PUBLISH) ||                                \
    !defined(EJIT_SRE_SHARED_TASKPOOL)
  GTEST_SKIP();
#else
  EJitFuncRegistry::instance().reset();
  Config C;
  C.enablePgo = true;
  C.globalLayoutBudgets.maxCollectionPolls = 64;
  for (unsigned I = 0; I < 20; ++I) {
    EJitLayoutTarget T;
    T.entryName = "layout_" + std::to_string(I);
    C.globalLayout.push_back(T);
  }
  auto Shared = std::make_unique<EJitSharedTaskPoolState>();
  EJitRuntimeState Runtime;
  EJitModuleLoader Loader;
  auto BC = realBusinessBitcode(20, false);
  for (unsigned I = 0; I < 20; ++I)
    ASSERT_TRUE(Loader.registerBitcode(
        "layout_" + std::to_string(I),
        reinterpret_cast<const uint8_t *>(BC.data()), BC.size()));
  EJitCompileDriver Driver(C, Runtime, Loader);
  Driver.registerSymbol("layout_host_callback",
                        reinterpret_cast<void *>(&layout_host_callback));
  auto *P = Driver.sharedTaskPool();
  P->bind(Shared.get());
  P->setWorkerHooks(nullptr, nullptr, nullptr);
  P->setPgoEnabled(true, 8, 4);
  ASSERT_TRUE(Driver.startSharedTaskPool());
  P->compileOrGet(0, nullptr, 0, nullptr);
  P->workerPollOnce();
  auto R = P->compileOrGet(0, nullptr, 0, nullptr);
  ASSERT_NE(R.fnPtr, nullptr);
  auto Fn = reinterpret_cast<int (*)(int, int (*)(int))>(R.fnPtr);
  EXPECT_EQ(Fn(10, &layout_host_callback), 62);
  uint32_t Generation = Shared->generation.loadAcquire();
  ASSERT_TRUE(Driver.refreshGlobalLayout(C.globalLayout));
  for (unsigned I = 0; I < 5; ++I)
    P->workerPollOnce();
  EXPECT_EQ(P->globalLayoutState(), EJitGlobalLayoutState::Refreshing);
  EXPECT_EQ(Shared->generation.loadAcquire(), Generation);
  EXPECT_EQ(Fn(10, &layout_host_callback),
            62); // retained body while lease lives
  P->releaseRead(R.bucketIndex);
  P->workerPollOnce();
  EXPECT_EQ(Shared->generation.loadAcquire(), Generation + 1);
  EXPECT_EQ(P->globalLayoutState(), EJitGlobalLayoutState::Collecting);
  P->compileOrGet(0, nullptr, 0, nullptr);
  P->workerPollOnce();
  auto fresh = P->compileOrGet(0, nullptr, 0, nullptr);
  ASSERT_NE(fresh.fnPtr, nullptr);
  EXPECT_NE(fresh.fnPtr, R.fnPtr);
  EXPECT_EQ(reinterpret_cast<int (*)(int, int (*)(int))>(fresh.fnPtr)(
                10, &layout_host_callback),
            62);
  P->releaseRead(fresh.bucketIndex);
  Driver.stopSharedTaskPool();
#endif
}

TEST(EJitGlobalLayoutIntegration,
     ColdConfiguredPgoUsesFourAdmissionsWithoutManualSeeding) {
  runProductionLayout(true, 7, false, 0, false, false, true);
}

TEST(EJitGlobalLayoutIntegration,
     ColdConfiguredAuditHonorsSamplingWithoutManualSeeding) {
  runProductionLayout(true, 7, false, 0, false, true, true);
}
