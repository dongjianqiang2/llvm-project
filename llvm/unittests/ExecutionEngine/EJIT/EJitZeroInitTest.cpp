//===-- EJitZeroInitTest.cpp - Worker-only init tests ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "gtest/gtest.h"

using namespace llvm;

TEST(EJitZeroInitAOT, FixupPlacementAndConstructorDefault) {
  LLVMContext Context;
  SMDiagnostic Error;
  auto M = parseAssemblyString(R"(
    target triple = "aarch64_be-none-elf"
    define i32 @probe(i32 %cell) !ejit.metadata !0 {
      ret i32 %cell
    }
    !0 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
  )", Error, Context);
  ASSERT_TRUE(M);
  ModuleAnalysisManager AM;
  EJitWrapperGenPass().run(*M, AM);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  auto *Func = M->getGlobalVariable("__ejit_funcidx_probe", true);
  auto *Dim = M->getGlobalVariable("__ejit_dimtype_cell", true);
  ASSERT_NE(Func, nullptr);
  ASSERT_NE(Dim, nullptr);
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
  EXPECT_FALSE(Func->getSection().empty());
  EXPECT_EQ(Func->getSection(), Dim->getSection());
  EXPECT_EQ(M->getGlobalVariable("llvm.global_ctors"), nullptr);
#else
  EXPECT_TRUE(Func->getSection().empty());
  EXPECT_TRUE(Dim->getSection().empty());
  EXPECT_NE(M->getGlobalVariable("llvm.global_ctors"), nullptr);
#endif
  unsigned FixupLoads = 0;
  for (Function &F : *M)
    for (Instruction &I : instructions(F))
      if (auto *Load = dyn_cast<LoadInst>(&I))
        if (Load->getPointerOperand() == Func ||
            Load->getPointerOperand() == Dim) {
          ++FixupLoads;
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
          EXPECT_EQ(Load->getOrdering(), AtomicOrdering::Acquire);
#else
          EXPECT_FALSE(Load->isAtomic());
#endif
        }
  EXPECT_EQ(FixupLoads, 2u);
}

#if defined(EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT) && !defined(EJIT_FREESTANDING)
#include "EJitBootstrapPool.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"

using namespace llvm::ejit;

static unsigned SealCalls;
static unsigned SplitCalls;
extern "C" void *SRE_MemDbgAlloc(unsigned, unsigned char, unsigned long,
                                 const char *, unsigned) {
  return nullptr; // This fixture uses an existing host executable address.
}
extern "C" unsigned split_2m_to_4k(unsigned long long, unsigned long long) {
  ++SplitCalls;
  return 0;
}
extern "C" unsigned enable_ex(unsigned, unsigned long long) {
  ++SealCalls;
  return 0;
}

// The hosted ELF linker synthesizes the same start/stop names for this
// fixture's undotted section that the SRE linker script defines in firmware.
static uint32_t CellSlot = UINT32_MAX;
[[gnu::retain, gnu::used, gnu::section("ejit_group")]]
static const ejit_reg_entry_t Registry[] = {
    {EJIT_REG_LIFECYCLE, "cell", nullptr, &CellSlot, 0}};

static int compiledProbe(int X) { return X + 7; }

TEST(EJitZeroInit, WorkerPublishesStaticFixupAndPgo) {
  constexpr uint32_t Worker = EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE;
  EJitCoreId::setCurrentForTest(Worker);
  auto Reset = make_scope_exit([] {
    ejit_shutdown();
    __atomic_store_n(&CellSlot, UINT32_MAX, __ATOMIC_RELEASE);
    EJitCoreId::resetForTest();
  });
  ASSERT_EQ(ejit_init_pgo(nullptr), EJIT_OK);
  const auto &State = gEJitSharedTaskPoolState;
  EXPECT_EQ(State.initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Ready));
  EXPECT_NE(__atomic_load_n(&CellSlot, __ATOMIC_ACQUIRE), UINT32_MAX);
  EXPECT_EQ(State.pgoEnabled.loadAcquire(), 1u);
  EXPECT_EQ(State.tier2Threshold.loadAcquire(), 64u);
}

TEST(EJitZeroInit, ProducerDispatchAndLifecycleWithoutInit) {
  constexpr uint32_t Worker = EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE;
  constexpr uint32_t Producer = Worker == 16 ? 0 : 16;
  auto &State = gEJitSharedTaskPoolState;
  SealCalls = SplitCalls = 0;
  const ejit_dim_pair_t Dim{0, 5};
  void *Fn = reinterpret_cast<void *>(1);
  uint32_t Bucket = UINT32_MAX;
  EJitCoreId::setCurrentForTest(Producer);
  auto Reset = make_scope_exit([] { EJitCoreId::resetForTest(); });
  EXPECT_EQ(ejit_init(nullptr), EJIT_ERR_DISABLED);
  EXPECT_EQ(ejit_activate("cell", 5), EJIT_ERR_NOT_ACTIVE);
  EXPECT_FALSE(ejit_is_active("cell", 5));
  EXPECT_EQ(ejit_taskpool_compile_or_get(0, &Dim, 1, &Fn, &Bucket),
            EJIT_ERR_NOT_ACTIVE);
  EXPECT_EQ(Fn, nullptr);
  EXPECT_EQ(Bucket, 0u);
  EXPECT_EQ(ejit_taskpool_pending_count(), 0u);
  State.initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Initializing));
  EXPECT_EQ(ejit_activate_all("cell"), EJIT_ERR_NOT_ACTIVE);
  State.magic = 0;
  State.initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Ready));
  EXPECT_EQ(ejit_activate("cell", 5), EJIT_ERR_NOT_ACTIVE); // bad ABI header
  State.initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Uninitialized));

  EJitSharedTaskPool Owner;
  Owner.bind(&State);
  Owner.setCompiler(
      [](void *, const EJitCompileRequest &, void **Out) {
        *Out = reinterpret_cast<void *>(&compiledProbe);
        return true;
      },
      nullptr);
  Owner.setCodeRangeProvider(
      [](void *, const void *Entry, EJitCompiledCodeInfo *Info) {
        auto Addr = reinterpret_cast<uintptr_t>(Entry);
        Info->fnPtr = const_cast<void *>(Entry);
        Info->codeStart = Addr & ~uintptr_t(4095);
        Info->codeSize = 4096;
        Info->poolBase = Addr & ~uintptr_t(2097151);
        Info->poolSize = 2097152;
        Info->poolId = 1;
        return true;
      },
      nullptr);
  EJitCoreId::setCurrentForTest(Worker);
  __atomic_store_n(&CellSlot, 0, __ATOMIC_RELEASE);
  uintptr_t Cells[EJIT_ICACHE_DIM_SIZE] = {};
  ASSERT_EQ(ejitIcacheRegisterSlot(0, Cells, 1, nullptr),
            EJitIcacheRegResult::Ok);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  auto Stop = make_scope_exit([&] {
    EJitCoreId::setCurrentForTest(Worker);
    Owner.ownerShutdown();
    ejitIcacheClearAll();
    __atomic_store_n(&CellSlot, UINT32_MAX, __ATOMIC_RELEASE);
  });

  EJitCoreId::setCurrentForTest(Producer);
  EXPECT_EQ(ejit_taskpool_get_worker_core(), Worker);
  EXPECT_EQ(ejit_activate("unknown", 5), EJIT_ERR_INVALID_PARAM);
  EXPECT_EQ(ejit_activate("cell", 256), EJIT_ERR_INVALID_PARAM);
  ASSERT_EQ(ejit_activate("cell", 5), EJIT_OK);
  EXPECT_TRUE(ejit_is_active("cell", 5));
  ASSERT_EQ(ejit_taskpool_compile_or_get(0, &Dim, 1, &Fn, &Bucket),
            EJIT_PENDING);
  EXPECT_EQ(Fn, nullptr);
  EXPECT_EQ(ejit_taskpool_pending_count(), 1u);
  EJitCoreId::setCurrentForTest(Worker);
  ASSERT_TRUE(Owner.pollOne());
  EJitCoreId::setCurrentForTest(Producer);
  ASSERT_EQ(ejit_taskpool_compile_or_get(0, &Dim, 1, &Fn, &Bucket), EJIT_OK);
  ASSERT_NE(Fn, nullptr);
  EXPECT_EQ(reinterpret_cast<int (*)(int)>(Fn)(3), 10);
  ejit_taskpool_release_read(Bucket);
  EXPECT_NE(SplitCalls, 0u);
  EXPECT_NE(SealCalls, 0u);
  EXPECT_EQ(Cells[5], reinterpret_cast<uintptr_t>(Fn));
  ejit_taskpool_stats_t Stats{};
  EXPECT_EQ(ejit_taskpool_get_stats(&Stats), EJIT_OK);
  EXPECT_EQ(Stats.readyEntries, 1u);
  EXPECT_EQ(ejit_deactivate("cell", 5), EJIT_OK);
  EXPECT_EQ(Cells[5], 0u);
  EXPECT_FALSE(ejit_is_active("cell", 5));
  EXPECT_EQ(ejit_taskpool_compile_or_get(0, &Dim, 1, &Fn, &Bucket),
            EJIT_ERR_INSTANCE_DISABLED);
}
#endif
