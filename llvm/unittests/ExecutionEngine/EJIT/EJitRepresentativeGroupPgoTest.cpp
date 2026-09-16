//===-- EJitRepresentativeGroupPgoTest.cpp - real representative PGO -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Integration test for the representative-PGO group lifecycle with REAL online
// PGO on the host:
//
//   * one candidate group of six legal cells elects ONE representative (the
//     first legal member, not a hardcoded cell 0);
//   * the representative's Tier-1 is compiled Instrumented by the real
//     EJitOptimizer pipeline, JIT-linked, and REALLY executed for exactly 64
//     real granted dispatches (the real EJitSharedTaskPool admits and accounts
//     every one of them);
//   * the real __profc_/__profd_ counters are read after execution, the schema
//     is validated (EJitProfileMerge::readProfileSchema) and the indexed
//     profile is synthesized from those live counters, not from a fixture;
//   * the frozen observation of the REAL queued Tier-2 request is joined with
//     the production applyT1DispatchObservation;
//   * the immutable EJitProfileBundle is published once by the group and the
//     SAME bundle is consumed by every member's PGOUse compile (entry_count
//     proves consumption);
//   * all six cells end up on ONE final physical Tier-2 object, established by
//     the representative's emission and reused only after the emitter's exact
//     full-identity compare; the shared code is really executed with raw
//     cell/TRP arguments and its loads/stores hit the correct cell rows;
//   * the real pool afterwards serves that same physical Tier-2 pointer.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitPreparedCode.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRepresentativeGroup.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectTransformLayer.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// Verify a module and report a real failure with the verifier output (ASSERT_*
/// is unavailable inside the JIT materialization lambda).
#define ADD_FAILURE_IF_VERIFY_FAILS(Mod)                                       \
  do {                                                                         \
    std::string VerifyErr;                                                     \
    raw_string_ostream OS(VerifyErr);                                          \
    if (verifyModule(Mod, &OS))                                                \
      ADD_FAILURE() << "module verify failed: " << OS.str();                   \
  } while (0)

struct CellRow {
  uint32_t gain;
  uint32_t live[2];
};

/// The entry exercises a dynamic branch on a runtime argument plus a dynamic
/// loop, with a may_const field load and period-array cell/TRP addressing, so a
/// real executed Tier-1 produces edge counters that differ per distribution.
std::string entryIR(StringRef Name) {
  std::string IR = R"(
  %Cfg = type { i32, [2 x i32] }
  @cfg = external global [6 x %Cfg], !ejit.metadata !0
  define i32 @)";
  IR += Name.str();
  IR += R"((i64 %cell, i64 %trp, i32 %x) !ejit.metadata !3 {
  entry:
    %gp = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 0
    %gain = load i32, ptr %gp, !ejit.may_const !7
    %cmp = icmp ugt i32 %x, %gain
    br i1 %cmp, label %hot, label %cold
  hot:
    %h = mul i32 %x, %gain
    br label %join
  cold:
    %c = add i32 %x, %gain
    br label %join
  join:
    %v = phi i32 [ %h, %hot ], [ %c, %cold ]
    br label %loop
  loop:
    %i = phi i32 [ 0, %join ], [ %i1, %loop ]
    %acc = phi i32 [ %v, %join ], [ %acc1, %loop ]
    %acc1 = add i32 %acc, %i
    %i1 = add i32 %i, 1
    %lc = icmp ult i32 %i1, %x
    br i1 %lc, label %loop, label %exit
  exit:
    %lp = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 1, i64 %trp
    %live = load i32, ptr %lp
    %sum = add i32 %acc1, %live
    store i32 %sum, ptr %lp
    %ci = trunc i64 %cell to i32
    %result = add i32 %sum, %ci
    ret i32 %result
  }
  !0 = !{!1, !2}
  !1 = !{!"ejit_period_arr", !"cell", i64 6}
  !2 = !{!"ejit_may_const_field", i64 0}
  !3 = !{!4, !5, !6}
  !4 = !{!"ejit_entry"}
  !5 = !{!"ejit_period_arr_ind", !"cell", i32 0}
  !6 = !{!"ejit_period_arr_ind", !"trp", i32 1}
  !7 = !{}
)";
  return IR;
}

EJitCodeIdentityScope codeScope(StringRef Entry, StringRef IRText) {
  EJitCodeIdentityScope S;
  S.source = SHA256::hash(arrayRefFromStringRef(IRText));
  S.entry = Entry.str();
  S.compilerPolicy = "test/representative-pgo/preserved-dims/final-t2";
  S.bindingGeneration = 1;
  return S;
}

struct ClockLog {
  uint32_t calls = 0;
  uint64_t next = 1000;
  uint64_t step = 10;
};
uint64_t groupClock(void *ctx) {
  auto *L = static_cast<ClockLog *>(ctx);
  ++L->calls;
  uint64_t V = L->next;
  L->next -= L->step;
  return V;
}

/// One real group run: real pool + real LLJIT + real optimizer + real emitter.
class RepGroupRun {
public:
  using Fn = uint32_t (*)(uint64_t, uint64_t, uint32_t);

  RepGroupRun(StringRef Name, uint32_t FnIndex, CellRow *RowsIn, uint32_t Quota,
              EJitRepresentativeGroupRegistry *SharedRegistry = nullptr)
      : EntryName(Name.str()), FuncIndex(FnIndex), Rows(RowsIn),
        IRText(entryIR(Name)), Quota(Quota) {
    EJitCoreId::resetForTest();
    State = std::make_unique<EJitSharedTaskPoolState>();
    J = cantFail(orc::LLJITBuilder().create());
    J->getObjTransformLayer().setTransform(
        [this](std::unique_ptr<MemoryBuffer> O)
            -> Expected<std::unique_ptr<MemoryBuffer>> {
          ++Objects;
          return std::move(O);
        });
    Registry = std::make_unique<PeriodArrayRegistry>();
    Registry->registerArray("cell", "cfg", Rows, 6);
    Opt = std::make_unique<EJitOptimizer>(*Registry, /*PreserveDimensions=*/true);
    Emitter = std::make_unique<EJitPreparedCodeEmitter>(*J, 1);
    if (SharedRegistry)
      Reg = SharedRegistry;
    else {
      OwnedReg = std::make_unique<EJitRepresentativeGroupRegistry>();
      Reg = OwnedReg.get();
    }

    EJitCoreId::setCurrentForTest(0);
    Pool.bind(State.get());
    Pool.setCompiler(&RepGroupRun::compileThunk, this);
    Pool.setMode(EJitCompileMode::Async);
    Pool.setPgoEnabled(true, Quota, /*maxConcurrentProfiles=*/4);
    Pool.setTraceClock(&groupClock, &Clock);
    EXPECT_TRUE(Pool.setRequestAttemptsEnabled(true));
    EXPECT_EQ(Pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  }

  ~RepGroupRun() { EJitCoreId::resetForTest(); }

  static bool compileThunk(void *Ctx, const EJitCompileRequest &Req,
                           void **OutFn) {
    return static_cast<RepGroupRun *>(Ctx)->compile(Req, OutFn);
  }

  /// Real compile callback: Instrumented Tier-1 (JIT-linked Instrumented
  /// pipeline, counters captured), PGOUse Tier-2 (the real queued request builds
  /// and publishes the bundle and links the one physical Tier-2 object).
  bool compile(const EJitCompileRequest &Req, void **OutFn) {
    Requests.push_back(Req);
    const uint32_t Tier = decodeReqTier(Req.funcIndex);
    if (stripReqTier(Req.funcIndex) != FuncIndex)
      return false;
    if (Tier == kEJitTierInstrumented) {
      T1Request = Req;
      T1Attempt = captureTier1ProfileAttemptIdentity(&Requests.back());
      auto &JD = cantFail(J->createJITDylib("t1_" + EntryName));
      orc::SymbolMap Syms;
      Syms[J->mangleAndIntern("cfg")] = orc::ExecutorSymbolDef(
          orc::ExecutorAddr::fromPtr(Rows), JITSymbolFlags::Exported);
      // The InstrProfiling lowering references the compiler-rt dummy
      // __llvm_profile_runtime; the JIT must define it (no profile runtime is
      // linked) or the Instrumented object never links.
      Syms[J->mangleAndIntern("__llvm_profile_runtime")] =
          orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(&ProfileRuntime),
                                 JITSymbolFlags::Exported);
      cantFail(JD.define(orc::absoluteSymbols(std::move(Syms))));

      CounterNames.clear();
      J->getIRTransformLayer().setTransform(
          [this](orc::ThreadSafeModule M, orc::MaterializationResponsibility &MR)
              -> Expected<orc::ThreadSafeModule> {
            ++T1Transforms;
            orc::SymbolFlagsMap Flags;
            M.withModuleDo([&](Module &Mod) {
              Opt->clearAnalyses();
              Opt->runPipeline(Mod, instrumentedContext());
              CounterNames.assign(Opt->getLastCounterNames().begin(),
                                  Opt->getLastCounterNames().end());
              for (const std::string &N : CounterNames) {
                Flags[J->mangleAndIntern("__profc_" + N)] =
                    JITSymbolFlags::Exported;
                Flags[J->mangleAndIntern("__profd_" + N)] =
                    JITSymbolFlags::Exported;
              }
              ADD_FAILURE_IF_VERIFY_FAILS(Mod);
              Opt->clearAnalyses();
            });
            if (!Flags.empty())
              if (Error E = MR.defineMaterializing(std::move(Flags)))
                return std::move(E);
            return std::move(M);
          });
      cantFail(J->addIRModule(JD, parseIR(IRText)));
      T1Fn = cantFail(J->lookup(JD, EntryName)).toPtr<void *>();
      Counters.clear();
      for (const std::string &N : CounterNames) {
        auto C = J->lookup(JD, "__profc_" + N);
        auto D = J->lookup(JD, "__profd_" + N);
        if (!C || !D)
          return false;
        Counters.push_back({N.c_str(), C->getValue(), D->getValue()});
      }
      *OutFn = T1Fn;
      return !Counters.empty() && T1Fn != nullptr;
    }
    if (Tier == kEJitTierPgoUse) {
      // The real queued Tier-2 request: freeze the bundle from the REAL
      // counters of the executed Tier-1, join the real frozen observation, and
      // publish it exactly once through the group.
      std::vector<PgoFunctionSchema> Schema;
      if (!readProfileSchema(Counters, {}, Schema))
        return false;
      EJitProfileBundle B;
      B.hasEdgeProfile = true;
      B.quality = ProfileSnapshotQuality::Complete;
      B.indexedProfile = synthesizeProfileBuffer(Counters);
      B.schema = Schema;
      if (B.indexedProfile.empty() || B.schema.empty())
        return false;
      applyT1DispatchObservation(B, &Requests.back(), T1Attempt);
      if (Reg->publishBundle(G, S, std::move(B)) != EJitPublishOutcome::Published)
        return false;
      Bundle = Reg->bundleFor(G);

      // The representative's own PGOUse compile, consuming the frozen bundle.
      auto M = parseIR(IRText);
      SpecializationContext Spec = useContext(S.logicalKey);
      uint64_t Count = 0;
      M.withModuleDo([&](Module &Mod) {
        Opt->clearAnalyses();
        Opt->runPipeline(Mod, Spec);
        if (Function *F = Mod.getFunction(EntryName))
          if (auto EC = F->getEntryCount())
            Count = EC->getCount();
        BranchProfiles = countProfMetadata(Mod);
        ADD_FAILURE_IF_VERIFY_FAILS(Mod);
        Opt->clearAnalyses();
      });
      RepresentativeEntryCount = Count;
      auto P = EJitPreparedCode::create(std::move(M),
                                        codeScope(EntryName, IRText), bindings());
      if (!P) {
        consumeError(P.takeError());
        return false;
      }
      auto LC = Emitter->link(std::move(*P));
      if (!LC) {
        consumeError(LC.takeError());
        return false;
      }
      T2CodeId = LC->codeId;
      T2Fn = LC->fn;
      if (!Reg->noteRepresentativeCode(G, S, T2CodeId, T2Fn, !LC->reused))
        return false;
      *OutFn = T2Fn;
      return true;
    }
    return false; // Baseline tier is not used by this test.
  }

  SpecializationContext instrumentedContext() const {
    SpecializationContext Spec;
    Spec.fnName = EntryName;
    Spec.cacheKey = uint64_t(FuncIndex) << 32;
    Spec.dimensions = {{"cell", 0}, {"trp", 1}};
    Spec.tier = CompileTier::Instrumented;
    return Spec;
  }

  SpecializationContext useContext(uint64_t Cell) const {
    SpecializationContext Spec;
    Spec.fnName = EntryName;
    Spec.cacheKey = (uint64_t(FuncIndex) << 32) | Cell;
    Spec.dimensions = {{"cell", static_cast<uint8_t>(Cell)}, {"trp", 1}};
    Spec.tier = CompileTier::PGOUse;
    // The immutable bundle is the carrier; profileData/scalarValueSites are the
    // compatibility views the PGOUse transform consumes (same as the driver).
    Spec.profileBundle = Bundle;
    if (Bundle) {
      Spec.profileData = Bundle->indexedProfile;
      Spec.scalarValueSites = Bundle->scalarSites;
    }
    return Spec;
  }

  orc::ThreadSafeModule parseIR(StringRef IR) const {
    auto C = std::make_unique<LLVMContext>();
    SMDiagnostic Diag;
    auto M = parseAssemblyString(IR, Diag, *C);
    EXPECT_NE(M, nullptr) << Diag.getMessage().str();
    if (!M)
      return {};
    M->setTargetTriple(J->getTargetTriple());
    M->setDataLayout(J->getDataLayout());
    return orc::ThreadSafeModule(std::move(M), std::move(C));
  }

  std::vector<EJitCodeBinding> bindings() const {
    return {{"cfg", reinterpret_cast<uintptr_t>(Rows), false}};
  }

  static uint32_t countProfMetadata(Module &M) {
    uint32_t N = 0;
    for (Function &F : M)
      for (BasicBlock &BB : F)
        for (Instruction &I : BB)
          if (I.hasMetadata("prof"))
            ++N;
    return N;
  }

  EJitSharedCacheSlot *findReadySlot(uint32_t FnIndex) {
    for (uint32_t B = 0; B < kEJitSharedCacheBuckets; ++B)
      for (uint32_t S = 0; S < kEJitSharedCacheSlots; ++S) {
        EJitSharedCacheSlot &Slot = State->buckets[B].slots[S];
        if (Slot.state.loadAcquire() ==
                static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
            Slot.funcIndex == FnIndex)
          return &Slot;
      }
    return nullptr;
  }

  // ---- identity ----
  std::string EntryName;
  uint32_t FuncIndex;
  CellRow *Rows;
  std::string IRText;
  uint32_t Quota;
  char ProfileRuntime = 0;

  // ---- real components ----
  std::unique_ptr<EJitSharedTaskPoolState> State;
  EJitSharedTaskPool Pool;
  ClockLog Clock;
  std::unique_ptr<orc::LLJIT> J;
  std::unique_ptr<PeriodArrayRegistry> Registry;
  std::unique_ptr<EJitOptimizer> Opt;
  std::unique_ptr<EJitPreparedCodeEmitter> Emitter;
  std::unique_ptr<EJitRepresentativeGroupRegistry> OwnedReg;
  EJitRepresentativeGroupRegistry *Reg = nullptr;

  // ---- group state ----
  EJitGroupHandle G;
  EJitRepresentativeSession S;
  std::vector<EJitWaiterToken> Waiters;
  std::vector<uint32_t> WaiterCells;

  // ---- captured observations ----
  std::vector<EJitCompileRequest> Requests;
  EJitCompileRequest T1Request{};
  Tier1ProfileAttemptIdentity T1Attempt;
  std::vector<std::string> CounterNames;
  SmallVector<PgoCounterRef, 4> Counters;
  EJitFrozenProfileBundle Bundle;
  void *T1Fn = nullptr;
  uint64_t T2CodeId = 0;
  void *T2Fn = nullptr;
  uint64_t RepresentativeEntryCount = 0;
  uint32_t BranchProfiles = 0;
  unsigned Objects = 0;
  unsigned T1Transforms = 0;
};

} // namespace

TEST(EJitRepresentativeGroupPgo,
     SixCellsShareOneRepresentativeTier1AndOnePhysicalTier2) {
  static once_flag Initialized;
  llvm::call_once(Initialized, [] {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
  });

  CellRow Rows[6] = {};
  for (CellRow &R : Rows)
    R.gain = 7;

  RepGroupRun Run("entryA", 200, Rows, /*Quota=*/64);

  // ---- admission gate + group + legality-based representative election ----
  EJitGroupAdmissionPolicy Policy;
  Policy.pgoEnabled = true;
  Policy.asyncService = true;
  Policy.normalOnlinePgo = true;
  Policy.dispatchQuota = 64;
  auto GH = Run.Reg->openGroup(0xC0FFEE, Policy);
  ASSERT_TRUE(static_cast<bool>(GH)) << toString(GH.takeError());
  Run.G = *GH;

  std::vector<EJitGroupMember> Members;
  for (uint32_t C = 0; C < 6; ++C)
    Members.push_back({/*logicalKey=*/C, /*funcIndex=*/200, /*cellActive=*/true,
                       /*cold=*/C == 0});
  auto SH = Run.Reg->electRepresentative(Run.G, Members, 0);
  ASSERT_TRUE(static_cast<bool>(SH)) << toString(SH.takeError());
  Run.S = *SH;
  // Cell 0 is cold: the representative is the first LEGAL member, not cell 0.
  EXPECT_EQ(Run.S.logicalKey, 1u);
  EXPECT_EQ(Run.S.dispatchLimit, 64u);
  EXPECT_EQ(Run.S.dispatchCount, 0u);
  EXPECT_NE(Run.S.attemptToken, 0u);
  EXPECT_NE(Run.S.samplingSessionId, 0u);

  // ---- non-representatives join as waiters; no personal sampling admission ----
  for (uint32_t C : {0u, 2u, 3u, 4u, 5u}) {
    auto W = Run.Reg->joinWaiter(Run.G, Members[C]);
    ASSERT_TRUE(static_cast<bool>(W)) << toString(W.takeError());
    Run.Waiters.push_back(*W);
    Run.WaiterCells.push_back(C);
  }
  EXPECT_EQ(Run.Reg->waiterCount(Run.G), 5u);
  // The publication barrier: before the bundle exists a member cannot consume
  // shared code (it must stay on AOT).
  {
    auto Probe = EJitPreparedCode::create(Run.parseIR(Run.IRText),
                                          codeScope("entryA", Run.IRText),
                                          Run.bindings());
    ASSERT_TRUE(static_cast<bool>(Probe)) << toString(Probe.takeError());
    auto D = Run.Reg->decideMember(Run.Waiters[0], (*Probe)->identity(),
                                   *Run.Emitter);
    EXPECT_EQ(D.kind, EJitMemberShare::NotReady);
  }

  // ---- 64 REAL granted Tier-1 dispatches through the real pool ----
  uint64_t Token = 0;
  ASSERT_EQ(Run.Pool
                .compileOrGet(200, nullptr, 0, reinterpret_cast<void *>(0x1234),
                              nullptr, 0, &Token)
                .status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_NE(Token, 0u);
  ASSERT_TRUE(Run.Pool.pollOne()); // real Instrumented compile + publish
  ASSERT_NE(Run.T1Fn, nullptr);
  ASSERT_EQ(Run.CounterNames.size(), 1u);
  EJitSharedCacheSlot *Slot = Run.findReadySlot(200);
  ASSERT_NE(Slot, nullptr);
  EXPECT_EQ(Slot->t1DispatchLimit.loadRelaxed(), 64u);

  uint32_t Executed = 0;
  for (unsigned I = 0; I < 64; ++I) {
    auto Hit = Run.Pool.tryCacheHit0D(200);
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit) << "dispatch " << I;
    ASSERT_EQ(Hit.fnPtr, Run.T1Fn) << "dispatch " << I;
    // REALLY execute the granted Instrumented pointer: 16 cold / 48 hot arms.
    uint32_t X = (I % 4 == 0) ? 3u : 10u;
    (void)reinterpret_cast<RepGroupRun::Fn>(Hit.fnPtr)(/*cell=*/1, I % 2, X);
    ++Executed;
    if (Hit.hasReadToken)
      Run.Pool.releaseRead(Hit.bucketIndex);
    // The pool froze the boundary in the same commit; the group quota uses the
    // pool's exact frozen value, never a compile-time timestamp.
    uint64_t Now = (I + 1 == 64) ? Slot->t1QuotaEnd.loadRelaxed() : 0;
    EJitDispatchOutcome O = Run.Reg->recordRepresentativeDispatch(Run.G, Run.S, Now);
    if (I + 1 == 64)
      EXPECT_EQ(O, EJitDispatchOutcome::CountedAndClosed);
    else
      EXPECT_EQ(O, EJitDispatchOutcome::Counted);
  }
  EXPECT_EQ(Executed, 64u);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 64u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 1000u);
  EXPECT_EQ(Run.Reg->diagnostics().representativeDispatches, 64u);

  // ---- the real queued Tier-2 request builds/publishes the bundle and links
  //      the ONE physical Tier-2 object ----
  ASSERT_EQ(Run.Pool.pendingCount(), 1u);
  ASSERT_TRUE(Run.Pool.pollOne());
  const EJitCompileRequest *T2 = nullptr;
  const EJitCompileRequest *T1 = nullptr;
  for (const EJitCompileRequest &R : Run.Requests) {
    if (decodeReqTier(R.funcIndex) == kEJitTierPgoUse)
      T2 = &R;
    if (decodeReqTier(R.funcIndex) == kEJitTierInstrumented)
      T1 = &R;
  }
  ASSERT_NE(T1, nullptr);
  ASSERT_NE(T2, nullptr);
  EXPECT_EQ(T2->attemptToken, T1->attemptToken);
  EXPECT_EQ(T2->generation, T1->generation);
  EXPECT_EQ(T2->t1DispatchCount, 64u);
  EXPECT_EQ(T2->t1DispatchLimit, 64u);
  EXPECT_EQ(T2->t1QuotaEnd, 1000u);

  // Real execution evidence: the entry counter counted 64 real entries and the
  // branch counters split 48 hot / 16 cold, read from the LIVE counters.
  const uint64_t *Counts =
      reinterpret_cast<const uint64_t *>(Run.Counters[0].profcAddr);
  const uint32_t NumCounters =
      *reinterpret_cast<const uint32_t *>(Run.Counters[0].profdAddr + 48);
  ASSERT_GE(NumCounters, 3u);
  EXPECT_EQ(Counts[1], 64u); // 64 REAL entries of the Instrumented code
  EXPECT_EQ(Counts[2], 48u); // 48 taken / 16 untaken (16 cold arms)
  EXPECT_EQ(Counts[1], 64u);

  // ---- the immutable bundle: one publication, full identity and honest counts
  ASSERT_TRUE(Run.Bundle != nullptr);
  EXPECT_EQ(Run.Bundle->groupId, Run.G.groupId);
  EXPECT_EQ(Run.Bundle->groupGeneration, Run.G.generation);
  EXPECT_EQ(Run.Bundle->samplingSessionId, Run.S.samplingSessionId);
  EXPECT_EQ(Run.Bundle->representativeAttemptToken, Run.S.attemptToken);
  EXPECT_EQ(Run.Bundle->representativeLogicalKey, 1u);
  EXPECT_EQ(Run.Bundle->actualDispatchCount, 64u);
  EXPECT_EQ(Run.Bundle->dispatchLimit, 64u);
  EXPECT_EQ(Run.Bundle->quotaEnd, 1000u);
  EXPECT_EQ(Run.Bundle->dispatchQuality,
            T1DispatchObservationQuality::FrozenAtQuota);
  EXPECT_FALSE(Run.Bundle->indexedProfile.empty());
  ASSERT_EQ(Run.Bundle->schema.size(), 1u);
  EXPECT_EQ(Run.Bundle->schema[0].pgoName, "entryA");
  EXPECT_EQ(Run.RepresentativeEntryCount, 64u); // PGOUse consumed the profile
  EXPECT_GT(Run.BranchProfiles, 0u);            // real !prof weights

  // ---- five non-representative members: same bundle, one physical Tier-2 ----
  for (size_t I = 0; I < Run.Waiters.size(); ++I) {
    const uint32_t Cell = Run.WaiterCells[I];
    // The Gen/Use prefix + site mapping of this member must be compatible with
    // the frozen bundle before any code reuse.
    std::string Reason;
    ASSERT_TRUE(Run.Reg->schemaCompatible(Run.G, Run.Bundle->schema, &Reason))
        << Reason;

    auto M = Run.parseIR(Run.IRText);
    uint64_t EntryCount = 0;
    uint32_t Profs = 0;
    M.withModuleDo([&](Module &Mod) {
      Run.Opt->clearAnalyses();
      Run.Opt->runPipeline(Mod, Run.useContext(Cell));
      if (Function *F = Mod.getFunction("entryA"))
        if (auto EC = F->getEntryCount())
          EntryCount = EC->getCount();
      Profs = RepGroupRun::countProfMetadata(Mod);
      Run.Opt->clearAnalyses();
    });
    // A different member really consumed the SAME verified bundle.
    EXPECT_EQ(EntryCount, 64u) << "cell " << Cell;
    EXPECT_GT(Profs, 0u) << "cell " << Cell;

    auto P = EJitPreparedCode::create(std::move(M),
                                      codeScope("entryA", Run.IRText),
                                      Run.bindings());
    ASSERT_TRUE(static_cast<bool>(P)) << toString(P.takeError());
    auto D = Run.Reg->decideMember(Run.Waiters[I], (*P)->identity(),
                                   *Run.Emitter);
    EXPECT_EQ(D.kind, EJitMemberShare::Reuse) << D.reason;
    EXPECT_EQ(D.codeId, Run.T2CodeId);
    EXPECT_EQ(D.fn, Run.T2Fn);

    auto LC = Run.Emitter->link(std::move(*P));
    ASSERT_TRUE(static_cast<bool>(LC)) << toString(LC.takeError());
    EXPECT_TRUE(LC->reused);
    EXPECT_EQ(LC->codeId, Run.T2CodeId);
    EXPECT_EQ(LC->fn, Run.T2Fn);
    EXPECT_TRUE(Run.Reg->completeMember(Run.Waiters[I], LC->codeId, LC->fn,
                                        /*EmittedNew=*/LC->reused ? false : true));

    // REALLY execute the shared physical Tier-2 with raw cell/TRP arguments.
    uint32_t Before = Rows[Cell].live[1];
    uint32_t Ret = reinterpret_cast<RepGroupRun::Fn>(LC->fn)(Cell, 1, 5);
    // x=5, gain=7 -> cold arm (5+7=12) + loop 0+1+2+3+4 = 22.
    EXPECT_EQ(Rows[Cell].live[1], Before + 22) << "cell " << Cell;
    EXPECT_EQ(Ret, Rows[Cell].live[1] + Cell);
  }

  // The representative's own cell runs the very same physical Tier-2 object.
  {
    uint32_t Before = Rows[1].live[1];
    uint32_t Ret = reinterpret_cast<RepGroupRun::Fn>(Run.T2Fn)(1, 1, 5);
    EXPECT_EQ(Rows[1].live[1], Before + 22);
    EXPECT_EQ(Ret, Rows[1].live[1] + 1);
  }

  // ---- exactly once, honest counters, one physical object ----
  EXPECT_FALSE(Run.Reg->completeMember(Run.Waiters[0], Run.T2CodeId, Run.T2Fn,
                                       /*EmittedNew=*/false));
  EJitGroupDiagnostics Diag = Run.Reg->diagnostics();
  EXPECT_EQ(Diag.admittedGroups, 1u);
  EXPECT_EQ(Diag.rejectedAdmissions, 0u);
  EXPECT_EQ(Diag.representativeSessions, 1u);
  EXPECT_EQ(Diag.representativeDispatches, 64u);
  EXPECT_EQ(Diag.waitersJoined, 5u);
  EXPECT_EQ(Diag.waitersCompleted, 5u);
  EXPECT_EQ(Diag.bundlePublications, 1u);
  EXPECT_EQ(Diag.completeProfiles, 1u);
  EXPECT_EQ(Diag.physicalCodeObjects, 1u);
  EXPECT_EQ(Diag.sharedPhysicalReuses, 5u);
  EXPECT_EQ(Diag.independentPhysicalObjects, 0u);
  EXPECT_EQ(Run.Emitter->stats().codeObjects, 1u);
  EXPECT_EQ(Run.Emitter->stats().reused, 5u);
  EXPECT_EQ(Run.T1Transforms, 1u);
  EXPECT_EQ(Run.Objects, 2u); // 1 real Instrumented T1 object + 1 real shared T2

  // The real pool now serves that exact physical shared Tier-2 pointer.
  auto T2Hit = Run.Pool.tryCacheHit0D(200);
  ASSERT_EQ(T2Hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(T2Hit.fnPtr, Run.T2Fn);
  EXPECT_NE(T2Hit.fnPtr, Run.T1Fn);
  if (T2Hit.hasReadToken)
    Run.Pool.releaseRead(T2Hit.bucketIndex);
}

TEST(EJitRepresentativeGroupPgo, TwoGroupsNeverMixSamplesQuotaOrSessions) {
  static once_flag Initialized;
  llvm::call_once(Initialized, [] {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
  });

  // Both groups live in ONE registry/session domain, so "never mix" is a real
  // property of the shared session allocator and not of separate test objects.
  EJitRepresentativeGroupRegistry SharedReg;

  // Group A: hot-heavy distribution (48 hot / 16 cold).
  CellRow RowsA[6] = {};
  for (CellRow &R : RowsA)
    R.gain = 7;
  RepGroupRun A("entryA", 200, RowsA, 64, &SharedReg);

  // Group B: cold-heavy distribution (16 hot / 48 cold) on its own entry.
  CellRow RowsB[6] = {};
  for (CellRow &R : RowsB)
    R.gain = 7;
  RepGroupRun B("entryB", 201, RowsB, 64, &SharedReg);

  auto runGroup = [](RepGroupRun &Run, uint64_t GroupId, bool HotHeavy) {
    EJitGroupAdmissionPolicy Policy;
    Policy.pgoEnabled = true;
    Policy.asyncService = true;
    Policy.normalOnlinePgo = true;
    Policy.dispatchQuota = 64;
    auto GH = Run.Reg->openGroup(GroupId, Policy);
    EXPECT_TRUE(static_cast<bool>(GH));
    Run.G = *GH;
    std::vector<EJitGroupMember> Members;
    for (uint32_t C = 0; C < 6; ++C)
      Members.push_back({C, Run.FuncIndex, true, false});
    auto SH = Run.Reg->electRepresentative(Run.G, Members, 0);
    EXPECT_TRUE(static_cast<bool>(SH));
    Run.S = *SH;
    EXPECT_EQ(Run.S.logicalKey, 0u); // cell 0 is legal here
    for (uint32_t C = 1; C < 6; ++C) {
      auto W = Run.Reg->joinWaiter(Run.G, Members[C]);
      EXPECT_TRUE(static_cast<bool>(W));
      Run.Waiters.push_back(*W);
      Run.WaiterCells.push_back(C);
    }
    uint64_t Token = 0;
    EXPECT_EQ(Run.Pool
                  .compileOrGet(Run.FuncIndex, nullptr, 0,
                                reinterpret_cast<void *>(0x1234), nullptr, 0,
                                &Token)
                  .status,
              EJitCompileOrGetStatus::EnqueuedPending);
    EXPECT_TRUE(Run.Pool.pollOne());
    EJitSharedCacheSlot *Slot = Run.findReadySlot(Run.FuncIndex);
    EXPECT_NE(Slot, nullptr);
    for (unsigned I = 0; I < 64; ++I) {
      auto Hit = Run.Pool.tryCacheHit0D(Run.FuncIndex);
      EXPECT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
      // HotHeavy: 12 of 16 iterations take the hot arm (48/16); otherwise the
      // inverse (16/48). x > gain = hot.
      bool Hot = HotHeavy ? (I % 4 != 0) : (I % 4 == 0);
      uint32_t X = Hot ? 10u : 3u;
      (void)reinterpret_cast<RepGroupRun::Fn>(Hit.fnPtr)(0, I % 2, X);
      if (Hit.hasReadToken)
        Run.Pool.releaseRead(Hit.bucketIndex);
      uint64_t Now = (I + 1 == 64 && Slot) ? Slot->t1QuotaEnd.loadRelaxed() : 0;
      Run.Reg->recordRepresentativeDispatch(Run.G, Run.S, Now);
    }
    EXPECT_EQ(Run.Pool.pendingCount(), 1u);
    EXPECT_TRUE(Run.Pool.pollOne());
    return Run.Bundle;
  };

  EJitFrozenProfileBundle BundleA = runGroup(A, 0xBEEF, /*HotHeavy=*/true);
  EJitFrozenProfileBundle BundleB = runGroup(B, 0xF00D, /*HotHeavy=*/false);
  ASSERT_TRUE(BundleA != nullptr);
  ASSERT_TRUE(BundleB != nullptr);

  // Sessions, attempts and groups never mix.
  EXPECT_NE(BundleA->groupId, BundleB->groupId);
  EXPECT_NE(BundleA->samplingSessionId, BundleB->samplingSessionId);
  EXPECT_NE(BundleA->representativeAttemptToken,
            BundleB->representativeAttemptToken);
  EXPECT_NE(BundleA->indexedProfile, BundleB->indexedProfile);

  // Each profile carries its own distribution: 48 vs 16 taken branches.
  const uint64_t *CountsA =
      reinterpret_cast<const uint64_t *>(A.Counters[0].profcAddr);
  const uint64_t *CountsB =
      reinterpret_cast<const uint64_t *>(B.Counters[0].profcAddr);
  EXPECT_EQ(CountsA[1], 64u);
  EXPECT_EQ(CountsB[1], 64u);
  EXPECT_EQ(CountsA[2], 48u);
  EXPECT_EQ(CountsB[2], 16u);
  EXPECT_EQ(CountsA[2] + CountsB[2], 64u);
  // Per-group quota is exact and independent: the shared registry accounted 64
  // dispatches for each of the two groups, and each bundle reports its own 64.
  EJitGroupDiagnostics SharedDiag = A.Reg->diagnostics();
  EXPECT_EQ(SharedDiag.admittedGroups, 2u);
  EXPECT_EQ(SharedDiag.representativeSessions, 2u);
  EXPECT_EQ(SharedDiag.representativeDispatches, 128u);
  EXPECT_EQ(BundleA->actualDispatchCount, 64u);
  EXPECT_EQ(BundleB->actualDispatchCount, 64u);
  EXPECT_EQ(BundleA->dispatchLimit, 64u);
  EXPECT_EQ(BundleB->dispatchLimit, 64u);

  // A late dispatch of a retired session cannot pollute the replacement.
  auto NewA = A.Reg->invalidateRepresentative(A.G, A.S, "cold");
  ASSERT_TRUE(static_cast<bool>(NewA)) << toString(NewA.takeError());
  EXPECT_EQ(A.Reg->recordRepresentativeDispatch(A.G, A.S, 9999),
            EJitDispatchOutcome::Stale);
  EJitProfileBundle Late;
  Late.hasEdgeProfile = true;
  Late.indexedProfile = "late";
  Late.schema = BundleA->schema;
  Late.dispatchQuality = T1DispatchObservationQuality::FrozenAtQuota;
  EXPECT_EQ(A.Reg->publishBundle(A.G, A.S, std::move(Late)),
            EJitPublishOutcome::Stale);
  // The retired generation's bundle is gone; the replacement starts empty and
  // the retired generation's waiters can no longer settle against it.
  EXPECT_TRUE(A.Reg->bundleFor(*NewA) == nullptr);
  auto StaleWaiter = A.Reg->joinWaiter(A.G, EJitGroupMember{1, 200, true, false});
  ASSERT_FALSE(static_cast<bool>(StaleWaiter));
  consumeError(StaleWaiter.takeError());
  // Group B is untouched by group A's retirement.
  EXPECT_EQ(B.Reg->bundleFor(B.G), BundleB);
  EXPECT_EQ(BundleB->actualDispatchCount, 64u);
}

TEST(EJitRepresentativeGroupPgo, AdmissionGatesAndLifecycleSettleExactlyOnce) {
  EJitRepresentativeGroupRegistry Reg;
  EJitGroupAdmissionPolicy P;
  P.pgoEnabled = true;
  P.asyncService = true;
  P.normalOnlinePgo = true;
  P.dispatchQuota = 64;

  // V1 sharing is Async + normal online PGO only: every rejection is decided
  // before any group/queue side effect.
  {
    auto Off = P;
    Off.pgoEnabled = false;
    EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Off),
              EJitGroupAdmitReject::PgoDisabled);
    auto R = Reg.openGroup(7, Off);
    ASSERT_FALSE(static_cast<bool>(R));
    consumeError(R.takeError());
  }
  {
    auto Sync = P;
    Sync.asyncService = false;
    EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Sync),
              EJitGroupAdmitReject::NotAsync);
    auto R = Reg.openGroup(7, Sync);
    ASSERT_FALSE(static_cast<bool>(R));
    consumeError(R.takeError());
  }
  {
    auto Audit = P;
    Audit.normalOnlinePgo = false;
    EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Audit),
              EJitGroupAdmitReject::AuditOnly);
    auto R = Reg.openGroup(7, Audit);
    ASSERT_FALSE(static_cast<bool>(R));
    consumeError(R.takeError());
  }
  {
    auto Changing = P;
    Changing.modeChangeInFlight = true;
    EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Changing),
              EJitGroupAdmitReject::ModeChangeInFlight);
    auto R = Reg.openGroup(7, Changing);
    ASSERT_FALSE(static_cast<bool>(R));
    consumeError(R.takeError());
  }
  {
    auto Zero = P;
    Zero.dispatchQuota = 0;
    EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Zero),
              EJitGroupAdmitReject::ZeroQuota);
    auto R = Reg.openGroup(7, Zero);
    ASSERT_FALSE(static_cast<bool>(R));
    consumeError(R.takeError());
  }
  // No rejected policy created a group or an admission record.
  EXPECT_EQ(Reg.groupCount(), 0u);
  EXPECT_EQ(Reg.diagnostics().admittedGroups, 0u);
  EXPECT_EQ(Reg.diagnostics().rejectedAdmissions, 5u);

  // Legality-based election: cell 0 cold, cell 1 inactive, cell 2 legal.
  auto GH = Reg.openGroup(7, P);
  ASSERT_TRUE(static_cast<bool>(GH)) << toString(GH.takeError());
  EJitGroupHandle G = *GH;
  std::vector<EJitGroupMember> Members = {
      {0, 11, true, true}, {1, 11, false, false}, {2, 11, true, false},
      {3, 11, true, false}, {4, 11, true, false}, {5, 11, true, false}};
  auto SH = Reg.electRepresentative(G, Members, 0);
  ASSERT_TRUE(static_cast<bool>(SH)) << toString(SH.takeError());
  EJitRepresentativeSession S = *SH;
  EXPECT_EQ(S.logicalKey, 2u);
  // One representative per generation: a second election returns the same one.
  auto Again = Reg.electRepresentative(G, Members, 0);
  ASSERT_TRUE(static_cast<bool>(Again));
  EXPECT_EQ(Again->attemptToken, S.attemptToken);

  auto W0 = Reg.joinWaiter(G, Members[0]);
  ASSERT_TRUE(static_cast<bool>(W0)) << toString(W0.takeError());

  // A session that never really dispatched cannot publish a bundle.
  EJitProfileBundle Bad;
  Bad.hasEdgeProfile = true;
  Bad.indexedProfile = "x";
  Bad.schema = {PgoFunctionSchema{"f", 1, 2, 1, 0, 0, 0}};
  Bad.dispatchQuality = T1DispatchObservationQuality::FrozenAtQuota;
  EXPECT_EQ(Reg.publishBundle(G, S, std::move(Bad)),
            EJitPublishOutcome::InvalidBundle);

  // Two real dispatches, then a valid publication.
  EXPECT_EQ(Reg.recordRepresentativeDispatch(G, S, 0),
            EJitDispatchOutcome::Counted);
  EXPECT_EQ(Reg.recordRepresentativeDispatch(G, S, 0),
            EJitDispatchOutcome::Counted);
  EJitProfileBundle Good;
  Good.hasEdgeProfile = true;
  Good.indexedProfile = "profile";
  Good.schema = {PgoFunctionSchema{"f", 1, 2, 1, 0, 0, 0}};
  Good.dispatchQuality = T1DispatchObservationQuality::FrozenAtQuota;
  EXPECT_EQ(Reg.publishBundle(G, S, std::move(Good)), EJitPublishOutcome::Published);
  ASSERT_TRUE(Reg.bundleFor(G) != nullptr);
  EXPECT_EQ(Reg.bundleFor(G)->actualDispatchCount, 2u);
  EXPECT_EQ(Reg.bundleFor(G)->samplingSessionId, S.samplingSessionId);
  // A duplicate late publication never replaces the frozen bundle.
  EJitProfileBundle Duplicate;
  Duplicate.hasEdgeProfile = true;
  Duplicate.indexedProfile = "other";
  Duplicate.schema = {PgoFunctionSchema{"f", 1, 2, 1, 0, 0, 0}};
  Duplicate.dispatchQuality = T1DispatchObservationQuality::FrozenAtQuota;
  EXPECT_EQ(Reg.publishBundle(G, S, std::move(Duplicate)),
            EJitPublishOutcome::AlreadyPublished);
  EXPECT_EQ(Reg.bundleFor(G)->indexedProfile, "profile");

  // Schema compatibility is exact: a differing site count is rejected instead
  // of dropping value/scalar data.
  std::string Reason;
  EXPECT_TRUE(Reg.schemaCompatible(G, {PgoFunctionSchema{"f", 1, 2, 1, 0, 0, 0}},
                                   &Reason));
  EXPECT_FALSE(Reg.schemaCompatible(
      G, {PgoFunctionSchema{"f", 1, 2, 1, 0, 0, 1}}, &Reason));
  EXPECT_FALSE(Reason.empty());
  EXPECT_FALSE(Reg.schemaCompatible(G, {}, &Reason));

  // Waiter cancellation settles its exact token only.
  EXPECT_TRUE(Reg.cancelWaiter(*W0));
  EXPECT_FALSE(Reg.cancelWaiter(*W0)); // already settled
  EXPECT_EQ(Reg.diagnostics().waitersCancelled, 1u);

  // Representative invalidation opens a new generation; the retired session's
  // promises can only settle as Stale.
  auto Next = Reg.invalidateRepresentative(G, S, "cold representative");
  ASSERT_TRUE(static_cast<bool>(Next)) << toString(Next.takeError());
  EXPECT_EQ(Next->generation, G.generation + 1);
  EXPECT_EQ(Reg.recordRepresentativeDispatch(G, S, 1),
            EJitDispatchOutcome::Stale);
  EXPECT_EQ(Reg.cancelRepresentative(G, S), false);
  EXPECT_EQ(Reg.diagnostics().representativeReElections, 1u);
  // A stale handle cannot elect or join in the new generation.
  auto StaleWaiter = Reg.joinWaiter(G, Members[3]);
  ASSERT_FALSE(static_cast<bool>(StaleWaiter));
  consumeError(StaleWaiter.takeError());
  // The new generation starts from zero: the retired count never carries over.
  auto NewSession = Reg.electRepresentative(*Next, Members, 0);
  ASSERT_TRUE(static_cast<bool>(NewSession)) << toString(NewSession.takeError());
  EXPECT_EQ(NewSession->dispatchCount, 0u);
  EXPECT_NE(NewSession->samplingSessionId, S.samplingSessionId);
  EXPECT_NE(NewSession->attemptToken, S.attemptToken);
  EXPECT_EQ(Reg.bundleFor(*Next), nullptr);
}