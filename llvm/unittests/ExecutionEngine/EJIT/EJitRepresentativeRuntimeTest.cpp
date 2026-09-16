//===-- EJitRepresentativeRuntimeTest.cpp - production runtime path --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Representative-PGO group sharing through the REAL runtime path:
//
//   * the runtime is initialized through the production C entry point
//     `ejit_init_representative` (the ordinary product default stays OFF);
//   * every Tier-1/Tier-2 request enters through the real business entry
//     `ejit_taskpool_compile_or_get` (the same call the AOT wrapper makes);
//   * the compile callback is the real `EJitCompileDriver::compileNow`, so the
//     real ORC/JITLink/optimizer path compiles Tier-1 and Tier-2;
//   * the group lifecycle (candidate grouping, representative election, waiter
//     join, quota, exactly-once publication) is driven by the production pool
//     hooks - `setSamplingAdmissionCallback` and `setDispatchObserver` - and
//     never by a test-side call to recordRepresentativeDispatch;
//   * the 64 granted Tier-1 dispatches are consumed through the real entry, and
//     the ONE physical Tier-2 pointer is resolved from the real shared cache.
//
// Generated code executes with its production read token held.
// Worker progress belongs exclusively to the production worker.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/ExecutionEngine/EJIT/EJitLibcallStubs.h"
#include "llvm/ExecutionEngine/EJIT/EJitRepresentativeGroup.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

// The host component archives this worker links against do not ship
// LLVMX86AsmParser (the EJIT JIT never parses assembly: it compiles bitcode, so
// the only reference is EJitOrcEngine's not-freestanding fallback
// InitializeAllAsmParsers()). A weak fallback keeps the production TU unchanged
// and is only reachable if that fallback ever runs.
extern "C" LLVM_ATTRIBUTE_WEAK void LLVMInitializeX86AsmParser() {}

namespace llvm {
namespace ejit {

// Host stand-in for the freestanding libcall table (EJitLibcallStubs.cpp). That
// TU also defines __llvm_profile_instrument_target, which this build must take
// from EJitVpCollector.cpp (the real value-profile collector); MSVC/COFF has no
// weak definition, so the two cannot be linked together on this host. The table
// supplies the host memory libcalls as explicit effective bindings, including
// calls introduced by backend intrinsic lowering in the shared-code route.
ArrayRef<LibcallSymbol> getLibcallSymbols() {
  static const LibcallSymbol Symbols[] = {
    {"memcpy", reinterpret_cast<void *>(&std::memcpy)},
    {"memset", reinterpret_cast<void *>(&std::memset)},
    {"memmove", reinterpret_cast<void *>(&std::memmove)}
  };
  return Symbols;
}

} // namespace ejit
} // namespace llvm

extern "C" void ejit_register_period_array(const char *, const char *, void *, uint64_t);

namespace {

/// Six legal cells of ONE entry: the candidate identity (funcIdx + dim TYPES)
/// is shared, the per-cell instance id differs. `trp` is the second dimension.
constexpr uint32_t kCells = 14; // shared, cancelled, cold and budget-exhausted groups
constexpr uint32_t kQuota = kEJitRepresentativeDispatchQuota;
constexpr uint32_t kPressureEntries = 20;
constexpr uint32_t kPressureCells = 6;

std::atomic<bool> PauseLast{false}, LastEntered{false}, ResumeLast{false};
extern "C" void rep_runtime_pause() {
  if (!PauseLast.load(std::memory_order_acquire)) return;
  LastEntered.store(true, std::memory_order_release);
  while (!ResumeLast.load(std::memory_order_acquire)) std::this_thread::yield();
}

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
extern "C" uint32_t rep_runtime_target(uint32_t X) { return X * 2 + 5; }
using ValueTarget = uint32_t (*)(uint32_t);
ValueTarget RuntimeTarget = &rep_runtime_target;
#endif

struct CellRow {
  uint32_t gain;
  uint32_t live[2];
};

/// Build the entry module of the runtime test: a real specialization-eligible
/// entry with a may_const field load, a dynamic branch and a dynamic loop, so a
/// genuine Instrumented Tier-1 has edge counters to profile.
std::unique_ptr<Module> buildEntryModule(LLVMContext &Ctx, StringRef Name,
                                         StringRef ConfigName = "cfg") {
  auto M = std::make_unique<Module>("ejit-representative-runtime", Ctx);
  M->setTargetTriple(Triple(sys::getDefaultTargetTriple()));

  new GlobalVariable(*M, Type::getInt32Ty(Ctx), false,
                     GlobalValue::ExternalLinkage, nullptr, "__llvm_profile_runtime");

  StructType *CfgTy =
      StructType::create(Ctx, {Type::getInt32Ty(Ctx),
                               ArrayType::get(Type::getInt32Ty(Ctx), 2)},
                         "Cfg");
  ArrayType *ArrTy = ArrayType::get(CfgTy, kCells);
  auto *Cfg = new GlobalVariable(
      *M, ArrTy, /*isConstant=*/false, GlobalValue::ExternalLinkage, nullptr,
      ConfigName);
  Cfg->setMetadata(
      "ejit.metadata",
      MDNode::get(Ctx,
                  {MDNode::get(Ctx, {MDString::get(Ctx, "ejit_period_arr"),
                                     MDString::get(Ctx, "cell"),
                                     ConstantAsMetadata::get(
                                         ConstantInt::get(Type::getInt64Ty(Ctx),
                                                          kCells))}),
                   MDNode::get(Ctx, {MDString::get(Ctx, "ejit_may_const_field"),
                                     ConstantAsMetadata::get(
                                         ConstantInt::get(Type::getInt64Ty(Ctx),
                                                          0))})}));

  FunctionType *FnTy = FunctionType::get(
      Type::getInt32Ty(Ctx),
      {Type::getInt64Ty(Ctx), Type::getInt64Ty(Ctx), Type::getInt32Ty(Ctx)},
      /*isVarArg=*/false);
  Function *F =
      Function::Create(FnTy, GlobalValue::ExternalLinkage, Name.str(), *M);
  F->setMetadata("ejit.metadata",
                 MDNode::get(Ctx,
                             {MDNode::get(Ctx,
                                          {MDString::get(Ctx, "ejit_entry")}),
                              MDNode::get(Ctx, {MDString::get(Ctx,
                                                             "ejit_period_arr_"
                                                             "ind"),
                                                MDString::get(Ctx, "cell"),
                                                ConstantAsMetadata::get(
                                                    ConstantInt::get(
                                                        Type::getInt32Ty(Ctx),
                                                        0))}),
                              MDNode::get(Ctx, {MDString::get(Ctx,
                                                             "ejit_period_arr_"
                                                             "ind"),
                                                MDString::get(Ctx, "trp"),
                                                ConstantAsMetadata::get(
                                                    ConstantInt::get(
                                                        Type::getInt32Ty(Ctx),
                                                        1))})}));

  IRBuilder<> B(Ctx);
  B.SetInsertPoint(BasicBlock::Create(Ctx, "entry", F));
  B.CreateCall(M->getOrInsertFunction("rep_runtime_pause",
                                     FunctionType::get(B.getVoidTy(), false)));
  Value *Cell = F->getArg(0);
  Value *Trp = F->getArg(1);
  Value *X = F->getArg(2);
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  auto Target = M->getOrInsertFunction("rep_runtime_target",
                    FunctionType::get(B.getInt32Ty(), {B.getInt32Ty()}, false));
  auto *Slot = new GlobalVariable(*M, B.getPtrTy(), false,
                    GlobalValue::ExternalLinkage, nullptr, "rep_runtime_slot");
  auto *FP = B.CreateLoad(B.getPtrTy(), Slot);
  Value *Indirect = B.CreateCall(Target.getFunctionType(), FP, {X});
  Value *Direct = B.CreateCall(Target, {X});
  auto *Src = B.CreateAlloca(B.getInt32Ty(), B.getInt32(64));
  auto *Dst = B.CreateAlloca(B.getInt32Ty(), B.getInt32(64));
  B.CreateMemSet(Src, B.getInt8(0), 256, Align(4));
  B.CreateStore(X, Src);
  // Keep the synthetic value-profile memory site within the fixed 64-element
  // scratch buffers even when the unequal-binding probe uses a large business
  // input (X=101). The clamp preserves a real memcpy/value-profile site while
  // avoiding a test-module buffer overrun in VP builds.
  Value *CopyElems = B.CreateSelect(
      B.CreateICmpULT(X, B.getInt32(64)), X, B.getInt32(64));
  Value *Bytes = B.CreateMul(B.CreateZExt(CopyElems, B.getInt64Ty()),
                             B.getInt64(4));
  B.CreateMemCpy(Dst, Align(4), Src, Align(4), Bytes);
  Value *Copied = B.CreateLoad(B.getInt32Ty(), Dst);
  Value *VPExtra = B.CreateAdd(B.CreateAdd(Indirect, Direct), Copied);
#endif

  Value *GainPtr =
      B.CreateGEP(ArrTy, Cfg, {B.getInt64(0), Cell, B.getInt32(0)}, "gp");
  LoadInst *Gain = B.CreateLoad(Type::getInt32Ty(Ctx), GainPtr, "gain");
  Gain->setMetadata("ejit.may_const", MDNode::get(Ctx, {}));
  Value *Cmp = B.CreateICmpUGT(X, Gain, "cmp");
  BasicBlock *Hot = BasicBlock::Create(Ctx, "hot", F);
  BasicBlock *Cold = BasicBlock::Create(Ctx, "cold", F);
  BasicBlock *Join = BasicBlock::Create(Ctx, "join", F);
  BasicBlock *Loop = BasicBlock::Create(Ctx, "loop", F);
  BasicBlock *Exit = BasicBlock::Create(Ctx, "exit", F);
  B.CreateCondBr(Cmp, Hot, Cold);

  B.SetInsertPoint(Hot);
  Value *H = B.CreateMul(X, Gain, "h");
  B.CreateBr(Join);

  B.SetInsertPoint(Cold);
  Value *C = B.CreateAdd(X, Gain, "c");
  B.CreateBr(Join);

  B.SetInsertPoint(Join);
  PHINode *V = B.CreatePHI(Type::getInt32Ty(Ctx), 2, "v");
  V->addIncoming(H, Hot);
  V->addIncoming(C, Cold);
  B.CreateBr(Loop);

  B.SetInsertPoint(Loop);
  PHINode *I = B.CreatePHI(Type::getInt32Ty(Ctx), 1, "i");
  PHINode *Acc = B.CreatePHI(Type::getInt32Ty(Ctx), 1, "acc");
  Value *Acc1 = B.CreateAdd(Acc, I, "acc1");
  Value *I1 = B.CreateAdd(I, B.getInt32(1), "i1");
  Value *LC = B.CreateICmpULT(I1, X, "lc");
  B.CreateCondBr(LC, Loop, Exit);
  I->addIncoming(B.getInt32(0), Join);
  I->addIncoming(I1, Loop);
  Acc->addIncoming(V, Join);
  Acc->addIncoming(Acc1, Loop);

  B.SetInsertPoint(Exit);
  Value *LivePtr = B.CreateGEP(ArrTy, Cfg,
                               {B.getInt64(0), Cell, B.getInt32(1), Trp}, "lp");
  Value *Live = B.CreateLoad(Type::getInt32Ty(Ctx), LivePtr, "live");
  Value *Sum = B.CreateAdd(Acc1, Live, "sum");
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  Sum = B.CreateAdd(Sum, VPExtra);
#endif
  B.CreateStore(Sum, LivePtr);
  Value *Ci = B.CreateTrunc(Cell, Type::getInt32Ty(Ctx), "ci");
  Value *Result = B.CreateAdd(Sum, Ci, "result");
  B.CreateRet(Result);
  return M;
}

class RepresentativeRuntime {
public:
  static void initializeTargets() {
    static std::once_flag Once;
    std::call_once(Once, [] {
      InitializeNativeTarget();
      InitializeNativeTargetAsmPrinter();
    });
  }

  // ---- process-global registration (constructor phase, before ejit_init) ----
  static std::string &bitcode() {
    static std::string B;
    return B;
  }
  static uint32_t &funcIndex() {
    static uint32_t I = 0;
    return I;
  }
  static uint32_t &cellSlot() {
    static uint32_t S = 0;
    return S;
  }
  static uint32_t &trpSlot() {
    static uint32_t S = 0;
    return S;
  }
  static uint32_t &profileRuntime() {
    static uint32_t C = 0;
    return C;
  }
  static CellRow *rows() {
    static CellRow R[kCells] = {};
    return R;
  }
  static std::vector<std::string> &pressureEntryNames() {
    static std::vector<std::string> Names = [] {
      std::vector<std::string> V;
      V.reserve(kPressureEntries);
      for (uint32_t I = 0; I < kPressureEntries; ++I)
        V.push_back("entry_rep_pressure_" + std::to_string(I));
      return V;
    }();
    return Names;
  }
  static std::vector<std::string> &pressureConfigNames() {
    static std::vector<std::string> Names = [] {
      std::vector<std::string> V;
      V.reserve(kPressureEntries);
      for (uint32_t I = 0; I < kPressureEntries; ++I)
        V.push_back("cfg_rep_pressure_" + std::to_string(I));
      return V;
    }();
    return Names;
  }
  static std::vector<std::string> &pressureBitcodes() {
    static std::vector<std::string> Bitcodes(kPressureEntries);
    return Bitcodes;
  }
  static std::vector<uint32_t> &pressureFuncIndices() {
    static std::vector<uint32_t> Indices(kPressureEntries, 0);
    return Indices;
  }
  static std::vector<CellRow> &pressureRows() {
    static std::vector<CellRow> Rows(kPressureEntries * kCells);
    return Rows;
  }
  static const char *entryName() { return "entry_rep_runtime"; }
};

/// Register the whole test fixture through the production C ABI, then bring the
/// runtime up through ejit_init_representative.
void initRuntimeOnce() {
  RepresentativeRuntime::initializeTargets();
  static std::once_flag Once;
  std::call_once(Once, [] {
    LLVMContext Ctx;
    auto M = buildEntryModule(Ctx, RepresentativeRuntime::entryName());
    std::string Err;
    raw_string_ostream OS(Err);
    if (verifyModule(*M, &OS))
      ADD_FAILURE() << "entry module verify failed: " << OS.str();
    std::string BC;
    raw_string_ostream BOS(BC);
    WriteBitcodeToFile(*M, BOS);
    BOS.flush();
    RepresentativeRuntime::bitcode() = BC;

    for (uint32_t I = 0; I < kCells; ++I) {
      RepresentativeRuntime::rows()[I].gain = 7;
      RepresentativeRuntime::rows()[I].live[0] = 0;
      RepresentativeRuntime::rows()[I].live[1] = 0;
    }

    RepresentativeRuntime::rows()[6].gain = 13;
    RepresentativeRuntime::rows()[7].gain = 13;
    RepresentativeRuntime::rows()[8].gain = 17;
    RepresentativeRuntime::rows()[9].gain = 17;
    RepresentativeRuntime::rows()[10].gain = 23;
    RepresentativeRuntime::rows()[11].gain = 23;
    RepresentativeRuntime::rows()[12].gain = 29;

    ejit_register_period_array("cell", "cfg", RepresentativeRuntime::rows(), kCells);
    ejit_register_symbol("cfg", RepresentativeRuntime::rows());
    ejit_register_symbol("rep_runtime_pause", reinterpret_cast<void *>(&rep_runtime_pause));
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    ejit_register_symbol("rep_runtime_target", reinterpret_cast<void *>(&rep_runtime_target));
    ejit_register_symbol("rep_runtime_slot", &RuntimeTarget);
#endif
    ejit_register_symbol("__llvm_profile_runtime",
                         &RepresentativeRuntime::profileRuntime());
    ejit_register_bitcode(
        RepresentativeRuntime::entryName(),
        reinterpret_cast<const uint8_t *>(RepresentativeRuntime::bitcode().data()),
        RepresentativeRuntime::bitcode().size());
    ejit_register_funcindex(RepresentativeRuntime::entryName(),
                            &RepresentativeRuntime::funcIndex());

    // The pressure matrix uses twenty separately registered entry functions
    // and a private period-array backing store per entry.  All arrays share
    // the same lifecycle name, so activation semantics remain identical to an
    // AOT wrapper while each entry's effective binding and live stores stay
    // isolated.  Entry zero deliberately gives cell five a different gain to
    // exercise the unequal-member fallback path.
    auto &PressureRows = RepresentativeRuntime::pressureRows();
    auto &PressureNames = RepresentativeRuntime::pressureEntryNames();
    auto &PressureConfigNames = RepresentativeRuntime::pressureConfigNames();
    auto &PressureBitcodes = RepresentativeRuntime::pressureBitcodes();
    auto &PressureFuncIndices = RepresentativeRuntime::pressureFuncIndices();
    for (uint32_t Entry = 0; Entry < kPressureEntries; ++Entry) {
      for (uint32_t Cell = 0; Cell < kCells; ++Cell) {
        CellRow &Row = PressureRows[Entry * kCells + Cell];
        Row.gain = (Entry == 0 && Cell == 5) ? 101u : 7u;
        Row.live[0] = 0;
        Row.live[1] = 0;
      }
      LLVMContext PressureCtx;
      auto PressureModule =
          buildEntryModule(PressureCtx, PressureNames[Entry],
                           PressureConfigNames[Entry]);
      std::string PressureErr;
      raw_string_ostream PressureOS(PressureErr);
      if (verifyModule(*PressureModule, &PressureOS))
        ADD_FAILURE() << "pressure entry module verify failed: "
                      << PressureNames[Entry] << ": " << PressureOS.str();
      std::string PressureBC;
      raw_string_ostream PressureBOS(PressureBC);
      WriteBitcodeToFile(*PressureModule, PressureBOS);
      PressureBOS.flush();
      PressureBitcodes[Entry] = std::move(PressureBC);
      ejit_register_period_array(
          "cell", PressureConfigNames[Entry].c_str(),
          &PressureRows[Entry * kCells], kCells);
      ejit_register_bitcode(
          PressureNames[Entry].c_str(),
          reinterpret_cast<const uint8_t *>(PressureBitcodes[Entry].data()),
          PressureBitcodes[Entry].size());
      ejit_register_funcindex(PressureNames[Entry].c_str(),
                              &PressureFuncIndices[Entry]);
    }
    ejit_register_lifecycle("cell", &RepresentativeRuntime::cellSlot());
    ejit_register_lifecycle("trp", &RepresentativeRuntime::trpSlot());
  });
}

class EJitRepresentativeRuntimeTest : public ::testing::Test {
protected:
  void SetUp() override {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    initRuntimeOnce();
  }
};

/// The V1 gate: the opt-in is accepted only for Async + normal online PGO.
TEST_F(EJitRepresentativeRuntimeTest,
       InitRejectsRepresentativeSharingWithPgoOff) {
  // Real init rejection preserves the staged registrations for the next test.
  ejit_config_t Sync{};
  Sync.compileMode = EJIT_COMPILE_SYNC;
  EXPECT_EQ(ejit_init_representative(&Sync), EJIT_ERR_INVALID_PARAM);
  EXPECT_EQ(ejit_representative_test_pool(), nullptr);
  for (bool Audit : {false, true}) {
    Config C;
    C.enableRepresentativeSharing = true;
    C.compileMode = CompileMode::Async;
    C.enablePgo = false;
    C.enableProfileAudit = Audit;
    EJit Rejected(C);
    EXPECT_TRUE(Rejected.initFailed());
    EXPECT_EQ(Rejected.initError().code, EJIT_ERR_INVALID_PARAM);
    EXPECT_EQ(Rejected.compileDriver(), nullptr);
  }
  for (CompileMode Mode : {CompileMode::Off, CompileMode::Sync,
                           CompileMode::Async}) {
    Config C;
    C.enableRepresentativeSharing = true;
    C.compileMode = Mode;
    C.enablePgo = true;
    C.enableProfileAudit = true;
    if (Mode == CompileMode::Async)
      C.representativeIdleTimeoutTicks = 0;
    EJit Rejected(C);
    EXPECT_TRUE(Rejected.initFailed());
    EXPECT_EQ(Rejected.initError().code, EJIT_ERR_INVALID_PARAM);
    EXPECT_EQ(Rejected.compileDriver(), nullptr);
  }
  EJitGroupAdmissionPolicy Policy;
  Policy.pgoEnabled = false;
  Policy.asyncService = true;
  Policy.normalOnlinePgo = true;
  Policy.dispatchQuota = kQuota;
  EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Policy),
            EJitGroupAdmitReject::PgoDisabled);

  Policy.pgoEnabled = true;
  Policy.asyncService = false;
  EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Policy),
            EJitGroupAdmitReject::NotAsync);

  Policy.asyncService = true;
  Policy.normalOnlinePgo = false;
  EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Policy),
            EJitGroupAdmitReject::AuditOnly);

  Policy.normalOnlinePgo = true;
  Policy.modeChangeInFlight = true;
  EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Policy),
            EJitGroupAdmitReject::ModeChangeInFlight);

  Policy.modeChangeInFlight = false;
  Policy.dispatchQuota = 0;
  EXPECT_EQ(EJitRepresentativeGroupRegistry::admissionReject(Policy),
            EJitGroupAdmitReject::ZeroQuota);
}

TEST_F(EJitRepresentativeRuntimeTest,
       InitAllowsOnlinePgoWithBuildDefaultAuditDiagnostics) {
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  EXPECT_TRUE(Config{}.enableProfileAudit);
#else
  EXPECT_FALSE(Config{}.enableProfileAudit);
#endif
  ejit_config_t Cfg{};
  Cfg.compileMode = EJIT_COMPILE_ASYNC;
  ASSERT_EQ(ejit_init_representative(&Cfg), EJIT_OK);
  ejit_representative_stats_t Stats{};
  ASSERT_EQ(ejit_representative_get_stats(&Stats), EJIT_OK);
  EXPECT_EQ(Stats.active, 1u);
  EXPECT_EQ(Stats.groups, 0u);
  auto *Pool = static_cast<EJitSharedTaskPool *>(ejit_representative_test_pool());
  ASSERT_NE(Pool, nullptr);
  EXPECT_EQ(Pool->pendingCount(), 0u);
  // Keep the one staged registration set for the generated-code tests below.
}

namespace {

/// The real business entry: exactly the call the AOT wrapper makes. Returns the
/// resolve status and captures the granted pointer / read token.
ejit_status_t entryCallFor(uint32_t funcIndex, uint32_t cell, uint32_t trp,
                            void **outFn, uint32_t *outBucket);

ejit_status_t entryCall(uint32_t cell, uint32_t trp, void **outFn,
                        uint32_t *outBucket) {
  return entryCallFor(RepresentativeRuntime::funcIndex(), cell, trp, outFn,
                      outBucket);
}

ejit_status_t entryCallFor(uint32_t funcIndex, uint32_t cell, uint32_t trp,
                            void **outFn, uint32_t *outBucket) {
  ejit_dim_pair_t Dims[2] = {{RepresentativeRuntime::cellSlot(), cell},
                             {RepresentativeRuntime::trpSlot(), trp}};
  *outFn = nullptr;
  *outBucket = 0;
  return ejit_taskpool_compile_or_get(funcIndex, Dims, 2, outFn, outBucket);
}

// AOT reference and observable store check; callers still own the read token.
void executeAndCheckRows(CellRow *Rows, void *Fn, uint32_t Cell,
                         uint32_t Trp, uint32_t X) {
  auto &Row = Rows[Cell];
  const uint32_t Before = Row.live[Trp];
  uint32_t Expected = Before + (X > Row.gain ? X * Row.gain : X + Row.gain)
                            + X * (X - 1) / 2;
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  Expected += X * 5 + 10;
#endif
  using Entry = uint32_t (*)(uint64_t, uint64_t, uint32_t);
  EXPECT_EQ(reinterpret_cast<Entry>(Fn)(Cell, Trp, X), Expected + Cell);
  EXPECT_EQ(Row.live[Trp], Expected);
}

void executeAndCheck(void *Fn, uint32_t Cell, uint32_t Trp, uint32_t X) {
  executeAndCheckRows(RepresentativeRuntime::rows(), Fn, Cell, Trp, X);
}

void releaseIfHeld(uint32_t status, uint32_t bucket) {
  if (status == EJIT_OK)
    ejit_taskpool_release_read(bucket);
}

} // namespace

/// The production-path acceptance: ONE entry, six legal cells, ONE
/// representative Tier-1 whose 64 real granted dispatches freeze the group
/// quota once, and ONE shared Tier-2 object that every member resolves through
/// its own real PGOUse consumption of the group's published bundle.
TEST_F(EJitRepresentativeRuntimeTest,
       SixCellsShareOneRepresentativeTier1AndOneSharedTier2) {
  // The representative is the FIRST legal arrival, not a hardcoded cell 0: cell
  // 1 arrives first on purpose.
  ejit_config_t Cfg{};
  Cfg.compileMode = EJIT_COMPILE_ASYNC;
  ASSERT_EQ(ejit_init_representative(&Cfg), EJIT_OK)
      << "ejit_init_representative must accept Async + normal online PGO";
  // Diagnostics on: this test is the runtime-path evidence, so the production
  // group/bundle/publish lines must be visible in its log.
  ejit_set_log_level(EJIT_LOG_VERBOSE);

  auto *ModePool = static_cast<EJitSharedTaskPool *>(ejit_representative_test_pool());
  ASSERT_NE(ModePool, nullptr);
  const auto Epoch = ModePool->state()->dispatchEpoch.loadAcquire();
  ejit_set_compile_mode(EJIT_COMPILE_SYNC);
  EXPECT_EQ(ejit_get_compile_mode(), EJIT_COMPILE_ASYNC);
  EXPECT_EQ(ModePool->getSharedMode(), EJitCompileMode::Async);
  EXPECT_EQ(ModePool->state()->dispatchEpoch.loadAcquire(), Epoch);
  EXPECT_EQ(ModePool->pendingCount(), 0u);

  ASSERT_EQ(ejit_activate("cell", 0), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 1), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 2), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 3), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 4), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 5), EJIT_OK);
  ASSERT_EQ(ejit_activate("trp", 1), EJIT_OK);

  // Registration sanity: the SAME process-global funcIndex the wrapper would
  // bake into its request must resolve this entry's bitcode.
  {
    ejit_representative_stats_t Probe{};
    (void)ejit_representative_get_stats(&Probe);
    EXPECT_EQ(Probe.active, 1u)
        << "the representative opt-in must really drive this runtime; "
           "funcIndex=" << RepresentativeRuntime::funcIndex()
        << " cellSlot=" << RepresentativeRuntime::cellSlot()
        << " trpSlot=" << RepresentativeRuntime::trpSlot();
  }

  // ---- the representative's own Tier-1 request enters the real queue ----
  void *Fn = nullptr;
  uint32_t Bucket = 0;
  const ejit_status_t FirstStatus = entryCall(1, 1, &Fn, &Bucket);
  EXPECT_NE(FirstStatus, EJIT_ERR_INVALID_PARAM);
  // The worker is running in Async mode, so the first resolve either already
  // carries the Instrumented pointer or reports it pending; both mean the
  // request went through the real admission path.
  EXPECT_TRUE(FirstStatus == EJIT_OK || FirstStatus == EJIT_PENDING)
      << "status=" << FirstStatus;
  if (FirstStatus == EJIT_OK && Fn)
    executeAndCheck(Fn, 1, 1, 11);
  releaseIfHeld(FirstStatus, Bucket);

  // ---- settle the queued Instrumented compile (production worker) ----
  void *T1Fn = FirstStatus == EJIT_OK ? Fn : nullptr;
  for (unsigned Attempt = 0; Attempt < 4000 && !T1Fn; ++Attempt) {
    void *DispatchFn = nullptr;
    uint32_t DispatchBucket = 0;
    const ejit_status_t S = entryCall(1, 1, &DispatchFn, &DispatchBucket);
    if (S == EJIT_OK && DispatchFn) {
      T1Fn = DispatchFn;
      executeAndCheck(DispatchFn, 1, 1, 11);
    }
    releaseIfHeld(S, DispatchBucket);
    if (!T1Fn) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_NE(T1Fn, nullptr)
      << "the representative's real Instrumented Tier-1 must be published by "
         "the production compile path";

  // Every non-representative is classified by the worker before admission.
  // All five stay on AOT and join without a private T1 or sample quota.
  uint32_t Joined = 0;
  for (uint32_t Cell : {0u, 2u, 3u, 4u, 5u}) {
    ejit_representative_stats_t St{};
    for (unsigned Attempt = 0; Attempt < 4000; ++Attempt) {
      void *F = nullptr;
      uint32_t B = 0;
      const auto S = entryCall(Cell, 1, &F, &B);
      EXPECT_EQ(F, nullptr);
      releaseIfHeld(S, B);
      EXPECT_EQ(ejit_representative_get_stats(&St), EJIT_OK);
      if (St.waitersJoined == Joined + 1) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(St.waitersJoined, ++Joined);
  }

  // Fail one real worker T2 compile before capture. The normal closed-quota
  // lookup must enqueue the retry without granting another T1 execution.
  ASSERT_EQ(ejit_representative_test_fail_next_tier2(), EJIT_OK);
  ASSERT_EQ(ejit_representative_test_fail_member_tier2(1), EJIT_OK);
  EJitSharedDiagnostics RetryBefore{};
  auto *RetryPool = static_cast<EJitSharedTaskPool *>(ejit_representative_test_pool());
  ASSERT_NE(RetryPool, nullptr);
  RetryPool->getDiagnostics(RetryBefore);
  RetryPool->failNextTier2QueuePushForTest();

  // ---- 64 REAL granted Tier-1 dispatches through the real entry ----
  uint32_t Granted = 1; // the settle call above was one granted dispatch
  for (uint32_t I = 1; I < kQuota; ++I) {
    void *DispatchFn = nullptr;
    uint32_t DispatchBucket = 0;
    const ejit_status_t S = entryCall(1, 1, &DispatchFn, &DispatchBucket);
    ASSERT_EQ(S, EJIT_OK) << "granted dispatch " << I << " status=" << S;
    ASSERT_NE(DispatchFn, nullptr) << "dispatch " << I;
    EXPECT_EQ(DispatchFn, T1Fn)
        << "every granted dispatch must resolve to the ONE representative "
           "Tier-1 object";
    if (I == kQuota - 1) {
      PauseLast.store(true, std::memory_order_release);
      std::thread Last([&] { executeAndCheck(DispatchFn, 1, 1, 3); });
      const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!LastEntered.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      EXPECT_TRUE(LastEntered.load(std::memory_order_acquire));
      // T1 is inside generated code with its original read token still live.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      ejit_representative_stats_t During{};
      EXPECT_EQ(ejit_representative_get_stats(&During), EJIT_OK);
      EXPECT_EQ(During.bundlePublications, 0u);
      ResumeLast.store(true, std::memory_order_release);
      Last.join();
      PauseLast.store(false, std::memory_order_release);
    } else {
      executeAndCheck(DispatchFn, 1, 1, I < 48 ? 11 : 3);
    }
    ++Granted;
    releaseIfHeld(S, DispatchBucket);
  }
  EXPECT_EQ(Granted, kQuota)
      << "the group quota must be met by the representative ALONE";

  // The production diagnostics must already agree with the real dispatches.
  {
    ejit_representative_stats_t St{};
    ASSERT_EQ(ejit_representative_get_stats(&St), EJIT_OK);
    EXPECT_EQ(St.active, 1u) << "the opt-in must really drive the runtime";
    EXPECT_EQ(St.groups, 1u);
    EXPECT_EQ(St.representativesElected, 1u);
    EXPECT_EQ(St.representativeDispatches, kQuota)
        << "the pool's committed-dispatch hook must own the group quota";
    EXPECT_EQ(St.representativeDispatchCount, kQuota);
    EXPECT_EQ(St.representativeDispatchLimit, kQuota);
    EXPECT_EQ(St.logicalRequests, kQuota);
  }

  // The 64th real dispatch closed the admission and armed the Tier-2 request.
  // Drive the real queue until the shared Tier-2 is published.
  void *T2Fn = nullptr;
  for (unsigned Attempt = 0; Attempt < 4000 && !T2Fn; ++Attempt) {
    void *F = nullptr;
    uint32_t B = 0;
    const ejit_status_t S = entryCall(1, 1, &F, &B);
    if (S == EJIT_OK && F) {
      if (F != T1Fn) T2Fn = F;
      executeAndCheck(F, 1, 1, 9);
    }
    releaseIfHeld(S, B);
    if (!T2Fn) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  {
    ejit_representative_stats_t St{};
    (void)ejit_representative_get_stats(&St);
    EXPECT_EQ(St.bundlePublications, 1u)
        << "the real Tier-2 request must publish the group bundle once "
           "(bundlesPublished="
        << St.bundlesPublished << " groups=" << St.groups
        << " pending=" << ejit_taskpool_pending_count() << ")";
    EXPECT_EQ(St.bundleDispatchCount, kQuota);
    EXPECT_EQ(St.bundleDispatchLimit, kQuota);
  }
  EXPECT_NE(T2Fn, nullptr)
      << "the real Tier-2 request must publish the shared final object";
  EXPECT_NE(T2Fn, T1Fn) << "Tier-1 and Tier-2 provenance must stay distinct";

  // Read the production frozen profile, not a synthetic writer input.
  size_t ProfileSize = 0;
  ASSERT_EQ(ejit_representative_copy_profile(nullptr, 0, &ProfileSize), EJIT_OK);
  ASSERT_GT(ProfileSize, 0u);
  std::string Profile(ProfileSize, '\0');
  ASSERT_EQ(ejit_representative_copy_profile(&Profile[0], Profile.size(),
                                             &ProfileSize), EJIT_OK);
  auto Reader = IndexedInstrProfReader::create(MemoryBuffer::getMemBufferCopy(Profile));
  ASSERT_TRUE(static_cast<bool>(Reader)) << toString(Reader.takeError());
  unsigned Records = 0;
  for (const auto &Record : **Reader) {
    ++Records;
    EXPECT_EQ(Record.Name, RepresentativeRuntime::entryName());
    std::vector<uint64_t> Counts = Record.Counts;
    std::sort(Counts.begin(), Counts.end());
    for (uint64_t C : Counts) llvm::outs() << "frozen edge counter=" << C << "\n";
    // IR-PGO stores hot edge, entry, and loop backedge. Cold is entry-hot:
    // 64-48=16, including the final call; backedges are 48*10 + 16*2=512.
    EXPECT_EQ(Counts, (std::vector<uint64_t>{48, 64, 512}));
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    ASSERT_EQ(Record.getNumValueSites(IPVK_IndirectCallTarget), 1u);
    ASSERT_EQ(Record.getNumValueSites(IPVK_MemOPSize), 1u);
    auto IC = Record.getValueArrayForSite(IPVK_IndirectCallTarget, 0);
    ASSERT_EQ(IC.size(), 1u);
    EXPECT_EQ(IC[0].Count, 64u);
    EXPECT_EQ(IC[0].Value, IndexedInstrProf::ComputeHash("rep_runtime_target"));
    auto Mem = Record.getValueArrayForSite(IPVK_MemOPSize, 0);
    ASSERT_EQ(Mem.size(), 2u);
    EXPECT_EQ(Mem[0].Value, 44u);
    EXPECT_EQ(Mem[0].Count, 48u);
    EXPECT_EQ(Mem[1].Value, 12u);
    EXPECT_EQ(Mem[1].Count, 16u);
#endif
  }
  EXPECT_EQ(Records, 1u);
  EXPECT_FALSE((*Reader)->hasError());
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  ejit_vp_stats_t VP{};
  ASSERT_EQ(ejit_vp_get_stats(&VP), EJIT_OK);
  EXPECT_EQ(VP.merges, 1u);
  EXPECT_EQ(VP.icValueSites, 1u);
  EXPECT_EQ(VP.memopValueSites, 1u);
  EXPECT_EQ(VP.scalarValueSites, 1u);
  size_t ScalarBytes = 0;
  ASSERT_EQ(ejit_representative_copy_scalar_profile(nullptr, 0, &ScalarBytes), EJIT_OK);
  ASSERT_EQ(ScalarBytes, sizeof(PgoScalarSite));
  PgoScalarSite Scalar{};
  ASSERT_EQ(ejit_representative_copy_scalar_profile(&Scalar, sizeof(Scalar), &ScalarBytes), EJIT_OK);
  EXPECT_EQ(Scalar.total, 64u);
  EXPECT_EQ(Scalar.topCount, 48u);
  EXPECT_EQ(Scalar.topValue, 11u);
  EXPECT_EQ(Scalar.funcHash, IndexedInstrProf::ComputeHash(RepresentativeRuntime::entryName()));
#endif

  // Owner scheduling must finish waiting members even without another member
  // dispatch. A completed final read releases each retained candidate borrow.
  for (unsigned A = 0; A < 4000; ++A) {
    ejit_representative_stats_t St{};
    ASSERT_EQ(ejit_representative_get_stats(&St), EJIT_OK);
    if (St.sharedPhysicalReuses == 5) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  {
    ejit_representative_stats_t St{};
    ASSERT_EQ(ejit_representative_get_stats(&St), EJIT_OK);
    EXPECT_EQ(St.sharedPhysicalReuses, 5u);
  }

  for (unsigned A = 0; A < 4000 && RetryPool->liveRequestAttemptCount(); ++A)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(RetryPool->liveRequestAttemptCount(), 0u);
  EJitSharedDiagnostics RetryAfter{};
  RetryPool->getDiagnostics(RetryAfter);
  EXPECT_EQ(RetryAfter.compileFailed, RetryBefore.compileFailed + 2);
  EXPECT_EQ(RetryAfter.queueFull, RetryBefore.queueFull + 1);

  // ---- every member now consumes the SAME published bundle ----
  //
  // Each member must pass the production final-IR and binding comparison.
  for (uint32_t Cell : {0u, 2u, 3u, 4u, 5u}) {
    void *MemberFn = nullptr;
    for (unsigned Attempt = 0; Attempt < 4000 && !MemberFn; ++Attempt) {
      void *F = nullptr;
      uint32_t B = 0;
      const ejit_status_t S = entryCall(Cell, 1, &F, &B);
      if (S == EJIT_OK && F) {
        MemberFn = F;
        executeAndCheck(F, Cell, 1, Cell + 2);
      }
      releaseIfHeld(S, B);
      if (!MemberFn) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    ASSERT_NE(MemberFn, nullptr)
        << "member cell " << Cell
        << " must resolve through the real pool after publication";
    EXPECT_EQ(MemberFn, T2Fn)
        << "equal final IR and bindings must reuse physical T2 for cell " << Cell;
  }

  // The production diagnostics, read AFTER the members consumed the profile:
  // exactly ONE group, five waiters, ONE published bundle, and one physical
  // object shared by six separately dispatched logical members.
  {
    ejit_representative_stats_t St{};
    ASSERT_EQ(ejit_representative_get_stats(&St), EJIT_OK);
    EXPECT_EQ(St.groups, 1u);
    EXPECT_EQ(St.representativesElected, 1u);
    EXPECT_EQ(St.waitersJoined, 5u)
        << "the five non-representatives must join as waiters";
    EXPECT_EQ(St.bundlePublications, 1u)
        << "the immutable bundle must be published exactly once";
    EXPECT_EQ(St.bundleDispatchCount, kQuota);
    EXPECT_EQ(St.bundleDispatchLimit, kQuota);
    EXPECT_EQ(St.bundleGeneration, 1u);
    EXPECT_EQ(St.representativeDispatchCount, kQuota);
    EXPECT_EQ(St.sharedPhysicalReuses, 5u);
    EXPECT_EQ(St.physicalCodeObjects, 1u);
  }
  // Same entry, different may_const prefix: a SECOND sampling group.
  ASSERT_EQ(ejit_activate("cell", 6), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 7), EJIT_OK);
  void *SecondT1 = nullptr;
  for (unsigned Attempt = 0; Attempt < 4000 && !SecondT1; ++Attempt) {
    void *F = nullptr;
    uint32_t B = 0;
    const auto S = entryCall(6, 1, &F, &B);
    if (S == EJIT_OK && F) {
      SecondT1 = F;
      executeAndCheck(F, 6, 1, 19);
    }
    releaseIfHeld(S, B);
    if (!SecondT1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_NE(SecondT1, nullptr);
  EXPECT_NE(SecondT1, T1Fn);
  ejit_representative_stats_t Second{};
  for (unsigned Attempt = 0; Attempt < 4000; ++Attempt) {
    void *F = nullptr;
    uint32_t B = 0;
    const auto S = entryCall(7, 1, &F, &B);
    EXPECT_EQ(F, nullptr);
    releaseIfHeld(S, B);
    ASSERT_EQ(ejit_representative_get_group_stats(1, &Second), EJIT_OK);
    if (Second.waitersLive == 1u) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(Second.waitersLive, 1u);
  EXPECT_EQ(Second.bundlePublications, 1u); // only the first group is frozen
  for (uint32_t I = 1; I < kQuota; ++I) {
    void *F = nullptr;
    uint32_t B = 0;
    auto S = entryCall(6, 1, &F, &B);
    ASSERT_EQ(S, EJIT_OK);
    EXPECT_EQ(F, SecondT1);
    executeAndCheck(F, 6, 1, I < 16 ? 19 : 5);
    releaseIfHeld(S, B);
    // Group one stays executable without reopening its profile or quota.
    S = entryCall(1, 1, &F, &B);
    ASSERT_EQ(S, EJIT_OK);
    EXPECT_EQ(F, T2Fn);
    executeAndCheck(F, 1, 1, 7);
    releaseIfHeld(S, B);
  }
  void *SecondT2 = nullptr;
  for (uint32_t Cell : {6u, 7u}) {
    void *Resolved = nullptr;
    for (unsigned Attempt = 0; Attempt < 4000 && !Resolved; ++Attempt) {
      void *F = nullptr;
      uint32_t B = 0;
      const auto S = entryCall(Cell, 1, &F, &B);
      if (S == EJIT_OK && F) {
        EXPECT_NE(F, SecondT1);
        Resolved = F;
        executeAndCheck(F, Cell, 1, Cell + 2);
      }
      releaseIfHeld(S, B);
      if (!Resolved) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_NE(Resolved, nullptr);
    if (!SecondT2) SecondT2 = Resolved;
    EXPECT_EQ(Resolved, SecondT2);
    EXPECT_NE(Resolved, T2Fn); // unequal final IR never aliases group one
  }
  ASSERT_EQ(ejit_representative_get_group_stats(1, &Second), EJIT_OK);
  ejit_representative_stats_t First{};
  ASSERT_EQ(ejit_representative_get_stats(&First), EJIT_OK);
  EXPECT_EQ(Second.groups, 2u);
  EXPECT_EQ(Second.bundlePublications, 2u);
  EXPECT_EQ(Second.physicalCodeObjects, 2u);
  EXPECT_EQ(Second.sharedPhysicalReuses, 6u);
  EXPECT_EQ(Second.representativeDispatches, 128u);
  EXPECT_EQ(First.representativeDispatchCount, 64u);
  EXPECT_EQ(Second.representativeDispatchCount, 64u);
  EXPECT_NE(First.representativeSamplingSessionId, Second.representativeSamplingSessionId);
  EXPECT_NE(First.representativeAttemptToken, Second.representativeAttemptToken);
  EXPECT_NE(First.bundleQuotaEnd, Second.bundleQuotaEnd);
  for (uint32_t Group : {0u, 1u}) {
    size_t Size = 0;
    ASSERT_EQ(ejit_representative_copy_group_profile(Group, false, nullptr, 0, &Size), EJIT_OK);
    std::string Data(Size, '\0');
    ASSERT_EQ(ejit_representative_copy_group_profile(Group, false, &Data[0], Data.size(), &Size), EJIT_OK);
    if (Group == 0) EXPECT_EQ(Data, Profile); // immutable across the second session
    auto R = IndexedInstrProfReader::create(MemoryBuffer::getMemBufferCopy(Data));
    ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
    for (const auto &Rec : **R) {
      auto Counts = Rec.Counts;
      std::sort(Counts.begin(), Counts.end());
      EXPECT_EQ(Counts, Group == 0 ? (std::vector<uint64_t>{48,64,512})
                                 : (std::vector<uint64_t>{16,64,480}));
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
      auto IC = Rec.getValueArrayForSite(IPVK_IndirectCallTarget, 0);
      ASSERT_EQ(IC.size(), 1u);
      EXPECT_EQ(IC[0].Count, 64u);
      auto Mem = Rec.getValueArrayForSite(IPVK_MemOPSize, 0);
      ASSERT_EQ(Mem.size(), 2u);
      EXPECT_EQ(Mem[0].Value, Group == 0 ? 44u : 20u);
      EXPECT_EQ(Mem[0].Count, 48u);
      EXPECT_EQ(Mem[1].Value, Group == 0 ? 12u : 76u);
      EXPECT_EQ(Mem[1].Count, 16u);
#endif
    }
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    PgoScalarSite Scalar{};
    ASSERT_EQ(ejit_representative_copy_group_profile(Group, true, &Scalar, sizeof(Scalar), &Size), EJIT_OK);
    EXPECT_EQ(Scalar.total, 64u);
    EXPECT_EQ(Scalar.topCount, 48u);
    EXPECT_EQ(Scalar.topValue, Group == 0 ? 11u : 5u);
#endif
  }

  // Cancel a real, partially sampled third representative through the pool.
  // Its replacement must use a fresh collector and exactly 64 new executions.
  ASSERT_EQ(ejit_activate("cell", 8), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 9), EJIT_OK);
  auto *Pool = static_cast<EJitSharedTaskPool *>(ejit_representative_test_pool());
  ASSERT_NE(Pool, nullptr);
  uint64_t CancelToken = 0;
  uint32_t CancelBucket = kEJitSharedCacheBuckets;
  void *CancelledT1 = nullptr;
  for (unsigned A = 0; A < 4000 && !CancelledT1; ++A) {
    void *F = nullptr; uint32_t B = 0;
    auto S = entryCall(8, 1, &F, &B);
    if (S == EJIT_OK && F) {
      CancelledT1 = F;
      CancelBucket = B;
      // The real sampling read token pins this slot in either build variant.
      EXPECT_LT(B, kEJitSharedCacheBuckets);
      if (B < kEJitSharedCacheBuckets)
        for (const auto &Slot : Pool->state()->buckets[B].slots)
          if (Slot.fnPtr.loadAcquire() == reinterpret_cast<uintptr_t>(F))
            CancelToken = Slot.attemptToken;
    }
    if (!CancelledT1) releaseIfHeld(S, B);
    if (!CancelledT1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_NE(CancelledT1, nullptr);
  ASSERT_NE(CancelToken, 0u);
  ejit_representative_stats_t BeforeCancel{};
  ASSERT_EQ(ejit_representative_get_group_stats(2, &BeforeCancel), EJIT_OK);
  EXPECT_EQ(BeforeCancel.representativeDispatchCount, 1u);
  PauseLast.store(true, std::memory_order_release);
  LastEntered.store(false, std::memory_order_release);
  ResumeLast.store(false, std::memory_order_release);
  std::thread OldExecution([&] { executeAndCheck(CancelledT1, 8, 1, 27); });
  const auto CancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!LastEntered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < CancelDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_TRUE(LastEntered.load(std::memory_order_acquire));
  std::atomic<bool> CancelDone{false};
  bool CancelSucceeded = false;
  std::thread CancelWorker([&] {
    CancelSucceeded = Pool->cancelRequestAttempt(CancelToken, EJitRequestAttemptReason::Cancelled);
    CancelDone.store(true, std::memory_order_release);
  });
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  while (!CancelDone.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < CancelDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_TRUE(CancelDone.load(std::memory_order_acquire));
  // Retracted slot, still-running old root: enqueue the replacement now. Its
  // worker must not replace code/profile storage until this token is released.
  if (CancelDone.load(std::memory_order_acquire)) {
    void *F = nullptr; uint32_t B = 0;
    auto S = entryCall(8, 1, &F, &B);
    EXPECT_EQ(F, nullptr);
    releaseIfHeld(S, B);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    S = entryCall(8, 1, &F, &B);
    EXPECT_EQ(F, nullptr);
    releaseIfHeld(S, B);
  }
#else
  // Reclaiming cancellation waits on the original read token itself.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(CancelDone.load(std::memory_order_acquire));
#endif
  ResumeLast.store(true, std::memory_order_release);
  OldExecution.join();
  PauseLast.store(false, std::memory_order_release);
  Pool->releaseRead(CancelBucket);
  CancelWorker.join();
  EXPECT_TRUE(CancelSucceeded);
  EXPECT_FALSE(Pool->cancelRequestAttempt(CancelToken, EJitRequestAttemptReason::Cancelled));
  void *ReplacementT1 = nullptr;
  uint64_t ReplacementToken = 0;
  for (unsigned I = 0; I < kQuota; ++I) {
    bool Executed = false;
    for (unsigned A = 0; A < 4000 && !Executed; ++A) {
      void *F = nullptr; uint32_t B = 0;
      auto S = entryCall(8, 1, &F, &B);
      if (S == EJIT_OK && F) {
        if (!ReplacementT1) {
          ReplacementT1 = F;
          EXPECT_LT(B, kEJitSharedCacheBuckets);
          if (B < kEJitSharedCacheBuckets)
            for (const auto &Slot : Pool->state()->buckets[B].slots)
              if (Slot.fnPtr.loadAcquire() == reinterpret_cast<uintptr_t>(F))
                ReplacementToken = Slot.attemptToken;
        }
        EXPECT_EQ(F, ReplacementT1);
        executeAndCheck(F, 8, 1, I < 48 ? 21 : 3);
        Executed = true;
      }
      releaseIfHeld(S, B);
      if (!Executed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(Executed) << "replacement sample " << I;
  }
  // A released address may be reused; lifecycle identity is the request token.
  EXPECT_NE(ReplacementToken, 0u);
  EXPECT_NE(ReplacementToken, CancelToken);
  void *ThirdT2 = nullptr;
  for (uint32_t Cell : {8u, 9u}) {
    void *Resolved = nullptr;
    for (unsigned A = 0; A < 4000 && !Resolved; ++A) {
      void *F = nullptr; uint32_t B = 0;
      auto S = entryCall(Cell, 1, &F, &B);
      if (S == EJIT_OK && F) {
        EXPECT_NE(F, ReplacementT1);
        executeAndCheck(F, Cell, 1, 7);
        Resolved = F;
      }
      releaseIfHeld(S, B);
      if (!Resolved) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_NE(Resolved, nullptr);
    if (!ThirdT2) ThirdT2 = Resolved;
    EXPECT_EQ(Resolved, ThirdT2);
    EXPECT_NE(Resolved, T2Fn);
    EXPECT_NE(Resolved, SecondT2);
  }
  ejit_representative_stats_t AfterCancel{};
  ASSERT_EQ(ejit_representative_get_group_stats(2, &AfterCancel), EJIT_OK);
  EXPECT_EQ(AfterCancel.bundleGeneration, 2u);
  EXPECT_EQ(AfterCancel.bundleDispatchCount, 64u);
  EXPECT_NE(AfterCancel.representativeSamplingSessionId,
            BeforeCancel.representativeSamplingSessionId);
  size_t ThirdSize = 0;
  ASSERT_EQ(ejit_representative_copy_group_profile(2, false, nullptr, 0, &ThirdSize), EJIT_OK);
  std::string ThirdProfile(ThirdSize, '\0');
  ASSERT_EQ(ejit_representative_copy_group_profile(2, false, &ThirdProfile[0], ThirdSize, &ThirdSize), EJIT_OK);
  auto ThirdReader = IndexedInstrProfReader::create(MemoryBuffer::getMemBufferCopy(ThirdProfile));
  ASSERT_TRUE(static_cast<bool>(ThirdReader)) << toString(ThirdReader.takeError());
  for (const auto &Rec : **ThirdReader) {
    auto Counts = Rec.Counts;
    std::sort(Counts.begin(), Counts.end());
    EXPECT_EQ(Counts, (std::vector<uint64_t>{48,64,992}));
  }
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  PgoScalarSite ThirdScalar{};
  ASSERT_EQ(ejit_representative_copy_group_profile(2, true, &ThirdScalar, sizeof(ThirdScalar), &ThirdSize), EJIT_OK);
  EXPECT_EQ(ThirdScalar.total, 64u);
  EXPECT_EQ(ThirdScalar.topCount, 48u);
  EXPECT_EQ(ThirdScalar.topValue, 21u);
#endif

  // A new logical lifecycle of an existing member must reacquire a waiter and
  // compare final IR/bindings again, then reuse the still-owned physical T2.
  ASSERT_EQ(ejit_deactivate("cell", 0), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 0), EJIT_OK);
  void *Renewed = nullptr;
  for (unsigned A = 0; A < 4000 && !Renewed; ++A) {
    void *F = nullptr; uint32_t B = 0;
    auto S = entryCall(0, 1, &F, &B);
    if (S == EJIT_OK && F) {
      Renewed = F;
      executeAndCheck(F, 0, 1, 8);
    }
    releaseIfHeld(S, B);
    if (!Renewed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(Renewed, T2Fn);
  ejit_representative_stats_t RenewedStats{};
  ASSERT_EQ(ejit_representative_get_stats(&RenewedStats), EJIT_OK);
  EXPECT_EQ(RenewedStats.physicalCodeObjects, 3u);
  EXPECT_EQ(RenewedStats.bundlePublications, 3u);
  EXPECT_EQ(RenewedStats.bundleDispatchCount, 64u);

  // A cold representative calls once. Owner maintenance cancels it without
  // requiring another dispatch from that cell; a different member re-elects.
  auto Sample = [&](uint32_t Cell, uint32_t X) -> void * {
    for (unsigned A = 0; A < 4000; ++A) {
      void *F = nullptr; uint32_t B = 0;
      auto S = entryCall(Cell, 1, &F, &B);
      if (S == EJIT_OK && F) executeAndCheck(F, Cell, 1, X);
      releaseIfHeld(S, B);
      if (S == EJIT_OK && F) return F;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return nullptr;
  };
  ASSERT_EQ(ejit_activate("cell", 10), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 11), EJIT_OK);
  ASSERT_NE(Sample(10, 31), nullptr);
  ejit_representative_stats_t ColdBefore{};
  ASSERT_EQ(ejit_representative_get_group_stats(3, &ColdBefore), EJIT_OK);
  EXPECT_EQ(ColdBefore.representativeDispatchCount, 1u);
  // Register the waiting peer while the default timeout is still generous.
  for (unsigned A = 0; A < 4000; ++A) {
    void *F = nullptr; uint32_t B = 0;
    auto S = entryCall(11, 1, &F, &B);
    EXPECT_EQ(F, nullptr);
    releaseIfHeld(S, B);
    ejit_representative_stats_t St{};
    ASSERT_EQ(ejit_representative_get_group_stats(3, &St), EJIT_OK);
    if (St.waitersJoined == ColdBefore.waitersJoined + 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(ejit_representative_test_timeout(100000000, 2), EJIT_OK);
  ejit_representative_stats_t Timed{};
  for (unsigned A = 0; A < 4000; ++A) {
    ASSERT_EQ(ejit_representative_get_group_stats(3, &Timed), EJIT_OK);
    if (Timed.representativeSamplingSessionId &&
        Timed.representativeSamplingSessionId != ColdBefore.representativeSamplingSessionId) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_NE(Timed.representativeSamplingSessionId, 0u);
  EXPECT_NE(Timed.representativeSamplingSessionId, ColdBefore.representativeSamplingSessionId);
  EXPECT_EQ(Timed.representativeDispatchCount, 0u); // election itself invents no sample
  ASSERT_EQ(ejit_representative_test_timeout(5000000000ULL, 2), EJIT_OK);
  void *ColdReplacement = nullptr;
  for (uint32_t I = 0; I < kQuota; ++I) {
    void *F = Sample(11, I < 48 ? 27 : 5);
    ASSERT_NE(F, nullptr);
    if (!ColdReplacement) ColdReplacement = F;
    EXPECT_EQ(F, ColdReplacement);
  }
  void *ColdT2 = Sample(11, 9);
  ASSERT_NE(ColdT2, nullptr);
  EXPECT_NE(ColdT2, ColdReplacement);
  EXPECT_EQ(Sample(10, 9), ColdT2);
  ASSERT_EQ(ejit_representative_get_group_stats(3, &Timed), EJIT_OK);
  EXPECT_EQ(Timed.bundleGeneration, 2u);
  EXPECT_EQ(Timed.bundleDispatchCount, 64u);
  EXPECT_NE(Timed.representativeSamplingSessionId, ColdBefore.representativeSamplingSessionId);
  size_t ColdSize = 0;
  ASSERT_EQ(ejit_representative_copy_group_profile(3, false, nullptr, 0, &ColdSize), EJIT_OK);
  std::string ColdProfile(ColdSize, '\0');
  ASSERT_EQ(ejit_representative_copy_group_profile(3, false, &ColdProfile[0], ColdSize, &ColdSize), EJIT_OK);
  auto ColdReader = IndexedInstrProfReader::create(MemoryBuffer::getMemBufferCopy(ColdProfile));
  ASSERT_TRUE(static_cast<bool>(ColdReader)) << toString(ColdReader.takeError());
  for (const auto &Rec : **ColdReader) {
    auto Counts = Rec.Counts;
    std::sort(Counts.begin(), Counts.end());
    EXPECT_EQ(Counts, (std::vector<uint64_t>{48,64,1312}));
  }

  // A zero-re-election budget terminates the cold group after its first round.
  // Subsequent requests stay on AOT and cannot reacquire a sampling admission.
  ASSERT_EQ(ejit_activate("cell", 12), EJIT_OK);
  ASSERT_NE(Sample(12, 31), nullptr);
  ASSERT_EQ(ejit_representative_test_timeout(20000000, 0), EJIT_OK);
  for (unsigned A = 0; A < 4000; ++A) {
    ASSERT_EQ(ejit_representative_get_group_stats(4, &Timed), EJIT_OK);
    if (!Timed.representativeSamplingSessionId) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(Timed.representativeSamplingSessionId, 0u);
  for (unsigned I = 0; I < 8; ++I) {
    void *F = nullptr; uint32_t B = 0;
    auto S = entryCall(12, 1, &F, &B);
    EXPECT_NE(S, EJIT_OK);
    EXPECT_EQ(F, nullptr);
    releaseIfHeld(S, B);
  }
  EXPECT_EQ(Pool->state()->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(Pool->pendingCount(), 0u);
  ASSERT_EQ(ejit_representative_test_timeout(5000000000ULL, 2), EJIT_OK);

  // Compiler-source fence: stop a real prefix read, deactivate its lifecycle,
  // and prove cancellation is not completion while the worker still borrows.
  ejit_borrow_fence_t Scope{};
  ASSERT_EQ(ejit_representative_deactivate_begin("cell", 0, &Scope), EJIT_OK);
  ASSERT_EQ(ejit_representative_borrow_status(&Scope), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 0), EJIT_OK);
  EXPECT_EQ(ejit_representative_borrow_status(&Scope), EJIT_ERR_INVALID_PARAM);
  ASSERT_EQ(ejit_representative_test_candidate_gate(1), 1u);
  void *GateFn = nullptr; uint32_t GateBucket = 0;
  const auto GateStatus = entryCall(0, 1, &GateFn, &GateBucket);
  EXPECT_EQ(GateFn, nullptr);
  releaseIfHeld(GateStatus, GateBucket);
  for (unsigned A = 0; A < 4000 && ejit_representative_test_candidate_gate(0) != 2; ++A)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(ejit_representative_test_candidate_gate(0), 2u);
  EXPECT_EQ(ejit_representative_deactivate_begin("cell", 0, &Scope), EJIT_OK);
  EXPECT_EQ(ejit_representative_borrow_status(&Scope), EJIT_PENDING);
  // Always release the worker gate before a fatal assertion/fixture shutdown.
  ejit_representative_test_candidate_gate(3);
  ejit_status_t Borrow = EJIT_PENDING;
  for (unsigned A = 0; A < 4000 && Borrow == EJIT_PENDING; ++A) {
    Borrow = ejit_representative_borrow_status(&Scope);
    if (Borrow == EJIT_PENDING) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(Borrow, EJIT_OK);
  EXPECT_EQ(ejit_representative_borrow_status(&Scope), EJIT_OK); // idempotent observation
  EXPECT_EQ(ejit_representative_deactivate_begin("missing", 0, &Scope), EJIT_ERR_INVALID_PARAM);
  // Source mutation happens only after the old compiler borrow is confirmed.
  RepresentativeRuntime::rows()[0].gain = 31;
  ASSERT_EQ(ejit_activate("cell", 0), EJIT_OK);
  void *ChangedT1 = nullptr;
  for (uint32_t I = 0; I < kQuota; ++I) {
    void *F = Sample(0, I < 48 ? 35 : 3);
    ASSERT_NE(F, nullptr);
    if (!ChangedT1) ChangedT1 = F;
    EXPECT_EQ(F, ChangedT1);
  }
  void *ChangedT2 = Sample(0, 9);
  ASSERT_NE(ChangedT2, nullptr);
  EXPECT_NE(ChangedT2, ChangedT1);
  EXPECT_NE(ChangedT2, T2Fn);
  EXPECT_EQ(Sample(1, 9), T2Fn); // unaffected old group remains executable
  size_t ChangedSize = 0;
  ASSERT_EQ(ejit_representative_copy_group_profile(5, false, nullptr, 0, &ChangedSize), EJIT_OK);
  std::string ChangedProfile(ChangedSize, '\0');
  ASSERT_EQ(ejit_representative_copy_group_profile(5, false, &ChangedProfile[0], ChangedSize, &ChangedSize), EJIT_OK);
  auto ChangedReader = IndexedInstrProfReader::create(MemoryBuffer::getMemBufferCopy(ChangedProfile));
  ASSERT_TRUE(static_cast<bool>(ChangedReader)) << toString(ChangedReader.takeError());
  for (const auto &Rec : **ChangedReader) {
    auto Counts = Rec.Counts;
    std::sort(Counts.begin(), Counts.end());
    EXPECT_EQ(Counts, (std::vector<uint64_t>{48,64,1664}));
  }

  // A member has no personal T1/profile to retain after final-transform failure.
  // Exhausting its bounded retries ends both the actual attempts and its lease.
  EJitSharedDiagnostics FinalBefore{}, FinalAfter{};
  Pool->getDiagnostics(FinalBefore);
  ASSERT_EQ(ejit_representative_test_fail_member_tier2(3), EJIT_OK);
  ASSERT_EQ(ejit_activate("cell", 13), EJIT_OK);
  void *FailedMember = nullptr; uint32_t FailedBucket = 0;
  const auto FailedStatus = entryCall(13, 1, &FailedMember, &FailedBucket);
  EXPECT_EQ(FailedMember, nullptr);
  releaseIfHeld(FailedStatus, FailedBucket);
  for (unsigned A = 0; A < 4000; ++A) {
    Pool->getDiagnostics(FinalAfter);
    if (FinalAfter.compileFailed == FinalBefore.compileFailed + 3 &&
        Pool->liveRequestAttemptCount() == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(FinalAfter.compileFailed, FinalBefore.compileFailed + 3);
  EXPECT_EQ(Pool->liveRequestAttemptCount(), 0u);
  for (unsigned I = 0; I < 5; ++I) {
    void *F = nullptr; uint32_t B = 0;
    const auto S = entryCall(13, 1, &F, &B);
    EXPECT_NE(S, EJIT_OK);
    EXPECT_EQ(F, nullptr);
    releaseIfHeld(S, B);
  }
  EXPECT_EQ(Pool->pendingCount(), 0u);
  ASSERT_EQ(ejit_representative_deactivate_begin("cell", 13, &Scope), EJIT_OK);
  EXPECT_EQ(ejit_representative_borrow_status(&Scope), EJIT_OK);

}

// R3 pressure and isolation acceptance: twenty independently registered
// entries each use six legal cells.  The first five cells of every entry share
// one real representative session and one physical Tier-2 object; entry zero's
// sixth cell has a different may_const binding and must compile independently.
// Sampling is driven by concurrent business callers while the production worker
// owns queue progress, freeze, profile publication and PGOUse.
TEST_F(EJitRepresentativeRuntimeTest,
       PressureTwentyEntriesSixCellsStayIsolatedAndShareOnlyEqualMembers) {
  ejit_representative_stats_t Before{};
  const auto BeforeStatus = ejit_representative_get_stats(&Before);
  ASSERT_TRUE(BeforeStatus == EJIT_OK || BeforeStatus == EJIT_ERR_NOT_ACTIVE);
  if (BeforeStatus == EJIT_ERR_NOT_ACTIVE || !Before.active) {
    ejit_config_t Cfg{};
    Cfg.compileMode = EJIT_COMPILE_ASYNC;
    ASSERT_EQ(ejit_init_representative(&Cfg), EJIT_OK);
    ASSERT_EQ(ejit_representative_get_stats(&Before), EJIT_OK);
  }
  ASSERT_EQ(Before.active, 1u);
  auto *Pool =
      static_cast<EJitSharedTaskPool *>(ejit_representative_test_pool());
  ASSERT_NE(Pool, nullptr);

  for (uint32_t Cell = 0; Cell < kPressureCells; ++Cell)
    ASSERT_EQ(ejit_activate("cell", Cell), EJIT_OK);
  ASSERT_EQ(ejit_activate("trp", 1), EJIT_OK);
  // Twenty entries are intentionally driven while earlier groups finish their
  // Tier-2 publication.  Keep the cold-representative maintenance window well
  // above this bounded host pressure run; this changes only the test knob.
  ASSERT_EQ(ejit_representative_test_timeout(5000000000ULL, 2), EJIT_OK);

  const auto &Names = RepresentativeRuntime::pressureEntryNames();
  const auto &FuncIndices = RepresentativeRuntime::pressureFuncIndices();
  auto &Rows = RepresentativeRuntime::pressureRows();
  ASSERT_EQ(Names.size(), kPressureEntries);
  ASSERT_EQ(FuncIndices.size(), kPressureEntries);
  for (uint32_t Entry = 0; Entry < kPressureEntries; ++Entry)
    ASSERT_LT(FuncIndices[Entry], EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX)
        << Names[Entry];
  for (uint32_t I = 0; I < kPressureEntries; ++I)
    for (uint32_t J = I + 1; J < kPressureEntries; ++J)
      ASSERT_NE(FuncIndices[I], FuncIndices[J]);

  std::array<void *, kPressureEntries> T1{};
  std::atomic<bool> WorkersOk{true};
  std::mutex ErrorMutex;
  std::vector<std::string> WorkerErrors;
  auto recordError = [&](const std::string &Message) {
    WorkersOk.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> Lock(ErrorMutex);
    WorkerErrors.push_back(Message);
  };
  auto executeNoAssert = [](CellRow *EntryRows, void *Fn, uint32_t Cell,
                            uint32_t Trp, uint32_t X) {
    auto &Row = EntryRows[Cell];
    const uint32_t BeforeLive = Row.live[Trp];
    uint32_t Expected =
        BeforeLive + (X > Row.gain ? X * Row.gain : X + Row.gain) +
        X * (X - 1) / 2;
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    Expected += X * 5 + 10;
#endif
    using Entry = uint32_t (*)(uint64_t, uint64_t, uint32_t);
    const uint32_t Actual = reinterpret_cast<Entry>(Fn)(Cell, Trp, X);
    return Actual == Expected + Cell && Row.live[Trp] == Expected;
  };
  std::vector<std::thread> Samplers;
  Samplers.reserve(kPressureEntries);

  // Establish each representative in deterministic entry order.  Member
  // calls are issued after all representative sessions have drained their
  // quotas; they remain AOT until the bundle is published and never receive a
  // private Tier-1 admission.
  for (uint32_t Entry = 0; Entry < kPressureEntries; ++Entry) {
    for (unsigned Attempt = 0; Attempt < 4000 && !T1[Entry]; ++Attempt) {
      void *Fn = nullptr;
      uint32_t Bucket = 0;
      const auto S = entryCallFor(FuncIndices[Entry], 0, 1, &Fn, &Bucket);
      if (S == EJIT_OK && Fn) {
        T1[Entry] = Fn;
        executeAndCheckRows(&Rows[Entry * kCells], Fn, 0, 1,
                            7u + Entry % 3u);
      }
      releaseIfHeld(S, Bucket);
      if (!T1[Entry])
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!T1[Entry]) {
      recordError("entry " + std::to_string(Entry) +
                  " representative did not resolve");
      break;
    }
    Samplers.emplace_back([&, Entry] {
      // The setup call above is sample zero; complete the remaining quota here.
      for (uint32_t Sample = 1; Sample < kQuota; ++Sample) {
        bool Executed = false;
        for (unsigned Attempt = 0; Attempt < 4000 && !Executed; ++Attempt) {
          void *Fn = nullptr;
          uint32_t Bucket = 0;
          const auto S = entryCallFor(FuncIndices[Entry], 0, 1, &Fn, &Bucket);
          if (S == EJIT_OK && Fn) {
            if (Fn != T1[Entry])
              recordError("entry " + std::to_string(Entry) +
                          " changed Tier-1 pointer before quota close");
            const uint32_t X = Sample < 48 ? (7u + Entry % 3u) : 3u;
            if (!executeNoAssert(&Rows[Entry * kCells], Fn, 0, 1, X))
              recordError("entry " + std::to_string(Entry) +
                          " representative result/live-store mismatch");
            Executed = true;
          }
          releaseIfHeld(S, Bucket);
          if (!Executed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!Executed)
          recordError("entry " + std::to_string(Entry) +
                      " representative sample did not resolve");
      }
    });
  }
  for (auto &Thread : Samplers)
    Thread.join();
  ASSERT_TRUE(WorkersOk.load(std::memory_order_acquire))
      << (WorkerErrors.empty() ? "worker failure" : WorkerErrors.front());

  std::array<void *, kPressureEntries> SharedT2{};
  for (uint32_t Entry = 0; Entry < kPressureEntries; ++Entry) {
    for (uint32_t Cell = 0; Cell < (Entry == 0 ? 5u : kPressureCells);
         ++Cell) {
      bool Resolved = false;
      for (unsigned Attempt = 0; Attempt < 4000 && !Resolved; ++Attempt) {
        void *Fn = nullptr;
        uint32_t Bucket = 0;
        const auto S = entryCallFor(FuncIndices[Entry], Cell, 1, &Fn, &Bucket);
        if (S == EJIT_OK && Fn && Fn != T1[Entry]) {
          if (!SharedT2[Entry])
            SharedT2[Entry] = Fn;
          EXPECT_EQ(Fn, SharedT2[Entry])
              << "equal member entry " << Entry << " cell " << Cell;
          const uint32_t X = 9u + (Entry % 4u);
          EXPECT_TRUE(executeNoAssert(&Rows[Entry * kCells], Fn, Cell, 1, X));
          Resolved = true;
        }
        releaseIfHeld(S, Bucket);
        if (!Resolved)
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      ASSERT_TRUE(Resolved)
          << "shared Tier-2 did not resolve entry " << Entry << " cell "
          << Cell;
    }
    ASSERT_NE(SharedT2[Entry], nullptr) << Names[Entry];
  }

  // Unequal effective binding: entry zero's sixth cell belongs to a distinct
  // candidate group and therefore cannot reuse entry zero's physical code.
  void *UnequalT1 = nullptr;
  for (unsigned Attempt = 0; Attempt < 4000 && !UnequalT1; ++Attempt) {
    void *Fn = nullptr;
    uint32_t Bucket = 0;
    const auto S = entryCallFor(FuncIndices[0], 5, 1, &Fn, &Bucket);
    if (S == EJIT_OK && Fn) {
      UnequalT1 = Fn;
      executeAndCheckRows(&Rows[0], Fn, 5, 1, 101);
    }
    releaseIfHeld(S, Bucket);
    if (!UnequalT1)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_NE(UnequalT1, nullptr);
  EXPECT_NE(UnequalT1, SharedT2[0]);
  for (uint32_t Sample = 1; Sample < kQuota; ++Sample) {
    bool Executed = false;
    for (unsigned Attempt = 0; Attempt < 4000 && !Executed; ++Attempt) {
      void *Fn = nullptr;
      uint32_t Bucket = 0;
      const auto S = entryCallFor(FuncIndices[0], 5, 1, &Fn, &Bucket);
      if (S == EJIT_OK && Fn) {
        EXPECT_EQ(Fn, UnequalT1);
        executeAndCheckRows(&Rows[0], Fn, 5, 1, Sample < 48 ? 101 : 3);
        Executed = true;
      }
      releaseIfHeld(S, Bucket);
      if (!Executed)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(Executed) << "unequal member sample " << Sample;
  }
  void *UnequalT2 = nullptr;
  for (unsigned Attempt = 0; Attempt < 4000 && !UnequalT2; ++Attempt) {
    void *Fn = nullptr;
    uint32_t Bucket = 0;
    const auto S = entryCallFor(FuncIndices[0], 5, 1, &Fn, &Bucket);
    if (S == EJIT_OK && Fn && Fn != UnequalT1) {
      UnequalT2 = Fn;
      executeAndCheckRows(&Rows[0], Fn, 5, 1, 9);
    }
    releaseIfHeld(S, Bucket);
    if (!UnequalT2)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_NE(UnequalT2, nullptr);
  EXPECT_NE(UnequalT2, SharedT2[0]);
  EXPECT_NE(UnequalT2, UnequalT1);

  ejit_representative_stats_t After{};
  ASSERT_EQ(ejit_representative_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.groups - Before.groups, 21u);
  EXPECT_EQ(After.representativesElected - Before.representativesElected, 21u);
  EXPECT_EQ(After.representativeDispatches - Before.representativeDispatches,
            21u * kQuota);
  EXPECT_EQ(After.bundlePublications - Before.bundlePublications, 21u);
  EXPECT_EQ(After.physicalCodeObjects - Before.physicalCodeObjects, 21u);
  EXPECT_EQ(After.waitersJoined - Before.waitersJoined, 99u);
  EXPECT_EQ(After.sharedPhysicalReuses - Before.sharedPhysicalReuses, 99u);
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  EXPECT_EQ(After.completeProfiles - Before.completeProfiles, 21u);
#else
  EXPECT_EQ(After.edgeOnlyProfiles - Before.edgeOnlyProfiles, 21u);
#endif
  EXPECT_EQ(Pool->liveRequestAttemptCount(), 0u);
  EXPECT_EQ(Pool->pendingCount(), 0u);
}

// Board-shaped scheduling: one producer continuously services all identities,
// including already-admitted representatives, while later entries defer.
TEST_F(EJitRepresentativeRuntimeTest,
       RoundRobinTwentyEntriesRejoinSharedTier2AfterBorrowFence) {
  ejit_representative_stats_t Before{};
  if (ejit_representative_get_stats(&Before) != EJIT_OK || !Before.active) {
    ejit_config_t Cfg{};
    Cfg.compileMode = EJIT_COMPILE_ASYNC;
    ASSERT_EQ(ejit_init_representative(&Cfg), EJIT_OK);
  }
  ASSERT_EQ(ejit_representative_test_timeout(5000000000ULL, 2), EJIT_OK);
  auto *Pool = static_cast<EJitSharedTaskPool *>(ejit_representative_test_pool());
  ASSERT_NE(Pool, nullptr);
  auto &Rows = RepresentativeRuntime::pressureRows();
  const auto &Indices = RepresentativeRuntime::pressureFuncIndices();
  auto DeactivateAndDrain = [](uint32_t Cell) {
    ejit_borrow_fence_t Fence{};
    if (ejit_representative_deactivate_begin("cell", Cell, &Fence) != EJIT_OK)
      return false;
    for (unsigned I = 0; I != 4000; ++I) {
      auto S = ejit_representative_borrow_status(&Fence);
      if (S == EJIT_OK) return true;
      if (S != EJIT_PENDING) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  for (uint32_t Cell = 0; Cell < kPressureCells; ++Cell) {
    ASSERT_TRUE(DeactivateAndDrain(Cell));
    for (uint32_t Entry = 0; Entry < kPressureEntries; ++Entry)
      Rows[Entry * kCells + Cell].gain =
          Entry == 0 && Cell == 5 ? 201u : 40u + Entry;
    ASSERT_EQ(ejit_activate("cell", Cell), EJIT_OK);
  }
  ASSERT_EQ(ejit_activate("trp", 1), EJIT_OK);
  ASSERT_EQ(ejit_representative_get_stats(&Before), EJIT_OK);
  using Matrix = std::array<std::array<void *, kPressureCells>, kPressureEntries>;
  Matrix Initial{}, Updated{};
  void *UnequalT1 = nullptr;
  auto Drive = [&](Matrix &Pointers, bool Rejoined) {
    unsigned Stable = 0;
    for (unsigned Round = 0; Round != 8000; ++Round) {
      bool Complete = true;
      for (uint32_t Entry = 0; Entry < kPressureEntries; ++Entry) {
        for (uint32_t Cell = 0; Cell < kPressureCells; ++Cell) {
          void *Fn = nullptr;
          uint32_t Bucket = 0;
          const auto S = entryCallFor(Indices[Entry], Cell, 1, &Fn, &Bucket);
          // Never take a T1 admission just to inspect the pointer: execute
          // every granted function before releasing its exact read token.
          if (S == EJIT_OK && Fn)
            executeAndCheckRows(&Rows[Entry * kCells], Fn, Cell, 1,
                                Round % 63u + 1u);
          releaseIfHeld(S, Bucket);
          if (S != EJIT_OK && S != EJIT_PENDING && S != EJIT_ERR_QUEUE_FULL)
            return false;
          if (S == EJIT_OK && !Fn) return false;
          Complete &= S == EJIT_OK;
          Pointers[Entry][Cell] = Fn;
          if (!Rejoined && Entry == 0 && Cell == 5 && Fn && !UnequalT1)
            UnequalT1 = Fn;
        }
        for (uint32_t Cell = 1; Cell < kPressureCells; ++Cell) {
          if (!Rejoined && Entry == 0 && Cell == 5)
            Complete &= Pointers[Entry][Cell] &&
                        Pointers[Entry][Cell] != Pointers[Entry][0] &&
                        Pointers[Entry][Cell] != UnequalT1;
          else
            Complete &= Pointers[Entry][Cell] == Pointers[Entry][0];
        }
        if (Rejoined)
          for (uint32_t Cell = 0; Cell < kPressureCells; ++Cell)
            Complete &= Pointers[Entry][Cell] == Initial[Entry][0];
      }
      Complete &= Pool->pendingCount() == 0 && Pool->liveRequestAttemptCount() == 0;
      Stable = Complete ? Stable + 1 : 0;
      if (Stable == 4) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  ASSERT_TRUE(Drive(Initial, false));
  ejit_representative_stats_t After{};
  ASSERT_EQ(ejit_representative_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.representativesElected - Before.representativesElected, 21u);
  EXPECT_EQ(After.representativeDispatches - Before.representativeDispatches,
            21u * kQuota);
  EXPECT_EQ(After.bundlePublications - Before.bundlePublications, 21u);
  EXPECT_EQ(After.physicalCodeObjects - Before.physicalCodeObjects, 21u);
  EXPECT_EQ(After.sharedPhysicalReuses - Before.sharedPhysicalReuses, 99u);
  ASSERT_TRUE(DeactivateAndDrain(5));
  Rows[5].gain = 40;
  ASSERT_EQ(ejit_activate("cell", 5), EJIT_OK);
  ASSERT_TRUE(Drive(Updated, true));
  ejit_representative_stats_t Final{};
  ASSERT_EQ(ejit_representative_get_stats(&Final), EJIT_OK);
  EXPECT_EQ(Final.physicalCodeObjects, After.physicalCodeObjects);
  EXPECT_EQ(Final.representativeDispatches, After.representativeDispatches);
  EXPECT_EQ(Final.sharedPhysicalReuses - After.sharedPhysicalReuses, 20u);
  EXPECT_EQ(Pool->pendingCount(), 0u);
  EXPECT_EQ(Pool->liveRequestAttemptCount(), 0u);
}

/// The default-off policy on the REAL runtime: an ordinary initialization never
/// creates a representative group (no opt-in, no group, no publication).
TEST_F(EJitRepresentativeRuntimeTest,
       PlainInitKeepsRepresentativeSharingOff) {
  ejit_shutdown();
  ejit_config_t Cfg{};
  Cfg.compileMode = EJIT_COMPILE_ASYNC;
  ASSERT_EQ(ejit_init(&Cfg), EJIT_OK);
  ejit_representative_stats_t St{};
  ASSERT_EQ(ejit_representative_get_stats(&St), EJIT_OK);
  EXPECT_EQ(St.active, 0u);
  EXPECT_EQ(St.groups, 0u);
  EXPECT_EQ(St.bundlePublications, 0u);
  EXPECT_EQ(ejit_taskpool_pending_count(), 0u);
  ejit_shutdown();

}

} // namespace
