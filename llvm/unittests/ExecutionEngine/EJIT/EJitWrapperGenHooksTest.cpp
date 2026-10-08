//===-- EJitWrapperGenHooksTest.cpp - PR231 wrapper-hook evidence ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The AOT wrapper's small-table hooks (`ejit_stab_wrapper_enter` / `ejit_stab_leave`)
// are the ONLY place a call site reaches admitted specialized code, so they are
// tested two ways, both against code the PASS actually generated:
//
//   Part A - generated-IR regressions. The pass is run in-process (this
//   checkout has no opt/FileCheck and no lit, so a `-passes=` command line
//   cannot be used here) over a module shaped like the lit fixtures, and the
//   emitted IR is asserted for the three dispatch paths x hooks ON/OFF:
//     1. the compile_or_get-success dispatch (`-ejit-inline-cache` OFF);
//     2. the inline-cache hit path (probe + cell);
//     3. the frame-less sentinel form, which exists ONLY with the hooks OFF
//        (with the hooks ON the hit path must be framed, because a `leave`
//        before a musttail tail jump would release the protected read before
//        the code it protects runs).
//   Cross-cutting: `verifyModule` must accept every shape. Common admission
//   occurs BEFORE ordinary resolution and takes no generic bucket read token;
//   no-policy paths preserve resolution under the policy-epoch guards.
//
//   Part B - a real generated wrapper is executed. The wrapped module is
//   JIT-compiled with LLJIT and its entry is called with real arguments; a real
//   EJitSmallTableHost is registered through the ordinary request path, driven
//   through common-T1 completion/freeze, T2 publication, a configuration move
//   and generation retirement. The wrapper's own call decides between the
//   published specialized entry and its AOT body, so the assertion values are
//   what distinguish the paths.
//
// Every executed-wrapper fixture starts a real Async/PGO runtime without warmup.
// Part D separately preserves a genuine ordinary-PGO warmup control. Common Host
// requests and publication are joined real owner-worker operations; the cold
// cases assert those operations independently of ordinary compile statistics.
// The runtime C ABI symbols the wrapper calls are
// the process's implementations, wired into the JIT like the product image's
// symbol table; no logical small-table slot is published by the test. All these
// are host checks: the readiness facts are the explicit test adapter, not a
// product readiness transaction or evidence of board/cache coherence.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableHost.h"
#include "EJitSharedTaskPoolTestAccess.h"
#include "../../../lib/ExecutionEngine/EJIT/EJitOwnerWorkerContext.h"
#include "../../../lib/ExecutionEngine/EJIT/EJitWrapperRuntimeTestAccess.h"
#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <chrono>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <atomic>
#include <cstdio>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace llvm;
using namespace llvm::ejit;

// The pass options the tests pin directly. All are non-static for exactly this
// reason: no opt/lit is available in this checkout, so the shapes must be
// selected in-process.
extern cl::opt<bool> EnableEJitSmallTableHooks;
extern cl::opt<bool> EJitInlineCache;
extern cl::opt<bool> EnableEJitGlobalCtors;

extern "C" {
void ejit_register_period_array(const char *, const char *, void *, uint64_t);
void ejit_register_static_var(const char *, void *);
void ejit_register_bitcode(const char *, const uint8_t *, uint64_t);
}

namespace {

std::function<void(unsigned)> gNoPolicyCheckObserver;
unsigned gNoPolicyCheckCalls = 0;
bool observedNoPolicyCurrent(uint64_t Epoch) {
  const unsigned Call = ++gNoPolicyCheckCalls;
  if (gNoPolicyCheckObserver)
    gNoPolicyCheckObserver(Call);
  // The delegate only makes a deterministic scheduling window. The actual
  // production epoch decision, resolver, code, counters and leases are unchanged.
  return ejit_stab_wrapper_no_policy_current(Epoch);
}

// LLJIT needs the native target registered once per process, including the
// assembler printer/parser: Part B emits and links real objects for this host.
static const bool kTargetsInitialized = [] {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
  return true;
}();

//===----------------------------------------------------------------------===//
// Shared pass-run harness
//===----------------------------------------------------------------------===//

std::string irToString(Module &M) {
  std::string S;
  raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return OS.str();
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

std::unique_ptr<Module> parseModule(LLVMContext &Ctx, StringRef Text,
                                    const char *Who) {
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(Text, Err, Ctx);
  if (!M)
    Err.print(Who, errs());
  return M;
}

/// Run the generated wrapper over \p M and return the printed IR. The caller
/// owns the option state (EnableEJitSmallTableHooks / EJitInlineCache).
std::string runWrapperGen(Module &M, Analyses &A) {
  ModulePassManager MPM;
  MPM.addPass(EJitWrapperGenPass());
  MPM.run(M, A.MAM);
  return irToString(M);
}

bool contains(StringRef Hay, StringRef Needle) { return Hay.contains(Needle); }

/// Number of occurrences of \p Needle in \p Hay.
unsigned countOccurrences(StringRef Hay, StringRef Needle) {
  unsigned N = 0;
  size_t Pos = 0;
  while ((Pos = Hay.find(Needle, Pos)) != StringRef::npos) {
    ++N;
    Pos += Needle.size();
  }
  return N;
}

/// The `target datalayout` / `target triple` header (mirrors the sibling host
/// suite: LLJIT rejects a module whose data layout differs from the JIT's).
std::string hostTargetHeader() {
  static const std::string Header = [] {
    InitializeNativeTarget();
    const std::string Triple = sys::getDefaultTargetTriple();
    const char *Fallback =
        "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:"
        "32:64-S128";
    std::string DL = Fallback;
    auto JTMB = orc::JITTargetMachineBuilder::detectHost();
    if (!JTMB) {
      consumeError(JTMB.takeError());
    } else if (auto HostDL = JTMB->getDefaultDataLayoutForTarget()) {
      DL = HostDL->getStringRepresentation();
    } else {
      consumeError(HostDL.takeError());
    }
    return "    target datalayout = \"" + DL + "\"\n    target triple = \"" +
           Triple + "\"\n";
  }();
  return Header;
}

//===----------------------------------------------------------------------===//
// The fixture entry (shared by both parts)
//===----------------------------------------------------------------------===//

constexpr unsigned kWrapCells = 4;
constexpr unsigned kWrapReadyCells = 3;
constexpr const char *kWrapPeriod = "tenant_cell";

/// mode: identical on the whole domain -> uniform constant. bycell: differs per
/// cell only -> the cell axis is kept, every other axis is dropped.
struct alignas(4) WrapElement {
  int32_t mode;
  int32_t bycell;
};
static_assert(sizeof(WrapElement) == 8, "layout must match the IR type");

/// One-dimension entry with two may_const fields (offset 0 uniform, offset 4
/// per-cell). `Cells` is the declared extent of the dimension, so the width of
/// the declared index space is the fixture's own and never a guessed capacity.
/// \p Src / \p Out are the source array and output array global names, so the
/// same entry text serves both the AOT image and the registered bitcode.
std::string entryBodyText(StringRef Src, StringRef Out, unsigned Cells) {
  const std::string C = Twine(Cells).str();
  std::string T = hostTargetHeader();
  T += "\n    %A = type { i32, i32 }\n";
  T += "    @" + Src.str() + " = external global [" + C + " x %A]\n";
  T += "    @" + Out.str() + " = external global [" + C + " x i32]\n";
  T += "\n    define i32 @f_entry(i32 %cell, i32 %x) !ejit.metadata !0 {\n";
  T += "    entry:\n";
  T += "      %row = getelementptr inbounds [" + C + " x %A], ptr @" +
       Src.str() + ", i64 0, i32 %cell\n";
  T += "      %p0 = getelementptr inbounds %A, ptr %row, i32 0, i32 0\n";
  T += "      %mode = load i32, ptr %p0, align 4, !ejit.may_const !3\n";
  T += "      %p1 = getelementptr inbounds %A, ptr %row, i32 0, i32 1\n";
  T += "      %bycell = load i32, ptr %p1, align 4, !ejit.may_const !3\n";
  T += "      %op = getelementptr inbounds [" + C + " x i32], ptr @" +
       Out.str() + ", i64 0, i32 %cell\n";
  T += "      store i32 %x, ptr %op, align 4\n";
  T += "      %m1 = mul i32 %mode, 1000\n";
  T += "      %m2 = mul i32 %bycell, 100\n";
  T += "      %s1 = add i32 %m1, %m2\n";
  T += "      %xm = mul i32 %x, 3\n";
  T += "      %sum = add i32 %s1, %xm\n";
  T += "      ret i32 %sum\n";
  T += "    }\n\n";
  T += "    !0 = !{!1, !2}\n";
  T += "    !1 = !{!\"ejit_entry\"}\n";
  T += "    !2 = !{!\"ejit_period_arr_ind\", !\"" + std::string(kWrapPeriod) +
       "\", i32 0}\n";
  T += "    !3 = !{}\n";
  return T;
}

/// The same entry, but the per-cell (non-uniform, TABLE) load happens AFTER a
/// call to an external observation hook. An external call is a memory clobber,
/// so the annotated load below it can neither be hoisted above it nor folded
/// into a constant: when the hook runs, the specialized call is really INSIDE
/// code that is still going to read the table. That is what makes a paused
/// lifetime check physical instead of an admission token held without executing.
std::string reentrantEntryBodyText(StringRef Src, StringRef Out,
                                   unsigned Cells) {
  const std::string C = Twine(Cells).str();
  std::string T = hostTargetHeader();
  T += "\n    %A = type { i32, i32 }\n";
  T += "    @" + Src.str() + " = external global [" + C + " x %A]\n";
  T += "    @" + Out.str() + " = external global [" + C + " x i32]\n";
  T += "\n    define i32 @f_entry(i32 %cell, i32 %x) !ejit.metadata !0 {\n";
  T += "    entry:\n";
  T += "      %row = getelementptr inbounds [" + C + " x %A], ptr @" +
       Src.str() + ", i64 0, i32 %cell\n";
  T += "      %p0 = getelementptr inbounds %A, ptr %row, i32 0, i32 0\n";
  T += "      %mode = load i32, ptr %p0, align 4, !ejit.may_const !3\n";
  T += "      %obs = call i32 @wrap_observe(i32 %cell)\n";
  T += "      %p1 = getelementptr inbounds %A, ptr %row, i32 0, i32 1\n";
  T += "      %bycell = load i32, ptr %p1, align 4, !ejit.may_const !3\n";
  T += "      %op = getelementptr inbounds [" + C + " x i32], ptr @" +
       Out.str() + ", i64 0, i32 %cell\n";
  T += "      store i32 %x, ptr %op, align 4\n";
  T += "      %m1 = mul i32 %mode, 1000\n";
  T += "      %m2 = mul i32 %bycell, 100\n";
  T += "      %s1 = add i32 %m1, %m2\n";
  T += "      %xm = mul i32 %x, 3\n";
  T += "      %sum = add i32 %s1, %xm\n";
  T += "      %sum2 = add i32 %sum, %obs\n";
  T += "      ret i32 %sum2\n";
  T += "    }\n\n";
  T += "    !0 = !{!1, !2}\n";
  T += "    !1 = !{!\"ejit_entry\"}\n";
  T += "    !2 = !{!\"ejit_period_arr_ind\", !\"" + std::string(kWrapPeriod) +
       "\", i32 0}\n";
  T += "    !3 = !{}\n";
  // Declared AFTER the definitions/metadata: the AOT module text is assembled
  // from the `define` onwards, so this declaration must travel with it.
  T += "    declare i32 @wrap_observe(i32)\n";
  return T;
}

/// A single C translation unit may define both business and test control code.
/// Its volatile, mutable callback slot is the closure boundary: the initializer
/// is owned by the AOT image, not followed or cloned into registered bitcode.
std::string singleTuCallbackBodyText(StringRef Src, StringRef Out,
                                     unsigned Cells) {
  std::string T = reentrantEntryBodyText(Src, Out, Cells);
  const std::string Direct = "      %obs = call i32 @wrap_observe(i32 %cell)\n";
  const size_t Pos = T.find(Direct);
  if (Pos == std::string::npos)
    return {};
  T.replace(Pos, Direct.size(),
            "      %probe = load volatile ptr, ptr @g_pr231_probe_dispatch, align 8\n"
            "      call void %probe()\n"
            "      %obs = add i32 0, 0\n");
  T += R"(
    @g_pr231_probe_dispatch = global ptr @pr231_probe_inflight, section ".ejit_pr231_shared", align 8
    @g_pr231_observer_calls = internal global i32 0
    @g_pr231_controller_state = internal global i32 77
    define internal void @pr231_probe_inflight() noinline {
      %before = load i32, ptr @g_pr231_observer_calls, align 4
      %next = add i32 %before, 1
      store i32 %next, ptr @g_pr231_observer_calls, align 4
      %unused = call i32 @wrap_observe(i32 0)
      ret void
    }
    define i32 @pr231_observation_count() {
      %count = load i32, ptr @g_pr231_observer_calls, align 4
      ret i32 %count
    }
    define void @pr231_probe_alternate() noinline {
      %before = load i32, ptr @g_pr231_observer_calls, align 4
      %next = add i32 %before, 10
      store i32 %next, ptr @g_pr231_observer_calls, align 4
      ret void
    }
    define i32 @test_ejit_period() {
      %state = load i32, ptr @g_pr231_controller_state, align 4
      ret i32 %state
    }
  )";
  return T;
}

Expected<std::unique_ptr<Module>> extractedRegistrationPayload(Module &M,
                                                               LLVMContext &C) {
  auto *Embedded = M.getGlobalVariable(GV_EJIT_BITCODE, true);
  auto *Bytes = Embedded && Embedded->hasInitializer()
                    ? dyn_cast<ConstantDataSequential>(Embedded->getInitializer())
                    : nullptr;
  if (!Bytes)
    return make_error<StringError>("missing actual PASS1 payload",
                                   inconvertibleErrorCode());
  return parseBitcodeFile(MemoryBufferRef(Bytes->getRawDataValues(),
                                         "actual PASS1 single-TU payload"), C);
}

/// The re-entrant observation hook. When armed it runs the test's mutations
/// while the specialized call is on the stack; it returns 0 so the entry's own
/// result is unchanged.
std::function<void()> g_observe;
int32_t wrapObserve(int32_t) {
  if (g_observe)
    g_observe();
  return 0;
}

//===----------------------------------------------------------------------===//
// Part A: the generated-IR regressions
//===----------------------------------------------------------------------===//

class WrapperGenIRTest : public testing::Test {
protected:
  LLVMContext Ctx;
  bool SavedHooks = EnableEJitSmallTableHooks;
  bool SavedIcache = EJitInlineCache;

  void SetUp() override {
    EnableEJitSmallTableHooks = false;
    EJitInlineCache = false;
  }
  void TearDown() override {
    EnableEJitSmallTableHooks = SavedHooks;
    EJitInlineCache = SavedIcache;
  }

  /// Run the pass on a fresh copy of the regression module and return its IR.
  std::string wrap(bool Hooks, bool Icache) {
    EnableEJitSmallTableHooks = Hooks;
    EJitInlineCache = Icache;
    auto M = parseModule(Ctx, entryBodyText("g_src", "g_out", kWrapCells),
                         "WrapperGenIRTest");
    EXPECT_TRUE(M);
    if (!M)
      return {};
    // The input must be valid before the pass, so a failure below is the pass's.
    EXPECT_FALSE(verifyModule(*M, &errs()));
    Analyses A;
    std::string Out = runWrapperGen(*M, A);
    // ...and the pass output must be valid IR for both flag values.
    EXPECT_FALSE(verifyModule(*M, &errs()))
        << "generated IR is invalid (hooks=" << Hooks
        << " icache=" << Icache << ")";
    return Out;
  }
};

void checkSingleTuCallbackExtraction(StringRef TargetHeader) {
  LLVMContext Ctx;
  std::string Text = singleTuCallbackBodyText("g_src", "g_out", kWrapCells);
  const std::string NativeHeader = hostTargetHeader();
  ASSERT_EQ(Text.compare(0, NativeHeader.size(), NativeHeader), 0);
  Text.replace(0, NativeHeader.size(), TargetHeader.str());
  auto M = parseModule(Ctx, Text, "single-TU actual bitcode extraction");
  ASSERT_TRUE(M);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  const bool SavedCtors = EnableEJitGlobalCtors;
  auto Restore = make_scope_exit([&] { EnableEJitGlobalCtors = SavedCtors; });
  EnableEJitGlobalCtors = false;
  Analyses A;
  ModulePassManager PM;
  PM.addPass(EJitRegisterBitcodePass());
  PM.run(*M, A.MAM);
  ASSERT_FALSE(verifyModule(*M, &errs()));

  LLVMContext PayloadCtx;
  auto Payload = extractedRegistrationPayload(*M, PayloadCtx);
  ASSERT_TRUE(static_cast<bool>(Payload)) << toString(Payload.takeError());
  ASSERT_FALSE(verifyModule(**Payload, &errs()));
  SCOPED_TRACE("input DL=" + M->getDataLayoutStr() +
               " payload DL=" + (*Payload)->getDataLayoutStr());
  EXPECT_EQ((*Payload)->getTargetTriple(), M->getTargetTriple());
  EXPECT_EQ((*Payload)->getDataLayoutStr(), M->getDataLayoutStr());
  EXPECT_EQ((*Payload)->getDataLayout(), M->getDataLayout());
  auto *Slot = (*Payload)->getNamedGlobal("g_pr231_probe_dispatch");
  ASSERT_NE(Slot, nullptr);
  EXPECT_TRUE(Slot->isDeclaration());
  EXPECT_FALSE(Slot->isConstant());
  EXPECT_FALSE(Slot->hasLocalLinkage());
  EXPECT_EQ((*Payload)->getNamedGlobal("g_pr231_observer_calls"), nullptr);
  EXPECT_EQ((*Payload)->getNamedGlobal("g_pr231_controller_state"), nullptr);
  EXPECT_EQ((*Payload)->getFunction("pr231_probe_inflight"), nullptr);
  EXPECT_EQ((*Payload)->getFunction("pr231_observation_count"), nullptr);
  EXPECT_EQ((*Payload)->getFunction("pr231_probe_alternate"), nullptr);
  EXPECT_EQ((*Payload)->getFunction("test_ejit_period"), nullptr);
  unsigned VolatileLoads = 0, IndirectCalls = 0;
  for (Function &F : **Payload)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        if (auto *Load = dyn_cast<LoadInst>(&I))
          if (Load->getPointerOperand()->stripPointerCasts() == Slot) {
            EXPECT_TRUE(Load->isVolatile());
            ++VolatileLoads;
          }
        if (auto *Call = dyn_cast<CallBase>(&I))
          if (!Call->getCalledFunction()) {
            auto *Loaded = dyn_cast<LoadInst>(Call->getCalledOperand());
            ASSERT_NE(Loaded, nullptr);
            EXPECT_EQ(Loaded->getPointerOperand()->stripPointerCasts(), Slot);
            ++IndirectCalls;
          }
      }
  EXPECT_EQ(VolatileLoads, 1u);
  EXPECT_EQ(IndirectCalls, 1u);

  // The actual emitted registry contains the callback SLOT address, not its
  // initializer's current function address. Product registration resolves that
  // same mutable object after lipo/GC; no manual test registration supplies it.
  auto *Registry = M->getGlobalVariable(".ejit.registry.bitcode", true);
  ASSERT_NE(Registry, nullptr);
  auto *Records = dyn_cast<ConstantArray>(Registry->getInitializer());
  ASSERT_NE(Records, nullptr);
  unsigned SlotRecords = 0;
  for (Value *V : Records->operands()) {
    auto *Record = dyn_cast<ConstantStruct>(V);
    ASSERT_NE(Record, nullptr);
    if (Record->getOperand(3)->stripPointerCasts() ==
        M->getNamedGlobal("g_pr231_probe_dispatch")) {
      EXPECT_EQ(cast<ConstantInt>(Record->getOperand(0))->getZExtValue(),
                EJIT_REG_SYMBOL);
      ++SlotRecords;
    }
  }
  EXPECT_EQ(SlotRecords, 1u);
  auto *OriginalSlot = M->getNamedGlobal("g_pr231_probe_dispatch");
  ASSERT_TRUE(OriginalSlot->hasInitializer());
  EXPECT_EQ(OriginalSlot->getInitializer()->stripPointerCasts(),
            M->getFunction("pr231_probe_inflight"));
}

TEST_F(WrapperGenIRTest, SingleTuMutableVolatileCallbackIsolatesControllerNative) {
  checkSingleTuCallbackExtraction(hostTargetHeader());
}

TEST_F(WrapperGenIRTest, SingleTuMutableVolatileCallbackIsolatesControllerBigEndian) {
  // LLVM 21's real BE AArch64 ELF ABI, including ptr32/ptr64 address spaces
  // and 32-bit function-pointer alignment (AArch64TargetMachine.cpp). A legacy
  // truncated layout would be auto-upgraded by the bitcode reader and is not
  // the actual frontend/backend ABI this regression must preserve exactly.
  checkSingleTuCallbackExtraction(
      "target datalayout = \"E-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-"
      "i16:16:32-i64:64-i128:128-n32:64-S128-Fn32\"\n"
      "target triple = \"aarch64_be-none-elf\"\n");
}

// --- Path 1: the compile_or_get-success dispatch (-ejit-inline-cache OFF) ---

TEST_F(WrapperGenIRTest, PlainDispatchHooksOffIsTheBaselineShape) {
  const std::string IR = wrap(/*Hooks=*/false, /*Icache=*/false);
  ASSERT_FALSE(IR.empty());
  EXPECT_FALSE(contains(IR, "ejit_stab_"));
  EXPECT_FALSE(contains(IR, "ejit_stab_leave"));
  EXPECT_FALSE(contains(IR, "@__ejit_icache_fn_f_entry"));
  // The baseline dispatch: resolve, call the resolved pointer, release the read.
  EXPECT_EQ(countOccurrences(IR, "call i32 @ejit_taskpool_compile_or_get_1d"),
            1u);
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_taskpool_release_read"), 1u);
}

TEST_F(WrapperGenIRTest, PlainDispatchHooksOnGatesBeforeOrdinaryResolve) {
  const std::string IR = wrap(/*Hooks=*/true, /*Icache=*/false);
  ASSERT_FALSE(IR.empty());
  ASSERT_TRUE(contains(IR, "declare ptr @ejit_stab_wrapper_enter")) << IR;
  ASSERT_TRUE(contains(IR, "declare void @ejit_stab_leave")) << IR;

  // The common gate precedes ordinary resolution: admitted takes its own
  // protected common call; owned refusal goes AOT; only no-policy may resolve.
  EXPECT_TRUE(contains(IR, "jit_stab_decide"));
  EXPECT_TRUE(contains(IR, "jit_common_call"));
  EXPECT_TRUE(contains(IR, "jit_plain_resolve"));
  EXPECT_EQ(countOccurrences(IR, "call ptr @ejit_stab_wrapper_enter"), 1u);
  EXPECT_FALSE(contains(IR, "call ptr @ejit_stab_enter("));
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_stab_leave"), 2u);
  // The reason load decides refused vs no-policy.
  EXPECT_TRUE(contains(IR, "ejit_stab_why_v"));
  // A no-policy epoch must still be current before lookup AND generic dispatch.
  EXPECT_GE(countOccurrences(IR, "call i1 @ejit_stab_wrapper_no_policy_current"),
            2u);
  EXPECT_EQ(countOccurrences(IR, "call i32 @ejit_taskpool_compile_or_get_1d"),
            1u);
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_taskpool_release_read"), 2u);

  auto M = parseModule(Ctx, IR, "WrapperGenIRTest.early");
  ASSERT_NE(M, nullptr);
  auto *F = M->getFunction("f_entry");
  ASSERT_NE(F, nullptr);
  CallBase *Enter = nullptr, *Resolve = nullptr;
  unsigned CommonCalls = 0, CommonLeaves = 0;
  for (auto &BB : *F)
    for (auto &I : BB)
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        Function *Callee = Call->getCalledFunction();
        if (Callee && Callee->getName() == "ejit_stab_wrapper_enter") {
          Enter = Call;
          EXPECT_EQ(Call->arg_size(), 8u);
        }
        if (Callee && Callee->getName() == "ejit_taskpool_compile_or_get_1d")
          Resolve = Call;
        if (BB.getName() == "jit_common_call") {
          if (!Callee)
            ++CommonCalls;
          else if (Callee->getName() == "ejit_stab_leave")
            ++CommonLeaves;
          else
            EXPECT_FALSE(Callee->getName().starts_with("ejit_taskpool_"))
                << "common calls must not acquire/release a generic bucket";
        }
      }
  ASSERT_NE(Enter, nullptr);
  ASSERT_NE(Resolve, nullptr);
  DominatorTree DT(*F);
  EXPECT_TRUE(DT.dominates(Enter, Resolve));
  EXPECT_EQ(CommonCalls, 1u);
  EXPECT_EQ(CommonLeaves, 1u);
}

// --- Path 2: the inline-cache hit path ------------------------------------

TEST_F(WrapperGenIRTest, IcacheHitIsFramelessOnlyWithoutHooks) {
  const std::string IR = wrap(/*Hooks=*/false, /*Icache=*/true);
  ASSERT_FALSE(IR.empty());
  EXPECT_FALSE(contains(IR, "ejit_stab_"));
  // Sentinel-formed 1D table: the wrapper is the probe plus one musttail BLR on
  // the loaded cell and has no guarded dispatch block at all.
  EXPECT_TRUE(contains(IR, "musttail call i32 %ejit_ic_fn")) << IR;
  EXPECT_FALSE(contains(IR, "jit_icache_dispatch"));
  EXPECT_FALSE(contains(IR, "jit_miss"));
}

TEST_F(WrapperGenIRTest, IcacheHitWithHooksIsFramedAndPaired) {
  const std::string IR = wrap(/*Hooks=*/true, /*Icache=*/true);
  ASSERT_FALSE(IR.empty());
  // Common policy is checked before even loading the ordinary cache. The
  // frame-less hit form is gone so admitted common calls can leave after return.
  // The no-policy miss tail call into MissFn remains a separate ordinary path.
  EXPECT_FALSE(contains(IR, "musttail call i32 %ejit_ic_fn")) << IR;
  EXPECT_TRUE(contains(IR, "call i32 %ejit_ic_fn(")) << IR;
  EXPECT_TRUE(contains(IR, "musttail call i32 @f_entry_miss")) << IR;
  EXPECT_TRUE(contains(IR, "jit_icache_dispatch"));
  EXPECT_TRUE(contains(IR, "jit_miss"));
  EXPECT_TRUE(contains(IR, "jit_common_hit_call"));
  EXPECT_TRUE(contains(IR, "jit_common_aot"));
  EXPECT_TRUE(contains(IR, "jit_plain_probe"));
  EXPECT_TRUE(contains(IR, "jit_plain_hit_call"));
  // One gate in the cache wrapper and one in the separate miss wrapper. Owned
  // refusal calls the untouched AOT body directly, never re-resolves via MissFn.
  EXPECT_EQ(countOccurrences(IR, "call ptr @ejit_stab_wrapper_enter"), 2u);
  EXPECT_FALSE(contains(IR, "call ptr @ejit_stab_enter("));
  // Both wrappers have common + ordinary-success paired leave sites.
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_stab_leave"), 4u);
  EXPECT_EQ(countOccurrences(IR, "call i32 @ejit_taskpool_compile_or_get_1d"),
            1u);
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_taskpool_release_read"), 2u);

  auto M = parseModule(Ctx, IR, "WrapperGenIRTest.early_icache");
  ASSERT_NE(M, nullptr);
  auto *F = M->getFunction("f_entry");
  ASSERT_NE(F, nullptr);
  CallBase *Enter = nullptr;
  LoadInst *CacheLoad = nullptr;
  unsigned CommonCalls = 0, CommonLeaves = 0, RefusedAotCalls = 0;
  for (auto &BB : *F)
    for (auto &I : BB) {
      if (auto *Load = dyn_cast<LoadInst>(&I))
        if (Load->getName() == "ejit_ic_fn")
          CacheLoad = Load;
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        Function *Callee = Call->getCalledFunction();
        if (Callee && Callee->getName() == "ejit_stab_wrapper_enter") {
          Enter = Call;
          EXPECT_EQ(Call->arg_size(), 8u);
        }
        if (BB.getName() == "jit_common_hit_call") {
          if (!Callee)
            ++CommonCalls;
          else if (Callee->getName() == "ejit_stab_leave")
            ++CommonLeaves;
          else
            EXPECT_FALSE(Callee->getName().starts_with("ejit_taskpool_"));
        }
        if (BB.getName() == "jit_common_aot") {
          ASSERT_NE(Callee, nullptr);
          EXPECT_EQ(Callee->getName(), "f_entry_smalltable_aot");
          ++RefusedAotCalls;
        }
      }
    }
  ASSERT_NE(Enter, nullptr);
  ASSERT_NE(CacheLoad, nullptr);
  DominatorTree DT(*F);
  EXPECT_TRUE(DT.dominates(Enter, CacheLoad));
  EXPECT_EQ(CommonCalls, 1u);
  EXPECT_EQ(CommonLeaves, 1u);
  EXPECT_EQ(RefusedAotCalls, 1u);
}

TEST_F(WrapperGenIRTest, HooksDoNotChangeTheUnrelatedABI) {
  // Raw arguments and the return value are the entry's own in both shapes: the
  // wrapper keeps F's signature, so a caller's ABI is untouched by the flag.
  const std::string Off = wrap(/*Hooks=*/false, /*Icache=*/false);
  const std::string On = wrap(/*Hooks=*/true, /*Icache=*/false);
  const char *Sig = "define i32 @f_entry(i32 %cell, i32 %x)";
  EXPECT_TRUE(contains(Off, Sig));
  EXPECT_TRUE(contains(On, Sig));
  // The default-off behavior is the pre-change wrapper: no hook declaration may
  // be emitted at all when the flag is off.
  EXPECT_FALSE(contains(Off, "ejit_stab"));
}

TEST_F(WrapperGenIRTest, EarlyCommonGateCarriesTheRealBoundPointerDescriptor) {
  EnableEJitSmallTableHooks = true;
  EJitInlineCache = false;
  std::string Text = entryBodyText("g_src", "g_out", kWrapCells);
  const std::string Signature = "(i32 %cell, i32 %x)";
  const size_t Sig = Text.find(Signature);
  ASSERT_NE(Sig, std::string::npos);
  Text.replace(Sig, Signature.size(), "(i32 %cell, i32 %x, ptr %config)");
  const std::string Metadata = "!0 = !{!1, !2}";
  const size_t Meta = Text.find(Metadata);
  ASSERT_NE(Meta, std::string::npos);
  Text.replace(Meta, Metadata.size(), "!0 = !{!1, !2, !4}");
  Text += "\n!4 = !{!\"ejit_bound_ptr\", !\"tenant_cell\", i32 2, i64 8}\n";
  auto M = parseModule(Ctx, Text, "WrapperGenIRTest.bound");
  ASSERT_NE(M, nullptr);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  Analyses A;
  const std::string IR = runWrapperGen(*M, A);
  ASSERT_FALSE(verifyModule(*M, &errs())) << IR;
  auto *F = M->getFunction("f_entry");
  ASSERT_NE(F, nullptr);
  unsigned Gates = 0;
  for (auto &BB : *F)
    for (auto &I : BB)
      if (auto *Call = dyn_cast<CallBase>(&I))
        if (auto *Callee = Call->getCalledFunction();
            Callee && Callee->getName() == "ejit_stab_wrapper_enter") {
          ++Gates;
          ASSERT_EQ(Call->arg_size(), 8u);
          EXPECT_FALSE(isa<ConstantPointerNull>(Call->getArgOperand(3)));
          auto *Count = dyn_cast<ConstantInt>(Call->getArgOperand(4));
          ASSERT_NE(Count, nullptr);
          EXPECT_EQ(Count->getZExtValue(), 1u);
        }
  EXPECT_EQ(Gates, 1u);
  EXPECT_TRUE(contains(IR, "store ptr %config")) << IR;
  EXPECT_TRUE(contains(IR, "store i32 8")) << IR;
  EXPECT_TRUE(contains(IR, "store i32 2")) << IR;
}

//===----------------------------------------------------------------------===//
// Part B: the generated wrapper, executed for real
//===----------------------------------------------------------------------===//

WrapElement g_wrap[kWrapCells];
int32_t g_wrap_out[kWrapCells];

void fillWrapConfig() {
  for (unsigned C = 0; C < kWrapCells; ++C) {
    g_wrap[C].mode = 1;
    g_wrap[C].bycell = static_cast<int32_t>(10 + C);
  }
  std::memset(g_wrap_out, 0, sizeof(g_wrap_out));
}

/// The real AOT baseline result of the entry, read from LIVE memory: this is
/// what the generated wrapper's AOT body computes.
int32_t wrapAotResult(const WrapElement &E, int32_t X) {
  return E.mode * 1000 + E.bycell * 100 + X * 3;
}

/// The AOT baseline callable with the entry's exact signature: the pointer the
/// host records as the AOT side of every decision (it is never called by the
/// host).
int32_t wrapAotEntry(int32_t Cell, int32_t X) {
  return wrapAotResult(g_wrap[Cell], X);
}

/// The callable the AOT image's inline-cache cell holds in this fixture: in a
/// product image that cell holds the resolved specialization (or the MissFn
/// sentinel), so a test-owned callable here lets the wrapper's UNCHANGED
/// dispatch be observed instead of guessed. It must be called exactly when no
/// small-table policy owns the entry, and never when the host admits or refuses.
unsigned g_cellCalls = 0;
int32_t wrapCellCallable(int32_t, int32_t) {
  ++g_cellCalls;
  return -777;
}

// A semantic-neutral observation of the AOT fallback. The generated AOT image
// gets this call before wrapper generation; the registered business bitcode
// does not. It distinguishes fallback from admitted T1/T2 without mutating a
// configuration whose values an outstanding protected borrow promises stable.
unsigned g_aotCalls = 0;
void wrapAotObserve() { ++g_aotCalls; }

/// The executed wrapper: the generated module is JIT-compiled and its entry is
/// the callable the application would call. `g_wrap`/`g_wrap_out` are the
/// module's globals and the runtime C ABI symbols are the process's REAL
/// implementations, wired in like the product image's symbol table.
class GeneratedWrapper {
public:
  static Expected<std::unique_ptr<GeneratedWrapper>>
  create(uint32_t FuncIdx, uint32_t CellSlot, bool Hooks, bool Icache = true,
         const std::string *BodyText = nullptr, bool SeedCacheHit = true,
         bool ObservePolicyEpoch = false) {
    auto W = std::unique_ptr<GeneratedWrapper>(new GeneratedWrapper());

    // The entry body: the plain fixture by default, or a caller-supplied variant
    // (the re-entrant one) for the tests that need a hook inside the call.
    const std::string Body =
        BodyText ? *BodyText
                 : entryBodyText("g_wrap", "g_wrap_out", kWrapCells);

    // The AOT image: the generated wrapper PLUS the per-function globals the AOT
    // registration fills, and the inline-cache cell table.
    std::string Text = wrapperModuleText(Body);
    auto AotModule = parseModule(W->ctx_, Text, "GeneratedWrapper");
    if (!AotModule)
      return make_error<StringError>("the AOT fixture module did not parse",
                                     inconvertibleErrorCode());
    auto *AotEntry = AotModule->getFunction("f_entry");
    if (!AotEntry || AotEntry->empty())
      return make_error<StringError>("the AOT fixture has no entry body",
                                     inconvertibleErrorCode());
    IRBuilder<> AotMarker(&*AotEntry->getEntryBlock().getFirstInsertionPt());
    AotMarker.CreateCall(AotModule->getOrInsertFunction(
        "wrap_aot_observe", FunctionType::get(AotMarker.getVoidTy(), false)));
    // The registered bitcode: the SAME entry WITHOUT the wrapper, which is what
    // the runtime compiles specializations from (a product registers the
    // pre-wrapper body). Kept alive for the host's request.
    W->compileModule_ =
        parseModule(W->ctx_, Body, "GeneratedWrapper.body");
    if (!W->compileModule_)
      return make_error<StringError>("the body module did not parse",
                                     inconvertibleErrorCode());

    bool SavedHooks = EnableEJitSmallTableHooks;
    bool SavedIcache = EJitInlineCache;
    bool SavedCtors = EnableEJitGlobalCtors;
    EnableEJitSmallTableHooks = Hooks;
    // The cache-HIT fixture seeds every cell with a callable. With Icache=false,
    // an unowned entry keeps the real ordinary resolver; an owned common entry
    // must branch before that resolver and must not acquire its bucket lease.
    EJitInlineCache = Icache;
    // No JIT'd constructors on this host: after the pass creates the wrapper's
    // globals, seed those actual globals with the values assigned by the real
    // registration APIs below.
    EnableEJitGlobalCtors = false;
    Analyses A;
    runWrapperGen(*AotModule, A);
    EnableEJitGlobalCtors = SavedCtors;
    EnableEJitSmallTableHooks = SavedHooks;
    EJitInlineCache = SavedIcache;

    if (Error E = seedGeneratedGlobals(*AotModule, FuncIdx, CellSlot,
                                      /*CacheHit=*/Hooks && Icache &&
                                          SeedCacheHit))
      return std::move(E);
    if (Hooks && Icache && !SeedCacheHit) {
      // Expose the PASS's actual zero-initialized cell table to this fixture's
      // ordinary registration, not a substitute table or seeded function.
      auto *Cache =
          AotModule->getGlobalVariable("__ejit_icache_fn_f_entry", true);
      if (!Cache || !Cache->getInitializer()->isNullValue())
        return make_error<StringError>("the cold inline cache is not empty",
                                       inconvertibleErrorCode());
      Cache->setLinkage(GlobalValue::ExternalLinkage);
      Cache->setVisibility(GlobalValue::DefaultVisibility);
    }
    W->wrappedIR_ = irToString(*AotModule);

    std::string VerifyErr;
    raw_string_ostream VOS(VerifyErr);
    if (verifyModule(*AotModule, &VOS))
      return make_error<StringError>("the generated wrapper is invalid IR: " +
                                         VOS.str(),
                                     inconvertibleErrorCode());

    auto JOrErr = orc::LLJITBuilder().create();
    if (!JOrErr)
      return JOrErr.takeError();
    W->jit_ = std::move(*JOrErr);
    if (Error E = defineRuntimeSymbols(*W->jit_, Hooks && Icache &&
                                                  SeedCacheHit,
                                       ObservePolicyEpoch))
      return std::move(E);

    auto TSM = orc::ThreadSafeModule(std::move(AotModule),
                                     std::make_unique<LLVMContext>());
    if (Error E = W->jit_->addIRModule(std::move(TSM)))
      return std::move(E);
    auto Sym = W->jit_->lookup("f_entry");
    if (!Sym)
      return Sym.takeError();
    W->entry_ = jitTargetAddressToFunction<int32_t (*)(int32_t, int32_t)>(
        Sym->getValue());
    if (!W->entry_)
      return make_error<StringError>("the wrapper entry did not resolve",
                                     inconvertibleErrorCode());
    if (Hooks && Icache && !SeedCacheHit) {
      auto Cache = W->jit_->lookup("__ejit_icache_fn_f_entry");
      if (!Cache)
        return Cache.takeError();
      W->icacheSlot_ = reinterpret_cast<void *>(Cache->getValue());
    }
    return std::move(W);
  }

  /// The registered-bitcode module (the entry body the runtime compiles
  /// specializations from), kept alive for the host's request.
  Module *compileModule() const { return compileModule_.get(); }

  int32_t call(int32_t Cell, int32_t X) const { return entry_(Cell, X); }

  StringRef wrappedIR() const { return wrappedIR_; }
  void *icacheSlot() const { return icacheSlot_; }

private:
  static std::string wrapperModuleText(const std::string &BodyText) {
    std::string T = hostTargetHeader();
    T += "\n    %A = type { i32, i32 }\n";
    T += "    @g_wrap = external global [" + Twine(kWrapCells).str() +
         " x %A]\n";
    T += "    @g_wrap_out = external global [" + Twine(kWrapCells).str() +
         " x i32]\n";
    // Let the pass create its own internal registration/cache globals. Seeding
    // internal globals before the pass causes its external-only name lookup to
    // ignore them and produce suffixed globals that the fixture never fills.
    T += "    declare i32 @wrap_cell_callable(i32, i32)\n";
    // The entry body itself: the same text the runtime compiles from.
    const size_t Def = BodyText.find("    define i32 @f_entry");
    T += BodyText.substr(Def);
    return T;
  }

  static Error seedGeneratedGlobals(Module &M, uint32_t FuncIdx,
                                    uint32_t CellSlot, bool CacheHit) {
    auto *FuncIndex = M.getGlobalVariable("__ejit_funcidx_f_entry", true);
    auto *DimType = M.getGlobalVariable(
        "__ejit_dimtype_" + std::string(kWrapPeriod), true);
    if (!FuncIndex || !DimType)
      return make_error<StringError>("the pass did not create registration globals",
                                     inconvertibleErrorCode());
    FuncIndex->setInitializer(
        ConstantInt::get(FuncIndex->getValueType(), FuncIdx));
    DimType->setInitializer(ConstantInt::get(DimType->getValueType(), CellSlot));

    // Hooks-on tests exercise the guarded hit route using a callable distinct
    // from the AOT body. Hooks-off tests retain the pass's MissFn sentinel and
    // therefore exercise the original default dispatch.
    if (CacheHit) {
      auto *Cache = M.getGlobalVariable("__ejit_icache_fn_f_entry", true);
      auto *Callable = M.getFunction("wrap_cell_callable");
      auto *CacheTy = Cache ? dyn_cast<ArrayType>(Cache->getValueType()) : nullptr;
      if (!CacheTy || !Callable)
        return make_error<StringError>("the pass did not create the cache-hit table",
                                       inconvertibleErrorCode());
      SmallVector<Constant *, 16> Cells(CacheTy->getNumElements(), Callable);
      Cache->setInitializer(ConstantArray::get(CacheTy, Cells));
    }
    return Error::success();
  }

  static Error defineRuntimeSymbols(orc::LLJIT &J, bool RegisterSeedCallable,
                                     bool ObservePolicyEpoch) {
    orc::SymbolMap Syms;
    auto &ES = J.getExecutionSession();
    auto Add = [&](StringRef Name, const void *Addr) {
      Syms[ES.intern(Name)] = orc::ExecutorSymbolDef(
          orc::ExecutorAddr::fromPtr(Addr),
          JITSymbolFlags::Exported | JITSymbolFlags::Callable);
    };
    // The module's own globals.
    Add("g_wrap", static_cast<const void *>(&g_wrap[0].mode));
    Add("g_wrap_out", static_cast<const void *>(&g_wrap_out[0]));
    if (RegisterSeedCallable)
      Add("wrap_cell_callable",
          reinterpret_cast<const void *>(&wrapCellCallable));
    Add("wrap_observe", reinterpret_cast<const void *>(&wrapObserve));
    Add("wrap_aot_observe", reinterpret_cast<const void *>(&wrapAotObserve));
    // The REAL runtime C ABI the generated wrapper calls: the process
    // implementations, not stand-ins, exactly as a product image exports them
    // to its AOT wrappers.
    Add("ejit_taskpool_compile_or_get_1d",
        reinterpret_cast<const void *>(&ejit_taskpool_compile_or_get_1d));
    Add("ejit_taskpool_release_read",
        reinterpret_cast<const void *>(&ejit_taskpool_release_read));
    Add("ejit_stab_enter", reinterpret_cast<const void *>(&ejit_stab_enter));
    Add("ejit_stab_wrapper_enter",
        reinterpret_cast<const void *>(&ejit_stab_wrapper_enter));
    Add("ejit_stab_wrapper_no_policy_current",
        ObservePolicyEpoch
            ? reinterpret_cast<const void *>(&observedNoPolicyCurrent)
            : reinterpret_cast<const void *>(&ejit_stab_wrapper_no_policy_current));
    Add("ejit_stab_leave", reinterpret_cast<const void *>(&ejit_stab_leave));
    Add("ejit_register_lifecycle",
        reinterpret_cast<const void *>(&ejit_register_lifecycle));
    Add("ejit_register_funcindex",
        reinterpret_cast<const void *>(&ejit_register_funcindex));
    Add("ejit_register_icache_slot",
        reinterpret_cast<const void *>(&ejit_register_icache_slot));
    return J.getMainJITDylib().define(orc::absoluteSymbols(std::move(Syms)));
  }

  LLVMContext ctx_;
  std::unique_ptr<Module> compileModule_;
  std::unique_ptr<orc::LLJIT> jit_;
  int32_t (*entry_)(int32_t, int32_t) = nullptr;
  void *icacheSlot_ = nullptr;
  std::string wrappedIR_;
};

/// The real registration retains this payload by address until runtime shutdown
/// has joined its worker. It is the pre-wrapper body, never an AOT stand-in.
std::vector<uint8_t> entryBodyBitcode() {
  LLVMContext Ctx;
  auto M = parseModule(Ctx, entryBodyText("g_wrap", "g_wrap_out", kWrapCells),
                       "entryBodyBitcode");
  if (!M)
    return {};
  SmallVector<char, 0> Buf;
  raw_svector_ostream OS(Buf);
  WriteBitcodeToFile(*M, OS);
  return std::vector<uint8_t>(Buf.begin(), Buf.end());
}

class GeneratedWrapperTest : public testing::Test {
protected:
  EJitRuntimeState State;
  bool SavedHooks = EnableEJitSmallTableHooks;
  bool SavedIcache = EJitInlineCache;

  std::shared_ptr<EJitSmallTableHostFactSource> Facts;
  std::unique_ptr<EJitSmallTableHost> Host;
  EJitSmallTableHost *prevGlobal_ = nullptr;
  std::unique_ptr<GeneratedWrapper> Wrapper;
  uint32_t FuncIdx = 0;
  uint32_t CellSlot = 0;
  SmallVector<EJitSmallTableDim, 1> Dims;
  std::vector<std::string> Periods;
  std::vector<uint8_t> RegisteredBitcode;

  void SetUp() override {
    gNoPolicyCheckObserver = nullptr;
    gNoPolicyCheckCalls = 0;
    EJitWrapperRuntimeTestAccess::setHostInstallationObserver(nullptr, nullptr);
    ejit_shutdown();
    EJitRegistrationStore::instance().consume();
    EJitRegistrationStore::instance().consumeError();
    // The wrapper's funcIndex/dimType assignment is process-global and
    // single-assignment: start from empty registries like a fresh image.
    EJitLifecycleRegistry::instance().reset();
    EJitFuncRegistry::instance().reset();
    prevGlobal_ = EJitSmallTableHost::installGlobal(nullptr);
    fillWrapConfig();
    g_cellCalls = 0;
    g_aotCalls = 0;
    g_observe = nullptr;
    State.getRegistry().registerStaticVar("g_wrap", &g_wrap[0].mode);
    State.getRegistry().registerStaticVar("g_wrap_out", &g_wrap_out[0]);
    FuncIdx = kEJitInvalidFuncIndex;
    CellSlot = kEJitInvalidDimType;
    ejit_register_funcindex("f_entry", &FuncIdx);
    ejit_register_lifecycle(kWrapPeriod, &CellSlot);
    ASSERT_NE(FuncIdx, kEJitInvalidFuncIndex);
    ASSERT_NE(CellSlot, kEJitInvalidDimType);
    RegisteredBitcode = entryBodyBitcode();
    ASSERT_FALSE(RegisteredBitcode.empty());
    ejit_register_period_array(kWrapPeriod, "g_wrap", &g_wrap[0].mode,
                               sizeof(g_wrap));
    ejit_register_static_var("g_wrap", &g_wrap[0].mode);
    ejit_register_static_var("g_wrap_out", &g_wrap_out[0]);
    ejit_register_bitcode("f_entry", RegisteredBitcode.data(),
                         RegisteredBitcode.size());
    ejit_config_t Cfg{};
    Cfg.compileMode = EJIT_COMPILE_ASYNC;
    Cfg.optLevel = EJIT_OPT_L2;
    ASSERT_EQ(ejit_init_pgo(&Cfg), EJIT_OK);
    for (unsigned C = 0; C < kWrapReadyCells; ++C)
      ASSERT_EQ(ejit_activate(kWrapPeriod, C), EJIT_OK) << "cell " << C;
    // No compile/resolve or generic PGO warmup occurs here. This is a genuine
    // Ready/activated runtime, not a mocked runtime-eligibility flag.
    ejit_taskpool_stats_t Stats{};
    ASSERT_EQ(ejit_taskpool_get_stats(&Stats), EJIT_OK);
    ASSERT_EQ(Stats.asyncCompiles, 0u);
    ASSERT_EQ(Stats.asyncEnqueues, 0u);
    ASSERT_EQ(Stats.pendingEntries, 0u);
  }

  void TearDown() override {
    gNoPolicyCheckObserver = nullptr;
    EJitWrapperRuntimeTestAccess::setHostInstallationObserver(nullptr, nullptr);
    g_observe = nullptr;
    // Join the worker before destroying any registered payload or fixture.
    ejit_shutdown();
    // The cold-icache case registered storage owned by Wrapper's AOT image.
    // Unregister it only after the owner joined and before that image dies.
    if (Wrapper && Wrapper->icacheSlot())
      ejitIcacheClearAll();
    EJitRegistrationStore::instance().consume();
    EJitRegistrationStore::instance().consumeError();
    if (Host) {
      if (EJitSmallTableHost::global() == Host.get())
        EJitSmallTableHost::installGlobal(nullptr);
      // A test that parked its host for an outstanding execution must not leave
      // it in the process-global retained registry across tests: take the
      // ownership back first, so the fixture's reset is the real destruction.
      EJitSmallTableHost::releaseRetainedOwner(Host.get());
      Host.reset();
    }
    // A FAILED test may have parked its owner and stopped before its own late
    // leave: nothing can deliver that completion after the test returned, so the
    // harness reclaims the registry here (production owners are released by the
    // late `leave`, never by this path).
    EJitSmallTableHost::abandonRetainedOwners();
    EXPECT_EQ(EJitSmallTableHost::retainedOwnerCount(), 0u)
        << "a parked owner leaked into the next test";
    EJitSmallTableHost::installGlobal(prevGlobal_);
    prevGlobal_ = nullptr;
    Wrapper.reset();
    EnableEJitSmallTableHooks = SavedHooks;
    EJitInlineCache = SavedIcache;
  }

  /// Build the generated wrapper and the configuration's fact source. The
  /// wrapper is the AOT image; `compileModule()` is the entry body the runtime
  /// compiles specializations from, exactly like the product's registered
  /// bitcode.
  bool makeWrapperAndFacts(bool Hooks = true) {
    auto W = GeneratedWrapper::create(FuncIdx, CellSlot, Hooks);
    if (!W) {
      ADD_FAILURE() << "generated wrapper: " << toString(W.takeError());
      return false;
    }
    Wrapper = std::move(*W);
    // A real configuration commit: the declared shape is the array's own 4
    // cells, and only cells 0..2 are confirmed ready.
    Facts = std::make_shared<EJitSmallTableHostFactSource>(
        "g_wrap", &g_wrap[0].mode, sizeof(g_wrap), /*Epoch=*/0xF00D);
    for (unsigned C = 0; C < kWrapReadyCells; ++C)
      Facts->addReadyMember({C}, 0xA000 + C);
    return true;
  }

  /// Hooks ON, inline cache OFF: common policy enters before ordinary resolution;
  /// only an unowned entry can reach the ordinary live taskpool resolver.
  bool makeResolvePathWrapper(bool ObservePolicyEpoch = false) {
    auto W = GeneratedWrapper::create(FuncIdx, CellSlot, /*Hooks=*/true,
                                      /*Icache=*/false, /*BodyText=*/nullptr,
                                      /*SeedCacheHit=*/false, ObservePolicyEpoch);
    if (!W) {
      ADD_FAILURE() << "generated resolve-path wrapper: "
                    << toString(W.takeError());
      return false;
    }
    Wrapper = std::move(*W);
    Facts = std::make_shared<EJitSmallTableHostFactSource>(
        "g_wrap", &g_wrap[0].mode, sizeof(g_wrap), /*Epoch=*/0xF00D);
    for (unsigned C = 0; C < kWrapReadyCells; ++C)
      Facts->addReadyMember({C}, 0xA000 + C);
    return true;
  }

  /// Register the actual pass-generated zero table with a fresh Async owner.
  /// All registered payload/table storage survives the owner's shutdown/join.
  bool makeColdIcacheWrapper(bool ObservePolicyEpoch = false) {
    auto W = GeneratedWrapper::create(
        FuncIdx, CellSlot, /*Hooks=*/true, /*Icache=*/true,
        /*BodyText=*/nullptr, /*SeedCacheHit=*/false, ObservePolicyEpoch);
    if (!W) {
      ADD_FAILURE() << "cold cache wrapper: " << toString(W.takeError());
      return false;
    }
    Wrapper = std::move(*W);
    if (!Wrapper->icacheSlot() ||
        *static_cast<uintptr_t *>(Wrapper->icacheSlot()) != 0) {
      ADD_FAILURE() << "the pass's actual cold cache is not empty";
      return false;
    }
    Facts = std::make_shared<EJitSmallTableHostFactSource>(
        "g_wrap", &g_wrap[0].mode, sizeof(g_wrap), /*Epoch=*/0xF00D);
    for (unsigned C = 0; C < kWrapReadyCells; ++C)
      Facts->addReadyMember({C}, 0xA000 + C);
    ejit_shutdown();
    ejit_register_icache_slot("f_entry", Wrapper->icacheSlot(), 1, nullptr);
    ejit_register_period_array(kWrapPeriod, "g_wrap", &g_wrap[0].mode,
                               sizeof(g_wrap));
    ejit_register_static_var("g_wrap", &g_wrap[0].mode);
    ejit_register_static_var("g_wrap_out", &g_wrap_out[0]);
    ejit_register_bitcode("f_entry", RegisteredBitcode.data(),
                         RegisteredBitcode.size());
    ejit_config_t Cfg{};
    Cfg.compileMode = EJIT_COMPILE_ASYNC;
    Cfg.optLevel = EJIT_OPT_L2;
    if (ejit_init_pgo(&Cfg) != EJIT_OK) {
      ADD_FAILURE() << "the real cold cache owner did not initialize";
      return false;
    }
    for (unsigned C = 0; C < kWrapReadyCells; ++C)
      if (ejit_activate(kWrapPeriod, C) != EJIT_OK) {
        ADD_FAILURE() << "cold cache activation failed: " << C;
        return false;
      }
    return true;
  }

  /// The re-entrant shape: the plain wrapper, over an entry that calls the
  /// observation hook from INSIDE the call, before its table load.
  bool makeReentrantWrapperAndFacts() {
    const std::string Body =
        reentrantEntryBodyText("g_wrap", "g_wrap_out", kWrapCells);
    auto W = GeneratedWrapper::create(FuncIdx, CellSlot, /*Hooks=*/true,
                                      /*Icache=*/true, &Body);
    if (!W) {
      ADD_FAILURE() << "generated re-entrant wrapper: "
                    << toString(W.takeError());
      return false;
    }
    Wrapper = std::move(*W);
    Facts = std::make_shared<EJitSmallTableHostFactSource>(
        "g_wrap", &g_wrap[0].mode, sizeof(g_wrap), /*Epoch=*/0xF00D);
    for (unsigned C = 0; C < kWrapReadyCells; ++C)
      Facts->addReadyMember({C}, 0xA000 + C);
    return true;
  }

  bool makeHost(EJitSmallTableHost::Options Opts = {}) {
    auto H = EJitSmallTableHost::create(Config(), State.getRegistry(), State,
                                        Facts, Opts);
    if (!H) {
      ADD_FAILURE() << "host create failed: " << toString(H.takeError());
      return false;
    }
    Host = std::move(*H);
    EJitSmallTableHost::installGlobal(Host.get());
    return true;
  }

  EJitSmallTableHost::EntryRequest makeRequest() {
    EJitSmallTableHost::EntryRequest Req;
    Req.module = Wrapper->compileModule();
    Req.entryName = "f_entry";
    Req.funcIndex = FuncIdx;
    Req.sourceVarName = "g_wrap";
    Dims.push_back({EJitSmallTableDim::Kind::Argument, /*argIndex=*/0,
                    /*modulus=*/0, /*extent=*/kWrapCells});
    Req.dims = Dims;
    Periods.push_back(kWrapPeriod);
    Req.dimPeriodNames = Periods;
    Req.codeGeneration = 1;
    return Req;
  }

  bool requestEntry() {
    EJitSmallTableHost::EntryRequest Req = makeRequest();
    std::string Why;
    if (Error E = Host->requestEntry(
            Req, reinterpret_cast<void *>(&wrapAotEntry), Why)) {
      ADD_FAILURE() << "requestEntry failed: " << toString(std::move(E))
                    << " (" << Why << ")";
      return false;
    }
    return true;
  }

  /// Common-T1 completion + freeze + T2 + publication through the runtime's own
  /// sampling protocol. This drives the host-side sampling loop; the tests that
  /// must prove the window is filled by REAL business traffic use
  /// `fillT1ThroughWrapper` instead.
  bool driveAndPublish() {
    const uint64_t Budget = Host->runtime().sampleBudget();
    const uint64_t Done = Host->driveSampling(Budget * 4 + 8, /*Arg=*/1);
    if (Done == 0) {
      ADD_FAILURE() << "the sampling driver performed no real execution";
      return false;
    }
    std::string Why;
    if (Error E = Host->publishGeneration(Why)) {
      ADD_FAILURE() << "publishGeneration failed: " << toString(std::move(E))
                    << " (" << Why << ")";
      return false;
    }
    return true;
  }

  /// Freeze + common T2 + publication on the already-filled window.
  bool publishNow() {
    std::string Why;
    if (Error E = Host->publishGeneration(Why)) {
      ADD_FAILURE() << "publishGeneration failed: " << toString(std::move(E))
                    << " (" << Why << ")";
      return false;
    }
    return true;
  }

  /// Fill the ONE common T1 window with the entry's OWN calls through the
  /// generated wrapper -- never a host-side sampling loop. Returns the number of
  /// real calls made.
  uint64_t fillT1ThroughWrapper(unsigned Cell, int32_t X, uint64_t MaxCalls) {
    uint64_t Calls = 0;
    const uint64_t Budget = Host->runtime().sampleBudget();
    while (Host->runtime().currentSessionSamples() < Budget &&
           Calls < MaxCalls) {
      EXPECT_EQ(Wrapper->call(static_cast<int32_t>(Cell), X),
                wrapAotResult(g_wrap[Cell], X))
          << "call " << Calls << " cell " << Cell;
      ++Calls;
    }
    return Calls;
  }

  void checkPreBoundInstallationWindow(bool Replacement) {
    ASSERT_TRUE(makeResolvePathWrapper());
    std::unique_ptr<EJitSmallTableHost> Previous;
    if (Replacement) {
      ASSERT_TRUE(makeHost());
      ASSERT_TRUE(requestEntry());
      ASSERT_TRUE(Host->wrapperAdmissionReady());
      Previous = std::move(Host);
    }
    auto NewHost = EJitSmallTableHost::create(
        Config(), State.getRegistry(), State, Facts, {});
    ASSERT_TRUE(static_cast<bool>(NewHost)) << toString(NewHost.takeError());
    Host = std::move(*NewHost);
    Dims.clear();
    Periods.clear();
    ASSERT_TRUE(requestEntry()); // prepare while NOT installed; real ORC T1
    ASSERT_TRUE(Host->bound());
    ASSERT_NE(Host->instrumentedEntry(), nullptr);
    ASSERT_FALSE(Host->wrapperAdmissionReady());
    auto Counter = Host->runtime().engine().lookup(1, "__profc_f_entry");
    ASSERT_TRUE(static_cast<bool>(Counter)) << toString(Counter.takeError());
    const auto *Raw = static_cast<const uint64_t *>(*Counter);
    ASSERT_EQ(*Raw, 0u);
    bool Observed = false;
    std::function<void(EJitSmallTableHost *)> Observe = [&](auto *Installing) {
      Observed = true;
      ASSERT_EQ(Installing, Host.get());
      ASSERT_EQ(EJitSmallTableHost::global(), Installing);
      ASSERT_FALSE(Installing->wrapperAdmissionReady());
      EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
      EXPECT_EQ(g_aotCalls, 1u);
      EXPECT_EQ(g_cellCalls, 0u);
      EXPECT_EQ(*Raw, 0u);
      EXPECT_EQ(Installing->runtime().currentSessionSamples(), 0u);
      EXPECT_EQ(Installing->physicalExecutions(), 0u);
      if (Previous)
        EXPECT_EQ(Previous->runtime().currentSessionSamples(), 0u);
    };
    EJitWrapperRuntimeTestAccess::setHostInstallationObserver(
        [](void *Ctx, EJitSmallTableHost *H) {
          (*static_cast<std::function<void(EJitSmallTableHost *)> *>(Ctx))(H);
        }, &Observe);
    auto ClearObserver = make_scope_exit([&] {
      EJitWrapperRuntimeTestAccess::setHostInstallationObserver(nullptr, nullptr);
    });
    EJitSmallTableHost::installGlobal(Host.get());
    ASSERT_TRUE(Observed) << "execute in the exact published-before-handoff window";
    ASSERT_TRUE(Host->wrapperAdmissionReady());
    ASSERT_GE(Host->ownerWorkerOperations(), 1u);
    EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
    EXPECT_EQ(g_aotCalls, 1u);
    EXPECT_EQ(*Raw, 1u);
    EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u);
    EXPECT_EQ(Host->physicalExecutions(), 0u);
    // Reinstalling the effective same Host must not start another handoff or
    // invalidate an admitted execution merely because a provider repeats setup.
    const uint64_t Epoch = EJitSmallTableHost::policyEpoch();
    const uint64_t WorkerOps = Host->ownerWorkerOperations();
    Observed = false;
    EXPECT_EQ(EJitSmallTableHost::installGlobal(Host.get()), Host.get());
    EXPECT_FALSE(Observed);
    EXPECT_EQ(EJitSmallTableHost::policyEpoch(), Epoch);
    EXPECT_EQ(Host->ownerWorkerOperations(), WorkerOps);
    EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u);
    if (Previous)
      EXPECT_TRUE(Previous->beginOwnerTeardown());
    ejit_taskpool_stats_t Stats{};
    ASSERT_EQ(ejit_taskpool_get_stats(&Stats), EJIT_OK);
    EXPECT_EQ(Stats.asyncEnqueues, 0u);
    EXPECT_EQ(Stats.asyncCompiles, 0u);
  }

  /// Genuine cold plain wrapper: all common samples bypass ordinary resolution.
  /// Request and publication use the real owner worker. The controller joins
  /// those bounded operations; common sampling itself comes from wrapper calls.
  void checkColdPlainCommonWindow(uint64_t Limit, bool Icache = false) {
    ASSERT_TRUE(Icache ? makeColdIcacheWrapper() : makeResolvePathWrapper());
    if (Icache)
      ASSERT_EQ(*static_cast<uintptr_t *>(Wrapper->icacheSlot()), 0u);
    EJitSmallTableHost::Options Opts;
    Opts.runtime.sampling.aggregateLimit = Limit;
    Opts.runtime.sampling.freezeWaitMillis = 25;
    ASSERT_TRUE(makeHost(Opts));
    ASSERT_TRUE(requestEntry());
    const uint64_t RequestWorkerOperations = Host->ownerWorkerOperations();
    ASSERT_GE(RequestWorkerOperations, 1u)
        << "cold common T1 must materialize on the actual owner worker";
    ejit_taskpool_stats_t Before{};
    ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
    ASSERT_EQ(Before.asyncEnqueues, 0u);
    ASSERT_EQ(Before.asyncCompiles, 0u);
    ASSERT_EQ(Before.pendingEntries, 0u);
    ASSERT_EQ(Before.readyEntries, 0u);
    ASSERT_TRUE(Host->runtime().sessionOpen());
    ASSERT_EQ(Host->runtime().sampleBudget(), Limit);
    const int32_t X = 4;
    for (uint64_t I = 0; I < Limit; ++I) {
      const unsigned Cell = I % kWrapReadyCells;
      ASSERT_EQ(Wrapper->call(Cell, X), wrapAotResult(g_wrap[Cell], X))
          << "cold call " << I << " cell " << Cell;
      ASSERT_EQ(Host->runtime().currentSessionSamples(), I + 1);
      EXPECT_EQ(Host->physicalExecutions(), 0u);
    }
    EXPECT_EQ(g_aotCalls, 0u);
    EXPECT_EQ(g_cellCalls, 0u);
    EXPECT_EQ(Host->runtime().sessionInFlight(), 0u);

    struct Snapshot {
      std::string name;
      uintptr_t address;
      uint64_t hash;
      std::vector<uint64_t> values;
    };
    std::vector<Snapshot> Snapshots;
    std::set<std::string> ExpectedNames;
    for (const std::string &Name : Host->runtime().engine().getLastCounterNames()) {
      ASSERT_TRUE(ExpectedNames.insert(Name).second);
      auto Counts = Host->runtime().engine().lookup(1, "__profc_" + Name);
      ASSERT_TRUE(static_cast<bool>(Counts)) << toString(Counts.takeError());
      auto Data = Host->runtime().engine().lookup(1, "__profd_" + Name);
      ASSERT_TRUE(static_cast<bool>(Data)) << toString(Data.takeError());
      const auto *Header =
          static_cast<const RawInstrProf::ProfileData<uintptr_t> *>(*Data);
      ASSERT_GT(Header->NumCounters, 0u);
      ASSERT_LE(Header->NumCounters, 1024u);
      Snapshot S{Name, reinterpret_cast<uintptr_t>(*Counts), Header->FuncHash,
                 std::vector<uint64_t>(Header->NumCounters)};
      std::memcpy(S.values.data(), *Counts, S.values.size() * sizeof(uint64_t));
      Snapshots.push_back(std::move(S));
    }
    ASSERT_EQ(Snapshots.size(), 1u);
    ASSERT_EQ(Snapshots.front().values.size(), 1u);
    EXPECT_EQ(Snapshots.front().values.front(), Limit)
        << "the aggregate count must be the actual emitted T1 counter";

    // Call limit+1 must execute observed AOT; every raw T1 counter stays fixed.
    EXPECT_EQ(Wrapper->call(0, X), wrapAotResult(g_wrap[0], X));
    EXPECT_EQ(g_aotCalls, 1u);
    EXPECT_EQ(Host->runtime().currentSessionSamples(), Limit);
    EXPECT_EQ(Host->physicalExecutions(), 0u);
    for (const auto &S : Snapshots) {
      std::vector<uint64_t> After(S.values.size());
      std::memcpy(After.data(), reinterpret_cast<const void *>(S.address),
                  After.size() * sizeof(uint64_t));
      EXPECT_EQ(After, S.values) << S.name;
    }
    std::string Why;
    auto Frozen = Host->runtime().freeze(Why);
    ASSERT_TRUE(static_cast<bool>(Frozen))
        << Why << ": " << toString(Frozen.takeError());
    const auto *Bundle = *Frozen;
    ASSERT_NE(Bundle, nullptr);
    EXPECT_EQ(Bundle->sampleCount, Limit);
    EXPECT_EQ(Bundle->participatingMembers, kWrapReadyCells);
    ASSERT_EQ(Bundle->counters.size(), ExpectedNames.size());
    auto Reader = InstrProfReader::create(
        MemoryBuffer::getMemBufferCopy(Bundle->profileData));
    ASSERT_TRUE(static_cast<bool>(Reader)) << toString(Reader.takeError());
    std::set<std::string> ProfileNames;
    for (const auto &Record : **Reader) {
      ASSERT_TRUE(ProfileNames.insert(Record.Name.str()).second);
      const Snapshot *Captured = nullptr;
      for (const auto &S : Snapshots)
        if (Record.Name == S.name)
          Captured = &S;
      ASSERT_NE(Captured, nullptr);
      EXPECT_EQ(Record.Hash, Captured->hash);
      EXPECT_EQ(Record.Counts, Captured->values);
    }
    EXPECT_FALSE((*Reader)->hasError());
    EXPECT_EQ(ProfileNames, ExpectedNames);
    ASSERT_TRUE(publishNow());
    ASSERT_GE(Host->ownerWorkerOperations(), RequestWorkerOperations + 1)
        << "profile consumption/common T2 must be another real owner operation";
    ASSERT_GE(Host->ownerWorkerOperations(), 2u);
    EXPECT_EQ(Host->bundle(), Bundle);
    EXPECT_EQ(Host->activeTier(), "final");
    for (unsigned Cell = 0; Cell < kWrapReadyCells; ++Cell) {
      EXPECT_EQ(Wrapper->call(Cell, X), wrapAotResult(g_wrap[Cell], X));
      EXPECT_EQ(Host->physicalExecutions(), 0u);
    }
    EXPECT_EQ(g_aotCalls, 1u);
    EXPECT_EQ(g_cellCalls, 0u);
    EXPECT_EQ(Facts->outstandingBorrows(), 0u);
    ejit_taskpool_stats_t After{};
    ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
    EXPECT_EQ(After.asyncEnqueues, Before.asyncEnqueues);
    EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles);
    EXPECT_EQ(After.pendingEntries, 0u);
    EXPECT_EQ(After.queueApproxSize, 0u);
    EXPECT_EQ(After.readyEntries, 0u);
    EXPECT_EQ(After.compileFailed, 0u);
    EXPECT_EQ(After.publishFailed, 0u);
    if (Icache)
      EXPECT_EQ(*static_cast<uintptr_t *>(Wrapper->icacheSlot()), 0u)
          << "common ownership bypasses even the actual cold cache/miss wrapper";
    outs() << "[PR231 early-wrapper] cold cells=3 samples=" << Limit
           << " path=" << (Icache ? "actual-zero-icache" : "plain")
           << " actual_counter=" << Snapshots.front().values.front()
           << " generic_enqueues=" << After.asyncEnqueues
           << " generic_compiles=" << After.asyncCompiles
           << " common_compile=real-owner-worker owner_worker_operations="
           << Host->ownerWorkerOperations() << " common_T2=executed\n";
  }
};

TEST_F(GeneratedWrapperTest, GeneratedWrapperCallsTheAdmittedPublishedEntry) {
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  // The generated IR really is the hooked shape (not a hand-written imitation).
  ASSERT_TRUE(Wrapper->wrappedIR().contains("call ptr @ejit_stab_wrapper_enter"))
      << Wrapper->wrappedIR();
  ASSERT_TRUE(makeHost());
  Host->setInvalidationHook([]() {});
  ASSERT_TRUE(requestEntry());

  // --- 1. Bound, but no published code yet: no FINAL entry is exposed. The
  // call is admitted to the instrumented (pre-publication) tier as one real
  // sample of the common window, so it returns the live-memory value and never
  // the cache cell's callable; nothing about publication is faked.
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_FALSE(Host->wouldDispatch({0}))
      << "no final entry may be exposed before publication";
  const int32_t X = 2;
  EXPECT_EQ(Wrapper->call(0, X), wrapAotResult(g_wrap[0], X));
  EXPECT_EQ(g_cellCalls, 0u)
      << "an admitted sampling call ran the cache cell instead of the host tier";
  EXPECT_EQ(Host->activeExecutions(), 0u)
      << "the sampling path left an execution in flight";
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u)
      << "the entry's own call must be one real sample of the common window";

  // --- 2. Common T1 completes, the session freezes, T2 compiles from the
  // immutable bundle and the admitted slots are published.
  ASSERT_TRUE(driveAndPublish());
  ASSERT_EQ(Host->publishedSlots(), kWrapReadyCells);
  EXPECT_TRUE(Host->codeReady());
  ASSERT_NE(Host->bundle(), nullptr);
  const uint64_t PublishedResourceGen = Host->publishedResourceGeneration();
  ASSERT_NE(Host->publishedCodeGeneration(), 0u);
  ASSERT_NE(PublishedResourceGen, 0u);

  // --- 3. The fallback marker distinguishes actual admitted execution while
  // leaving the protected source configuration unchanged.
  const int32_t Frozen = wrapAotResult(WrapElement{1, 10}, X); // cell 0, frozen

  EXPECT_TRUE(Host->wouldDispatch({0}));
  EXPECT_EQ(Wrapper->call(0, X), Frozen)
      << "the generated wrapper did not run the admitted published entry";
  EXPECT_EQ(g_cellCalls, 0u)
      << "the wrapper ran its cache cell instead of the published entry";
  EXPECT_EQ(g_aotCalls, 0u)
      << "the published dispatch fell back to the AOT body";
  EXPECT_EQ(Host->activeExecutions(), 0u)
      << "the admitted path did not settle its execution (leave missing)";

  // Every ready member is reachable through the same generated wrapper, and the
  // unready member of the declared space stays on the AOT body.
  for (unsigned C = 0; C < kWrapReadyCells; ++C) {
    const int32_t Want =
        wrapAotResult(WrapElement{1, static_cast<int32_t>(10 + C)}, X);
    EXPECT_EQ(Wrapper->call(static_cast<int32_t>(C), X), Want) << "cell " << C;
    EXPECT_EQ(Host->activeExecutions(), 0u);
  }
  EXPECT_EQ(g_aotCalls, 0u);
  EXPECT_FALSE(Host->wouldDispatch({kWrapReadyCells}));
  EXPECT_EQ(Wrapper->call(kWrapReadyCells, X),
            wrapAotResult(g_wrap[kWrapReadyCells], X))
      << "an unconfirmed row must stay AOT";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(g_aotCalls, 1u) << "the unconfirmed member must execute AOT";
  EXPECT_EQ(Host->activeExecutions(), 0u);

  // --- 4. The configuration moves: published slots are retracted, so the
  // wrapper refuses again and the AOT body (live memory) is what runs. The old
  // generation can then be retired.
  Host->noteConfigurationChange("test: configuration moved");
  EXPECT_EQ(Host->publishedSlots(), 0u);
  ASSERT_EQ(Facts->outstandingBorrows(), 0u);
  for (unsigned C = 0; C < kWrapCells; ++C)
    g_wrap[C].bycell += 500;
  const int32_t Live = wrapAotResult(g_wrap[0], X);
  ASSERT_NE(Frozen, Live);
  EXPECT_EQ(Wrapper->call(0, X), Live)
      << "a drained generation must not be reachable from the wrapper";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(g_aotCalls, 2u);
  EXPECT_EQ(Host->activeExecutions(), 0u);
  std::string Why;
  EXPECT_TRUE(Host->retireGenerationsUpTo(PublishedResourceGen, Why)) << Why;
}

//===----------------------------------------------------------------------===//
// Part C: the ONE common T1 window is filled by the entry's OWN calls
//===----------------------------------------------------------------------===//

TEST_F(GeneratedWrapperTest, ColdPlainWrapperFillsCommonT1WithoutGenericPgo) {
  checkColdPlainCommonWindow(/*Limit=*/64);
}

TEST_F(GeneratedWrapperTest, PreBoundInstallCannotAdmitBeforeItsRealWorkerHandoff) {
  checkPreBoundInstallationWindow(/*Replacement=*/false);
}

TEST_F(GeneratedWrapperTest,
       PreBoundSameFunctionReplacementCannotInheritThePreviousWrapperGrant) {
  checkPreBoundInstallationWindow(/*Replacement=*/true);
}

TEST_F(GeneratedWrapperTest, ColdPlainWrapperHonoursConfigurableBudgetOfEight) {
  checkColdPlainCommonWindow(/*Limit=*/8);
}

TEST_F(GeneratedWrapperTest, ColdZeroIcacheWrapperFillsCommonT1WithoutGenericPgo) {
  checkColdPlainCommonWindow(/*Limit=*/64, /*Icache=*/true);
}

TEST_F(GeneratedWrapperTest, ColdZeroIcacheWrapperHonoursConfigurableBudgetOfEight) {
  checkColdPlainCommonWindow(/*Limit=*/8, /*Icache=*/true);
}

TEST_F(GeneratedWrapperTest, WrapperCallsFillTheOneCommonT1OfSixtyFour) {
  // The spec's common-T1 policy: ONE session per entry/code generation, filled
  // by the real admitted calls of the entry's own traffic (aggregate 64 by
  // default), shared across the ready members -- never by a host-side sampling
  // loop and never 64 per member. The calls below go through the GENERATED
  // wrapper's own dispatch; the host only observes.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());

  const uint64_t Budget = Host->runtime().sampleBudget();
  EXPECT_EQ(Budget, 64u) << "the default aggregate budget is one common 64";
  EXPECT_TRUE(Host->runtime().sessionOpen());
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_FALSE(Host->wouldDispatch({0}))
      << "no FINAL entry may be exposed before publication";

  const int32_t X = 4;
  uint64_t Calls = 0;
  while (Host->runtime().currentSessionSamples() < Budget) {
    const unsigned C = static_cast<unsigned>(Calls % kWrapReadyCells);
    ASSERT_EQ(Wrapper->call(static_cast<int32_t>(C), X),
              wrapAotResult(g_wrap[C], X))
        << "call " << Calls << " cell " << C;
    ++Calls;
    ASSERT_LE(Calls, Budget + 1)
        << "the common window did not fill from the entry's real calls";
  }
  EXPECT_EQ(Calls, Budget)
      << "each admitted call is exactly one aggregate sample";
  EXPECT_EQ(Host->runtime().currentSessionSamples(), Budget);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 0u)
      << "every call completed before the freeze boundary";
  EXPECT_EQ(Host->physicalExecutions(), 0u)
      << "every admitted call left its execution";

  struct CounterSnapshot {
    std::string name;
    uintptr_t address;
    std::vector<uint64_t> values;
  };
  std::vector<CounterSnapshot> CountersBefore;
  // Read every real counter in the actual T1 module before the 65th call.
  // makeRequest binds this initial common generation to code-generation key 1.
  for (const std::string &Name : Host->runtime().engine().getLastCounterNames()) {
    auto Counters = Host->runtime().engine().lookup(1, "__profc_" + Name);
    ASSERT_TRUE(static_cast<bool>(Counters)) << toString(Counters.takeError());
    auto Data = Host->runtime().engine().lookup(1, "__profd_" + Name);
    ASSERT_TRUE(static_cast<bool>(Data)) << toString(Data.takeError());
    const auto *Header =
        static_cast<const RawInstrProf::ProfileData<uintptr_t> *>(*Data);
    ASSERT_GT(Header->NumCounters, 0u);
    ASSERT_LE(Header->NumCounters, 1024u);
    CounterSnapshot Snapshot{Name, reinterpret_cast<uintptr_t>(*Counters),
                             std::vector<uint64_t>(Header->NumCounters)};
    std::memcpy(Snapshot.values.data(), *Counters,
                Snapshot.values.size() * sizeof(uint64_t));
    CountersBefore.push_back(std::move(Snapshot));
  }
  ASSERT_FALSE(CountersBefore.empty());
  ASSERT_EQ(CountersBefore.front().values.size(), 1u)
      << "the fixture entry has one basic block and one real entry counter";
  EXPECT_EQ(CountersBefore.front().values.front(), Budget);

  // Exhaustion closes new T1 execution, not just bookkeeping. The next call
  // takes AOT and cannot increment any counter in the completed common window.
  EXPECT_EQ(g_aotCalls, 0u);
  EXPECT_EQ(Wrapper->call(0, X), wrapAotResult(g_wrap[0], X));
  EXPECT_EQ(Host->runtime().currentSessionSamples(), Budget);
  EXPECT_EQ(g_aotCalls, 1u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  for (const CounterSnapshot &Snapshot : CountersBefore) {
    std::vector<uint64_t> After(Snapshot.values.size());
    std::memcpy(After.data(), reinterpret_cast<const void *>(Snapshot.address),
                After.size() * sizeof(uint64_t));
    EXPECT_EQ(After, Snapshot.values) << Snapshot.name;
  }

  // Freeze first and verify the real T1 counters before common T2 replaces the
  // engine's lookup namespace for this same code-generation key.
  std::string Why;
  auto Frozen = Host->runtime().freeze(Why);
  ASSERT_TRUE(static_cast<bool>(Frozen))
      << Why << ": " << toString(Frozen.takeError());
  const EJitSmallTableProfileBundle *B = *Frozen;
  ASSERT_NE(B, nullptr);
  EXPECT_EQ(B->sampleCount, Budget);
  EXPECT_EQ(B->participatingMembers, kWrapReadyCells)
      << "the aggregate window is shared across members, not 64 per member";
  EXPECT_EQ(B->resourceGeneration, Host->runtime().resourceGeneration());

  // Every captured Tier-1 counter resolves in the bundle's actual generation;
  // symbol-name equality alone would not establish identity.
  EXPECT_FALSE(B->profileData.empty())
      << "freeze must carry a real synthesized profile, not a placeholder";
  ASSERT_GT(B->counters.size(), 0u);
  for (const PgoCounterRef &C : B->counters) {
    ASSERT_NE(C.pgoName, nullptr);
    const std::string CounterName = std::string("__profc_") + C.pgoName;
    auto Addr = Host->runtime().engine().lookup(B->codeGeneration, CounterName);
    ASSERT_TRUE(static_cast<bool>(Addr))
        << CounterName << ": " << toString(Addr.takeError());
    EXPECT_NE(C.profcAddr, 0u) << CounterName;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(*Addr), C.profcAddr) << CounterName;
  }

  const std::string FrozenProfile = B->profileData;
  ASSERT_TRUE(publishNow());
  ASSERT_EQ(Host->bundle(), B) << "T2 must consume the same frozen bundle";
  EXPECT_EQ(B->profileData, FrozenProfile);
  EXPECT_EQ(Host->publishedResourceGeneration(),
            Host->runtime().resourceGeneration());
  EXPECT_EQ(Host->publishedSlots(), kWrapReadyCells);
  EXPECT_TRUE(Host->codeReady());

  // The original configuration remains stable; the AOT marker proves that the
  // wrapper really reached the published T2 rather than a same-valued fallback.
  EXPECT_EQ(Wrapper->call(0, X), wrapAotResult(WrapElement{1, 10}, X))
      << "publication did not expose the frozen T2 entry";
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(g_aotCalls, 1u)
      << "publication must not add an AOT call after the over-quota fallback";
}

TEST_F(GeneratedWrapperTest, WrapperCallsHonourAConfigurableAggregateBudget) {
  // The aggregate limit is configurable: the SAME wrapper shape fills whatever
  // budget the entry was admitted with, and publication then consumes a bundle
  // of exactly that many samples.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 8;
  ASSERT_TRUE(makeHost(Opts));
  ASSERT_TRUE(requestEntry());
  EXPECT_EQ(Host->runtime().sampleBudget(), 8u);

  EXPECT_EQ(fillT1ThroughWrapper(/*Cell=*/0, /*X=*/2, /*MaxCalls=*/32), 8u)
      << "the configurable aggregate budget is not what filled the window";
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 8u);
  ASSERT_TRUE(publishNow());
  ASSERT_NE(Host->bundle(), nullptr);
  EXPECT_EQ(Host->bundle()->sampleCount, 8u);
  EXPECT_EQ(Host->publishedSlots(), kWrapReadyCells);
}

TEST_F(GeneratedWrapperTest, FreezeWaitsForTheLastWrapperExecution) {
  // Admission is not completion: with the last counted sample still OPEN through
  // the wrapper's own hook ABI, the freeze must refuse rather than read a
  // half-complete window, and the real leave must be what completes it.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 4;
  Opts.runtime.sampling.waitForInFlightOnFreeze = true;
  Opts.runtime.sampling.freezeWaitMillis = 100;
  ASSERT_TRUE(makeHost(Opts));
  ASSERT_TRUE(requestEntry());

  // Three completed samples through the generated wrapper.
  const int32_t X = 2;
  uint64_t Calls = 0;
  while (Host->runtime().currentSessionSamples() < 3) {
    ASSERT_EQ(Wrapper->call(0, X), wrapAotResult(g_wrap[0], X));
    ASSERT_LT(++Calls, 8u) << "the wrapper stopped producing samples";
  }
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 3u);

  // The fourth (the budget's last) admitted execution is entered through the
  // wrapper's own hook ABI and held open.
  ejit_dim_pair_t Dim = {CellSlot, 0};
  uint64_t Ticket = 0;
  const char *Reason = nullptr;
  uint64_t PolicyEpoch = 0;
  void *Entry = ejit_stab_wrapper_enter(FuncIdx, &Dim, 1, nullptr, 0, &Ticket,
                                        &Reason, &PolicyEpoch);
  ASSERT_NE(Entry, nullptr) << (Reason ? Reason : "");
  ASSERT_NE(Ticket, 0u);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 4u);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
  EXPECT_EQ(Host->physicalExecutions(), 1u);

  // The freeze is refused while that real execution has not returned.
  std::string Why;
  Error TooEarly = Host->publishGeneration(Why);
  EXPECT_TRUE(static_cast<bool>(TooEarly))
      << "a granted dispatch is not a completed sample";
  if (TooEarly)
    consumeError(std::move(TooEarly));
  EXPECT_FALSE(Why.empty());
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
  EXPECT_EQ(Host->physicalExecutions(), 1u)
      << "the refused freeze must not have dropped the execution";

  // The real return completes the window; the SAME session then freezes whole.
  ejit_stab_leave(Ticket);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 0u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  ASSERT_TRUE(publishNow());
  ASSERT_NE(Host->bundle(), nullptr);
  EXPECT_EQ(Host->bundle()->sampleCount, 4u);
  EXPECT_EQ(Host->publishedSlots(), kWrapReadyCells);
}

TEST_F(GeneratedWrapperTest, FreezeWaitsForTheRealSixtyFourthWrapperReturn) {
  // Unlike the ticket-only barrier test above, all 64 samples execute the
  // generated wrapper and its actual ORC T1. The final T1 calls the observer
  // before its table load, so freeze is attempted while that same machine-code
  // execution is still on the stack. Only the wrapper's automatic leave after
  // the real return may drain this reader; the test never calls leave itself.
  ASSERT_TRUE(makeReentrantWrapperAndFacts());
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.waitForInFlightOnFreeze = true;
  Opts.runtime.sampling.freezeWaitMillis = 25;
  ASSERT_TRUE(makeHost(Opts));
  Host->registerExtraSymbol("wrap_observe",
                            reinterpret_cast<void *>(&wrapObserve));
  ASSERT_TRUE(requestEntry());

  const uint64_t Budget = Host->runtime().sampleBudget();
  ASSERT_EQ(Budget, 64u);
  const uint64_t Gen = Host->runtime().resourceGeneration();
  ASSERT_NE(Gen, 0u);
  const uintptr_t Table =
      reinterpret_cast<uintptr_t>(Host->runtime().resource()->base());
  const int32_t X = 3;
  for (uint64_t I = 0; I + 1 < Budget; ++I) {
    const unsigned Cell = I % kWrapReadyCells;
    ASSERT_EQ(Wrapper->call(Cell, X), wrapAotResult(g_wrap[Cell], X));
  }
  ASSERT_EQ(Host->runtime().currentSessionSamples(), Budget - 1);
  ASSERT_EQ(Host->runtime().sessionInFlight(), 0u);
  ASSERT_EQ(Host->physicalExecutions(), 0u);
  ASSERT_EQ(Host->runtime().physicalReaders(Gen), 0u);
  ASSERT_GE(Facts->outstandingBorrows(), 1u);
  ASSERT_EQ(g_aotCalls, 0u);

  struct CounterSnapshot {
    std::string name;
    uintptr_t address;
    uintptr_t dataAddress;
    std::vector<uint64_t> values;
  };
  std::vector<CounterSnapshot> Counters;
  // Resolve the real T1 counter arrays before the final call and before T2 can
  // replace this code-generation key's lookup namespace.
  for (const std::string &Name : Host->runtime().engine().getLastCounterNames()) {
    auto Values = Host->runtime().engine().lookup(1, "__profc_" + Name);
    ASSERT_TRUE(static_cast<bool>(Values)) << toString(Values.takeError());
    auto Data = Host->runtime().engine().lookup(1, "__profd_" + Name);
    ASSERT_TRUE(static_cast<bool>(Data)) << toString(Data.takeError());
    const auto *Header =
        static_cast<const RawInstrProf::ProfileData<uintptr_t> *>(*Data);
    ASSERT_GT(Header->NumCounters, 0u);
    ASSERT_LE(Header->NumCounters, 1024u);
    Counters.push_back({Name, reinterpret_cast<uintptr_t>(*Values),
                        reinterpret_cast<uintptr_t>(*Data),
                        std::vector<uint64_t>(Header->NumCounters)});
  }
  ASSERT_EQ(Counters.size(), 1u);
  ASSERT_EQ(Counters.front().name, "f_entry");
  ASSERT_EQ(Counters.front().values.size(), 1u)
      << "the re-entrant fixture has one block and one real entry counter";
  ASSERT_EQ(*reinterpret_cast<const uint64_t *>(Counters.front().address),
            Budget - 1);

  bool Inside = false;
  bool SawTimeout = false;
  g_observe = [&]() {
    Inside = true;
    EXPECT_EQ(Host->runtime().currentSessionSamples(), Budget);
    EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
    EXPECT_EQ(Host->runtime().inFlight(), 1u);
    EXPECT_EQ(Host->physicalExecutions(), 1u);
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
    EXPECT_TRUE(Host->runtime().samplingProtected());
    const uint64_t Borrows = Facts->outstandingBorrows();
    EXPECT_GE(Borrows, 1u);
    EXPECT_EQ(*reinterpret_cast<const uint64_t *>(Counters.front().address),
              Budget)
        << "the 64th real T1 entry incremented its actual counter";

    std::string Why;
    const auto Started = std::chrono::steady_clock::now();
    auto TooEarly = Host->runtime().freeze(Why);
    const auto Waited = std::chrono::steady_clock::now() - Started;
    EXPECT_FALSE(static_cast<bool>(TooEarly))
        << "freeze must not consume counters while this T1 is still running";
    if (!TooEarly) {
      const std::string Failure = toString(TooEarly.takeError());
      EXPECT_TRUE(contains(Failure, "after the freeze wait")) << Failure;
      SawTimeout = contains(Why, "after the freeze wait");
    }
    EXPECT_TRUE(SawTimeout) << Why;
    EXPECT_GE(Waited, std::chrono::milliseconds(25));
    EXPECT_EQ(Host->bundle(), nullptr);
    EXPECT_TRUE(Host->runtime().sessionOpen());
    EXPECT_EQ(Host->publishedSlots(), 0u);
    EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
    EXPECT_EQ(Host->runtime().inFlight(), 1u);
    EXPECT_EQ(Host->physicalExecutions(), 1u);
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
    EXPECT_EQ(Host->runtime().resourceGeneration(), Gen);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(Host->runtime().resource()->base()),
              Table);
    EXPECT_TRUE(Host->runtime().samplingProtected());
    EXPECT_EQ(Facts->outstandingBorrows(), Borrows)
        << "the refused freeze must not release the running T1's borrow";
  };

  const unsigned LastCell = (Budget - 1) % kWrapReadyCells;
  EXPECT_EQ(Wrapper->call(LastCell, X), wrapAotResult(g_wrap[LastCell], X));
  g_observe = nullptr;
  ASSERT_TRUE(Inside);
  ASSERT_TRUE(SawTimeout);
  EXPECT_EQ(g_aotCalls, 0u)
      << "all 64 calls must have executed the real admitted common T1";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), Budget);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 0u);
  EXPECT_EQ(Host->runtime().inFlight(), 0u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 0u)
      << "the generated wrapper's real return must release its reader";
  EXPECT_TRUE(Host->runtime().samplingProtected())
      << "the completed window keeps its borrow until successful freeze";
  EXPECT_GE(Facts->outstandingBorrows(), 1u);
  for (CounterSnapshot &Counter : Counters)
    std::memcpy(Counter.values.data(),
                reinterpret_cast<const void *>(Counter.address),
                Counter.values.size() * sizeof(uint64_t));
  EXPECT_EQ(Counters.front().values.front(), Budget);

  std::string Why;
  auto Frozen = Host->runtime().freeze(Why);
  ASSERT_TRUE(static_cast<bool>(Frozen))
      << Why << ": " << toString(Frozen.takeError());
  const EJitSmallTableProfileBundle *Bundle = *Frozen;
  ASSERT_NE(Bundle, nullptr);
  EXPECT_EQ(Bundle->sampleCount, Budget);
  EXPECT_EQ(Bundle->participatingMembers, kWrapReadyCells);
  EXPECT_EQ(Bundle->resourceGeneration, Gen);
  EXPECT_EQ(Bundle->resourceAddress, Table);
  EXPECT_FALSE(Host->runtime().samplingProtected());
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);

  // Decode the actual frozen buffer and compare every emitted function's full
  // counter array, not just its function name or the logical sample count.
  const std::string CompleteProfile = Bundle->profileData;
  ASSERT_FALSE(CompleteProfile.empty());
  std::set<std::string> ExpectedNames;
  for (const CounterSnapshot &Counter : Counters)
    ASSERT_TRUE(ExpectedNames.insert(Counter.name).second);
  std::set<std::string> CapturedNames;
  for (const PgoCounterRef &Counter : Bundle->counters) {
    ASSERT_NE(Counter.pgoName, nullptr);
    ASSERT_TRUE(CapturedNames.insert(Counter.pgoName).second);
    const CounterSnapshot *Expected = nullptr;
    for (const CounterSnapshot &Candidate : Counters)
      if (Candidate.name == Counter.pgoName)
        Expected = &Candidate;
    ASSERT_NE(Expected, nullptr);
    EXPECT_EQ(Counter.profcAddr, Expected->address);
    EXPECT_EQ(Counter.profdAddr, Expected->dataAddress);
  }
  EXPECT_EQ(CapturedNames, ExpectedNames);
  auto ReaderOrErr = InstrProfReader::create(
      MemoryBuffer::getMemBufferCopy(CompleteProfile));
  ASSERT_TRUE(static_cast<bool>(ReaderOrErr))
      << toString(ReaderOrErr.takeError());
  std::set<std::string> ProfileNames;
  for (const NamedInstrProfRecord &Record : **ReaderOrErr) {
    ASSERT_TRUE(ProfileNames.insert(Record.Name.str()).second);
    const CounterSnapshot *Expected = nullptr;
    for (const CounterSnapshot &Counter : Counters)
      if (Record.Name == Counter.name)
        Expected = &Counter;
    ASSERT_NE(Expected, nullptr) << Record.Name.str();
    const auto *Header =
        reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(
            Expected->dataAddress);
    EXPECT_EQ(Record.Hash, Header->FuncHash);
    EXPECT_EQ(Record.Counts.size(), Header->NumCounters);
    EXPECT_EQ(Record.Counts, Expected->values) << Record.Name.str();
  }
  EXPECT_FALSE((*ReaderOrErr)->hasError());
  EXPECT_EQ(ProfileNames, ExpectedNames)
      << "freeze must preserve every emitted counter in the full profile";

  ASSERT_TRUE(publishNow());
  EXPECT_EQ(Host->bundle(), Bundle);
  EXPECT_EQ(Bundle->profileData, CompleteProfile);
  EXPECT_EQ(Host->publishedResourceGeneration(), Gen);
  EXPECT_EQ(Host->publishedSlots(), kWrapReadyCells);
  EXPECT_TRUE(Host->codeReady());
  EXPECT_EQ(Wrapper->call(LastCell, X), wrapAotResult(g_wrap[LastCell], X));
  EXPECT_EQ(g_aotCalls, 0u)
      << "the same generated wrapper must reach the published common T2";
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
}

TEST_F(GeneratedWrapperTest, WrapperLateMemberJoinsTheLiveGeneration) {
  // A COMPATIBLE late member (a coordinate the contract reports Extendable)
  // joins the SAME resource/code generation AFTER publication: it must become
  // reachable through the SAME generated wrapper, must not restart the aggregate
  // quota or open a new session, and must run that generation's own frozen table
  // rather than re-reading live memory.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  const uint64_t Budget = Host->runtime().sampleBudget();
  ASSERT_EQ(fillT1ThroughWrapper(/*Cell=*/0, /*X=*/3, Budget + 4), Budget);
  ASSERT_TRUE(publishNow());
  const uint64_t Gen = Host->publishedResourceGeneration();
  const uint64_t Session = Host->runtime().sessionId();
  const uint64_t Samples = Host->runtime().sampleCount();
  ASSERT_NE(Gen, 0u);
  ASSERT_EQ(Host->publishedSlots(), kWrapReadyCells);
  EXPECT_FALSE(Host->wouldDispatch({kWrapReadyCells}));

  // The fourth member of the declared shape becomes confirmed ready. Its row is
  // published from the LIVE configuration at admission time.
  Facts->addReadyMember({kWrapReadyCells}, 0xA000 + kWrapReadyCells);
  // Product instance activation and the Host fact-source notification are
  // distinct facts. The early wrapper gate requires BOTH, just as startup does
  // for the initial members; a Host-only activation must not authorize code.
  ASSERT_EQ(ejit_activate(kWrapPeriod, kWrapReadyCells), EJIT_OK);
  std::string AdmitWhy;
  ASSERT_EQ(Host->runtime().admitMember({kWrapReadyCells}, &AdmitWhy),
            EJitSmallTableAdmission::Extendable)
      << AdmitWhy;
  std::string Why;
  ASSERT_TRUE(Host->onProductActivated(kWrapPeriod, kWrapReadyCells, &Why))
      << Why;

  EXPECT_EQ(Host->publishedResourceGeneration(), Gen)
      << "a compatible late member must not rebuild the generation";
  EXPECT_EQ(Host->runtime().sessionId(), Session)
      << "a late member must not open a new sampling session";
  EXPECT_EQ(Host->runtime().sampleCount(), Samples)
      << "a late member must not restart the aggregate quota";
  EXPECT_EQ(Host->publishedSlots(), kWrapReadyCells + 1);
  EXPECT_TRUE(Host->wouldDispatch({kWrapReadyCells}));

  // The admitted row remains stable; the fallback marker distinguishes the
  // common published entry from AOT without violating the source borrow.
  const int32_t JoinedBycell = 10 + static_cast<int32_t>(kWrapReadyCells);
  EXPECT_EQ(Wrapper->call(kWrapReadyCells, 3),
            wrapAotResult(WrapElement{1, JoinedBycell}, 3))
      << "the late member must run the shared frozen generation";
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(g_aotCalls, 0u);
}

TEST_F(GeneratedWrapperTest, PausedTableReadSurvivesCancelRebuildAndRetire) {
  // The coordinator's P1 counterexample, executed for REAL: the mutation happens
  // from INSIDE the specialized call, after the entry was entered and BEFORE the
  // table load the same call still has to perform. An admission token held while
  // nothing executes is not proof; this call is genuinely reading the table when
  // the session is cancelled, a new generation is adopted, and the old
  // generation is asked to retire.
  ASSERT_TRUE(makeReentrantWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  Host->setInvalidationHook([]() {});
  Host->registerExtraSymbol("wrap_observe",
                            reinterpret_cast<void *>(&wrapObserve));
  ASSERT_TRUE(requestEntry());

  const int32_t X = 7;
  const uint64_t Budget = Host->runtime().sampleBudget();
  ASSERT_EQ(fillT1ThroughWrapper(/*Cell=*/0, X, Budget + 4), Budget)
      << "the re-entrant entry's own calls did not fill the common T1 window";
  ASSERT_TRUE(publishNow());
  const uint64_t Gen = Host->runtime().resourceGeneration();
  ASSERT_NE(Gen, 0u);
  ASSERT_EQ(Host->publishedResourceGeneration(), Gen);

  // Keep the source stable. The callback's live physical-reader checks and the
  // AOT marker prove that this is the published table-reading call.
  const int32_t Frozen = wrapAotResult(WrapElement{1, 10}, X);

  bool Inside = false;
  bool SawDeferred = false;
  g_observe = [&]() {
    Inside = true;
    EXPECT_EQ(Host->physicalExecutions(), 1u)
        << "the running call must hold its execution while inside the table";
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
    EXPECT_GE(Facts->outstandingBorrows(), 1u)
        << "the running call must hold its protected read";

    // Logical cancellation, while the call is inside its table: it must settle
    // admission/publication only, never the physical lease or the borrow.
    Host->cancel("test: cancel from inside the specialized call");
    EXPECT_EQ(Host->publishedSlots(), 0u);
    EXPECT_EQ(Host->physicalExecutions(), 1u)
        << "cancel is a logical settlement, not physical completion";
    EXPECT_EQ(Host->logicallyClosedExecutions(), 1u);
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
    EXPECT_GE(Facts->outstandingBorrows(), 1u)
        << "cancel must not release the running call's protected read";

    // A new generation is prepared while the old call still reads the old
    // table; the old storage moves to the retained set.
    SmallVector<EJitSmallTableRowKey, 4> NoExtra;
    std::string Why;
    Error Rebuild = Host->beginNextGeneration(NoExtra, Why);
    if (Rebuild) {
      ADD_FAILURE() << "beginNextGeneration refused while only a published call "
                       "of the old generation runs: "
                    << toString(std::move(Rebuild)) << " (" << Why << ")";
      consumeError(std::move(Rebuild));
      return;
    }
    EXPECT_GT(Host->runtime().resourceGeneration(), Gen);
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u)
        << "adopting a new generation must not release the old reader";
    EXPECT_GT(Host->runtime().retainedBytes(), 0u);

    // Retire the generation the running call is INSIDE. Accepted, DEFERRED.
    EXPECT_TRUE(Host->retireGenerationsUpTo(Gen, Why)) << Why;
    SawDeferred = true;
    EXPECT_TRUE(Host->hasRetiredExecutions());
    EXPECT_GT(Host->runtime().pendingRetireGenerationCount(), 0u);
    EXPECT_EQ(Host->runtime().stats().retiredGenerations, 0u)
        << "storage a running table read uses must not be freed";
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
  };

  // The generated wrapper runs the published T2 entry; from inside it the
  // observation cancels/rebuilds/retires, then the SAME call reads its table.
  EXPECT_EQ(Wrapper->call(0, X), Frozen)
      << "the paused table read did not complete with the frozen value";
  EXPECT_TRUE(Inside) << "the observation never ran inside the specialized call";
  EXPECT_TRUE(SawDeferred) << "the retirement was never requested from inside";
  EXPECT_EQ(g_aotCalls, 0u) << "the observation must run in the admitted T2";
  g_observe = nullptr;

  // The real return releases this execution's lease and performs the reclaim.
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 0u);
  EXPECT_FALSE(Host->hasRetiredExecutions());
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 0u)
      << "the deferred retirement must complete at the real return";
  EXPECT_GE(Host->runtime().stats().reclaimedAfterReaders, 1u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
  EXPECT_GT(Host->staleLeaveCount(), 0u)
      << "the late completion is stale for the cancelled session";
}

TEST_F(GeneratedWrapperTest, PausedSamplingExecutionSurvivesCancelRebuildAndRetire) {
  // The T1 half, executed for real: the SAME generated wrapper call is inside
  // the instrumented tier (the common sampling window) when the session is
  // cancelled, a new generation is adopted and the old generation is asked to
  // retire. The cancel drops the session's sample accounting, not the physical
  // reader; the retirement is deferred to the real return.
  ASSERT_TRUE(makeReentrantWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  Host->setInvalidationHook([]() {});
  Host->registerExtraSymbol("wrap_observe",
                            reinterpret_cast<void *>(&wrapObserve));
  ASSERT_TRUE(requestEntry());

  const int32_t X = 5;
  const uint64_t Gen = Host->runtime().resourceGeneration();
  ASSERT_NE(Gen, 0u);
  EXPECT_TRUE(Host->runtime().sessionOpen());
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);

  bool Inside = false;
  bool SawDeferred = false;
  g_observe = [&]() {
    Inside = true;
    EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u)
        << "the call admitted to the instrumented tier is a real sample";
    EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
    EXPECT_EQ(Host->physicalExecutions(), 1u);
    EXPECT_TRUE(Host->runtime().samplingProtected());
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);

    Host->cancel("test: cancel while the sampled call is inside the table");
    EXPECT_EQ(Host->runtime().sessionInFlight(), 0u)
        << "the cancelled session gave up its sample accounting";
    EXPECT_EQ(Host->runtime().inFlight(), 1u)
        << "the granted execution is still physically in flight";
    EXPECT_EQ(Host->physicalExecutions(), 1u);
    EXPECT_TRUE(Host->runtime().samplingProtected())
        << "the window's protected read guards the running call";
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);

    SmallVector<EJitSmallTableRowKey, 4> NoExtra;
    std::string Why;
    Error Rebuild = Host->beginNextGeneration(NoExtra, Why);
    if (Rebuild) {
      ADD_FAILURE() << "beginNextGeneration refused a cancelled session whose "
                       "real sample is still in flight: "
                    << toString(std::move(Rebuild)) << " (" << Why << ")";
      consumeError(std::move(Rebuild));
      return;
    }
    EXPECT_GT(Host->runtime().resourceGeneration(), Gen);
    EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u)
        << "the old generation is still physically read after the rebuild";
    EXPECT_GT(Host->runtime().retainedBytes(), 0u);

    EXPECT_TRUE(Host->retireGenerationsUpTo(Gen, Why)) << Why;
    SawDeferred = true;
    EXPECT_GT(Host->runtime().pendingRetireGenerationCount(), 0u);
    EXPECT_EQ(Host->runtime().stats().retiredGenerations, 0u)
        << "storage a running sampled call reads must not be freed";
  };

  EXPECT_EQ(Wrapper->call(0, X), wrapAotResult(g_wrap[0], X))
      << "the sampled table read did not complete";
  EXPECT_TRUE(Inside) << "the observation never ran inside the sampled call";
  EXPECT_TRUE(SawDeferred) << "the retirement was never requested from inside";
  g_observe = nullptr;

  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().inFlight(), 0u);
  EXPECT_FALSE(Host->runtime().samplingProtected())
      << "the cancelled window's borrow is released at its last real return";
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 0u);
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 0u)
      << "the deferred retirement completes at the real return";
  EXPECT_GE(Host->runtime().stats().reclaimedAfterReaders, 1u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
  EXPECT_GE(Host->runtime().stats().staleCallbacks, 1u)
      << "the late sample is a stale callback, never a completion of the new "
         "session";
  EXPECT_GT(Host->staleLeaveCount(), 0u);
}

TEST_F(GeneratedWrapperTest, HooksOnWithNoHostKeepsTheUnchangedDispatch) {
  // The default-off product state: the wrapper is built WITH the hooks but no
  // small-table host is installed. The early wrapper gate answers "no policy", so
  // the wrapper keeps its unchanged dispatch and calls the pointer its own cache
  // resolved (here: the fixture's cell callable) rather than being forced onto
  // the AOT body. One entry without a host must not lose its specialization, and
  // a product that never activates the feature stays on the unchanged path.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  ASSERT_EQ(EJitSmallTableHost::global(), nullptr);
  EXPECT_EQ(Wrapper->call(0, 5), -777)
      << "the no-policy path did not keep the wrapper's own dispatch";
  EXPECT_EQ(g_cellCalls, 1u);
}

TEST_F(GeneratedWrapperTest, ForeignHostKeepsTheUnchangedDispatch) {
  ASSERT_TRUE(makeWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  auto ForeignModule = CloneModule(*Wrapper->compileModule());
  ASSERT_NE(ForeignModule, nullptr);
  auto *ForeignEntry = ForeignModule->getFunction("f_entry");
  ASSERT_NE(ForeignEntry, nullptr);
  ForeignEntry->setName("foreign_entry");
  auto Req = makeRequest();
  Req.module = ForeignModule.get();
  Req.entryName = "foreign_entry";
  Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("foreign_entry");
  ASSERT_NE(Req.funcIndex, FuncIdx);
  std::string Why;
  Error E = Host->requestEntry(Req, reinterpret_cast<void *>(&wrapAotEntry), Why);
  ASSERT_FALSE(static_cast<bool>(E)) << Why << ": " << toString(std::move(E));
  ASSERT_TRUE(Host->bound());
  EXPECT_FALSE(Host->isBoundTo(FuncIdx));
  EXPECT_EQ(Wrapper->call(0, 5), -777)
      << "a Host for another function must leave the existing dispatch intact";
  EXPECT_EQ(g_cellCalls, 1u);
  EXPECT_EQ(g_aotCalls, 0u);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
}

TEST_F(GeneratedWrapperTest, OwnedWrapperRefusesADeactivatedInstance) {
  ASSERT_TRUE(makeWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());
  ASSERT_EQ(ejit_deactivate(kWrapPeriod, 0), EJIT_OK);
  ejit_taskpool_stats_t Before{};
  ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u)
      << "common readiness cannot bypass actual lifecycle activation";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
  ejit_taskpool_stats_t After{};
  ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.asyncEnqueues, Before.asyncEnqueues);
  EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles);
}

TEST_F(GeneratedWrapperTest, OwnedWrapperRefusesAfterActualRuntimeShutdown) {
  ASSERT_TRUE(makeWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());
  ASSERT_TRUE(Host->codeReady());
  ejit_shutdown();
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u)
      << "a still-installed Host cannot authorize code after runtime shutdown";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
}

TEST_F(GeneratedWrapperTest,
       ActualCommonT1PermissionRefusalDoesNotConsumeQuotaOrCounters) {
  ASSERT_TRUE(makeWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  auto *Pool = EJitWrapperRuntimeTestAccess::pool();
  ASSERT_NE(Pool, nullptr);
  ASSERT_NE(Pool->state(), nullptr);
  ASSERT_TRUE(Host->runtime().sessionOpen());
  ASSERT_EQ(Host->runtime().currentSessionSamples(), 0u);
  void *T1 = Host->instrumentedEntry();
  ASSERT_NE(T1, nullptr);
  ASSERT_EQ(Host->activeEntry(), T1);
  EJitCompiledCodeInfo RealInfo;
  ASSERT_TRUE(Host->runtime().engine().findCodeRange(T1, RealInfo));
  ASSERT_EQ(RealInfo.fnPtr, T1);
  ASSERT_GT(RealInfo.codeSize, 0u);

  struct RawSnapshot {
    std::string name;
    uintptr_t address;
    std::vector<uint64_t> values;
  };
  std::vector<RawSnapshot> BeforeCounters;
  for (const auto &Name : Host->runtime().engine().getLastCounterNames()) {
    auto Profc = Host->runtime().engine().lookup(1, "__profc_" + Name);
    auto Profd = Host->runtime().engine().lookup(1, "__profd_" + Name);
    ASSERT_TRUE(static_cast<bool>(Profc)) << toString(Profc.takeError());
    ASSERT_TRUE(static_cast<bool>(Profd)) << toString(Profd.takeError());
    const auto *Data =
        reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(*Profd);
    ASSERT_GT(Data->NumCounters, 0u);
    ASSERT_LE(Data->NumCounters, 1024u);
    RawSnapshot Snapshot{Name, reinterpret_cast<uintptr_t>(*Profc), {}};
    const auto *Raw = static_cast<const uint64_t *>(*Profc);
    Snapshot.values.assign(Raw, Raw + Data->NumCounters);
    for (uint64_t Count : Snapshot.values)
      EXPECT_EQ(Count, 0u);
    BeforeCounters.push_back(std::move(Snapshot));
  }
  ASSERT_FALSE(BeforeCounters.empty());

  struct Preparation {
    EJitSmallTableHost *host;
    EJitSmallTableHostFactSource *facts;
    void *entry;
    bool allow = false;
    unsigned calls = 0;
  } Prep{Host.get(), Facts.get(), T1};
  Pool->setSealMode(false);
  Pool->setPrepareCodeCallback(
      [](void *Ctx, const void *Entry) {
        auto &P = *static_cast<Preparation *>(Ctx);
        ++P.calls;
        EXPECT_EQ(Entry, P.entry)
            << "prepare the actual common object, not an ordinary cache slot";
        EXPECT_GT(P.host->physicalExecutions(), 0u);
        EXPECT_GT(P.host->runtime().physicalReaders(
                      P.host->runtime().resourceGeneration()), 0u)
            << "permission work must hold the actual generation alive";
        EXPECT_GT(P.facts->outstandingBorrows(), 0u);
        EXPECT_EQ(P.host->runtime().currentSessionSamples(), 0u)
            << "preparation must precede quota admission";
        return P.allow;
      },
      &Prep);
  auto Restore = make_scope_exit(
      [&] { Pool->setPrepareCodeCallback(nullptr, nullptr); });
  const size_t SessionBorrows = Facts->outstandingBorrows();
  ASSERT_EQ(SessionBorrows, 0u);
  ASSERT_FALSE(Host->runtime().samplingProtected());
  const uint64_t Session = Host->runtime().sessionId();
  ejit_taskpool_stats_t Before{};
  ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);

  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u);
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Prep.calls, 1u) << "the real permission primitive must be reached";
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);
  EXPECT_EQ(Host->runtime().sessionId(), Session);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(
                Host->runtime().resourceGeneration()), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), SessionBorrows)
      << "a refused preparation pin leaves without opening a sampling borrow";
  EXPECT_FALSE(Host->runtime().samplingProtected());
  for (const auto &Snapshot : BeforeCounters) {
    const auto *Raw = reinterpret_cast<const uint64_t *>(Snapshot.address);
    EXPECT_EQ(std::vector<uint64_t>(Raw, Raw + Snapshot.values.size()),
              Snapshot.values) << Snapshot.name;
  }

  Prep.allow = true;
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u)
      << "restored permissions must run the same actual common T1";
  EXPECT_EQ(Prep.calls, 2u);
  EXPECT_EQ(Host->activeEntry(), T1);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 1u)
      << "the first admitted sample establishes the session-long borrow";
  EXPECT_TRUE(Host->runtime().samplingProtected());
  ASSERT_EQ(BeforeCounters.size(), 1u);
  ASSERT_EQ(BeforeCounters.front().values.size(), 1u);
  EXPECT_EQ(*reinterpret_cast<const uint64_t *>(BeforeCounters.front().address),
            1u) << "the accepted call must really execute T1 instrumentation";
  ejit_taskpool_stats_t After{};
  ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.asyncEnqueues, Before.asyncEnqueues);
  EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles);
}

TEST_F(GeneratedWrapperTest, ActualCommonT2FourKSealRefusalFallsBackAndRecovers) {
  ASSERT_TRUE(makeWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());
  auto *Pool = EJitWrapperRuntimeTestAccess::pool();
  ASSERT_NE(Pool, nullptr);
  void *T2 = Host->activeEntry();
  ASSERT_NE(T2, nullptr);
  EJitCompiledCodeInfo RealInfo;
  ASSERT_TRUE(Host->runtime().engine().findCodeRange(T2, RealInfo));
  ASSERT_EQ(RealInfo.fnPtr, T2);
  ASSERT_GT(RealInfo.codeSize, 0u);
  ASSERT_LE(RealInfo.codeSize, 4u << 20);
  ASSERT_GT(RealInfo.poolBase, 0u);
  ASSERT_GT(RealInfo.poolSize, 0u);
  ASSERT_EQ(RealInfo.writableCount, 0u) << "final T2 has no PGO counter writes";
  constexpr uintptr_t Page = 4096;
  const uintptr_t CodePageBegin = RealInfo.codeStart & ~(Page - 1);
  const uintptr_t CodePageEnd =
      (RealInfo.codeStart + RealInfo.codeSize + Page - 1) & ~(Page - 1);
  ASSERT_GT(CodePageEnd, CodePageBegin);
  struct FourKPermission {
    EJitSmallTableHost *host;
    EJitCompiledCodeInfo info;
    uintptr_t pageBegin;
    uintptr_t pageEnd;
    unsigned splits = 0;
    std::vector<uintptr_t> seals;
    bool allow = false;
  } Permission{Host.get(), RealInfo, CodePageBegin, CodePageEnd};
  Pool->setSealMode(true);
  Pool->setSplitPoolCallback(
      [](void *Ctx, uintptr_t Base, uint64_t Size) {
        auto &P = *static_cast<FourKPermission *>(Ctx);
        ++P.splits;
        EXPECT_EQ(Base, P.info.poolBase);
        EXPECT_EQ(Size, P.info.poolSize);
        EXPECT_GT(P.host->physicalExecutions(), 0u);
        return true;
      }, &Permission);
  Pool->setSealPageCallback(
      [](void *Ctx, uintptr_t Address) {
        auto &P = *static_cast<FourKPermission *>(Ctx);
        P.seals.push_back(Address);
        EXPECT_EQ(Address % Page, 0u);
        EXPECT_GE(Address, P.pageBegin);
        EXPECT_LT(Address, P.pageEnd)
            << "seal the actual finalized object's pages only";
        EXPECT_GT(P.host->physicalExecutions(), 0u);
        EXPECT_GT(P.host->runtime().physicalReaders(
                      P.host->runtime().resourceGeneration()), 0u);
        return P.allow;
      }, &Permission);
  auto Restore = make_scope_exit([&] {
    Pool->setSplitPoolCallback(nullptr, nullptr);
    Pool->setSealPageCallback(nullptr, nullptr);
  });
  const uint64_t Samples = Host->runtime().sampleCount();
  ejit_taskpool_stats_t Before{};
  ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u);
  EXPECT_EQ(g_cellCalls, 0u);
  ASSERT_FALSE(Permission.seals.empty());
  EXPECT_GE(Permission.splits, 1u)
      << "the actual common pool must be split before a page seal is attempted";
  EXPECT_EQ(Permission.seals.front(), CodePageBegin);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(
                Host->runtime().resourceGeneration()), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
  EXPECT_EQ(Host->activeEntry(), T2);
  EXPECT_EQ(Host->runtime().sampleCount(), Samples);

  Permission.allow = true;
  Permission.seals.clear();
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u) << "a successful retry must execute actual common T2";
  ASSERT_EQ(Permission.seals.size(), (CodePageEnd - CodePageBegin) / Page);
  for (size_t I = 0; I < Permission.seals.size(); ++I)
    EXPECT_EQ(Permission.seals[I], CodePageBegin + I * Page);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
  ejit_taskpool_stats_t After{};
  ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.asyncEnqueues, Before.asyncEnqueues);
  EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles);
}

TEST_F(GeneratedWrapperTest, OffModeKeepsTheAlreadyCompiledCommonT2Hit) {
  ASSERT_TRUE(makeWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());
  auto *Pool = EJitWrapperRuntimeTestAccess::pool();
  ASSERT_NE(Pool, nullptr);
  ASSERT_TRUE(Host->codeReady());
  const auto *Bundle = Host->bundle();
  ASSERT_NE(Bundle, nullptr);
  const uint64_t Samples = Bundle->sampleCount;
  Pool->setSharedMode(EJitCompileMode::Off);
  ASSERT_EQ(Pool->getSharedMode(), EJitCompileMode::Off);
  ejit_taskpool_stats_t Before{};
  ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
  for (unsigned Cell = 0; Cell < kWrapReadyCells; ++Cell)
    EXPECT_EQ(Wrapper->call(Cell, 3), wrapAotResult(g_wrap[Cell], 3));
  EXPECT_EQ(g_aotCalls, 0u)
      << "Off prevents new ordinary compilation, not existing compiled hits";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->bundle(), Bundle);
  EXPECT_EQ(Bundle->sampleCount, Samples);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
  ejit_taskpool_stats_t After{};
  ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.asyncEnqueues, Before.asyncEnqueues);
  EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles);
  EXPECT_EQ(After.pendingEntries, Before.pendingEntries);
}

TEST_F(GeneratedWrapperTest, DisabledSharedInstanceCannotEnterCommonT1) {
  ASSERT_TRUE(makeWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  auto *Pool = EJitWrapperRuntimeTestAccess::pool();
  ASSERT_NE(Pool, nullptr);
  ASSERT_TRUE(Pool->isInstanceActive(CellSlot, 0));
  const size_t Borrows = Facts->outstandingBorrows();
  ASSERT_EQ(Borrows, 0u);
  ASSERT_FALSE(Host->runtime().samplingProtected());
  ejit_taskpool_set_instance_enabled(CellSlot, 0, 0);
  ASSERT_FALSE(Pool->isInstanceActive(CellSlot, 0));
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u);
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), Borrows);
  EXPECT_FALSE(Host->runtime().samplingProtected());
  ejit_taskpool_set_instance_enabled(CellSlot, 0, 1);
  ASSERT_TRUE(Pool->isInstanceActive(CellSlot, 0));
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 1u);
  EXPECT_TRUE(Host->runtime().samplingProtected());
}

TEST_F(GeneratedWrapperTest, ActualRuntimeReplacementCannotReviveTheOldHost) {
  ASSERT_TRUE(makeWrapperAndFacts());
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());
  const uint64_t OldOwner = currentEJitRuntimeOwnerIdentity();
  ASSERT_NE(OldOwner, 0u);
  void *OldT2 = Host->activeEntry();
  ASSERT_NE(OldT2, nullptr);
  const uint64_t OldResource = Host->runtime().resourceGeneration();
  ejit_shutdown();
  EXPECT_EQ(currentEJitRuntimeOwnerIdentity(), 0u);
  ejit_register_period_array(kWrapPeriod, "g_wrap", &g_wrap[0].mode,
                             sizeof(g_wrap));
  ejit_register_static_var("g_wrap", &g_wrap[0].mode);
  ejit_register_static_var("g_wrap_out", &g_wrap_out[0]);
  ejit_register_bitcode("f_entry", RegisteredBitcode.data(),
                       RegisteredBitcode.size());
  ejit_config_t Cfg{};
  Cfg.compileMode = EJIT_COMPILE_ASYNC;
  Cfg.optLevel = EJIT_OPT_L2;
  ASSERT_EQ(ejit_init_pgo(&Cfg), EJIT_OK);
  const uint64_t NewOwner = currentEJitRuntimeOwnerIdentity();
  ASSERT_NE(NewOwner, 0u);
  ASSERT_NE(NewOwner, OldOwner);
  for (unsigned Cell = 0; Cell < kWrapReadyCells; ++Cell)
    ASSERT_EQ(ejit_activate(kWrapPeriod, Cell), EJIT_OK);
  ASSERT_EQ(EJitSmallTableHost::global(), Host.get());
  ASSERT_TRUE(Host->codeReady());
  ASSERT_FALSE(Host->belongsToRuntimeOwner(NewOwner));
  ejit_taskpool_stats_t Before{};
  ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(g_aotCalls, 1u)
      << "new runtime Ready/activation may not bless an old Host's code";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->activeEntry(), OldT2);
  EXPECT_EQ(Host->runtime().resourceGeneration(), OldResource);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
  ejit_taskpool_stats_t After{};
  ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.asyncEnqueues, Before.asyncEnqueues);
  EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles);
}

TEST_F(GeneratedWrapperTest, RefusedBorrowNeverBecomesAPolicy) {
  // A configuration commit whose facts are not confirmed refuses the borrow, so
  // the request fails closed: no plan, no resource, no published slot, and the
  // host is not bound to the entry. The wrapper must then keep its UNCHANGED
  // dispatch (no policy), never be forced onto the AOT body by a half-registered
  // host, and own no execution.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  Facts->refuseBorrow("test: the configuration commit is not confirmed");
  ASSERT_TRUE(makeHost());
  EJitSmallTableHost::EntryRequest Req = makeRequest();
  std::string Why;
  Error E = Host->requestEntry(Req, reinterpret_cast<void *>(&wrapAotEntry), Why);
  EXPECT_TRUE(static_cast<bool>(E)) << "a refused borrow must fail the request";
  if (E)
    consumeError(std::move(E));
  EXPECT_FALSE(Host->bound());
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_FALSE(Host->wouldDispatch({0}));
  EXPECT_EQ(Wrapper->call(0, 1), -777)
      << "an unbound host must leave the entry's own dispatch intact";
  EXPECT_EQ(g_cellCalls, 1u);
  EXPECT_EQ(Host->activeExecutions(), 0u);
}

TEST_F(GeneratedWrapperTest, CancelledSessionRejectsTheStaleTicket) {
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());

  // A real entered execution through the hook ABI the wrapper emits: this is the
  // only way to hold a real ticket for the stale-callback assertion (the wrapper
  // itself releases its execution before returning).
  ejit_dim_pair_t Dim = {CellSlot, 0};
  uint64_t Ticket = 0;
  const char *Reason = nullptr;
  uint64_t PolicyEpoch = 0;
  void *Entry = ejit_stab_wrapper_enter(FuncIdx, &Dim, 1, nullptr, 0, &Ticket,
                                        &Reason, &PolicyEpoch);
  ASSERT_NE(Entry, nullptr) << (Reason ? Reason : "");
  ASSERT_NE(Ticket, 0u);

  const uint64_t Before = Host->staleLeaveCount();
  Host->cancel("test: the session was cancelled (timeout/queue failure)");
  EXPECT_EQ(Host->publishedSlots(), 0u);
  // The completion arrives after the session died: it is rejected and counted,
  // never merged into another generation.
  ejit_stab_leave(Ticket);
  EXPECT_EQ(Host->staleLeaveCount(), Before + 1);
  EXPECT_EQ(Host->activeExecutions(), 0u);

  // A cancelled session exposes nothing: the generated wrapper refuses and runs
  // its AOT body.
  EXPECT_FALSE(Host->wouldDispatch({0}));
  EXPECT_EQ(Wrapper->call(0, 1), wrapAotResult(g_wrap[0], 1));
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->activeExecutions(), 0u);
}

TEST_F(GeneratedWrapperTest, WrapperExecutionKeepsItsTableThroughCancelAndRetire) {
  // The P1 lifetime contract on the GENERATED wrapper path. The execution is
  // entered through the wrapper's own hook ABI (`ejit_stab_wrapper_enter`, the call the
  // emitted IR makes), so the lease under test is the one the generated wrapper
  // takes. While that execution is open the session is cancelled, the
  // configuration moves and the old resource generation is asked to retire: none
  // of those may free the column storage the specialized code reads, and the
  // reclamation must happen at the real return.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());
  const uint64_t Gen = Host->runtime().resourceGeneration();
  ASSERT_NE(Gen, 0u);
  const uint64_t PublishedCodeGen = Host->publishedCodeGeneration();
  const uint32_t Cell = 0;
  const int32_t X = 3;
  const int32_t Frozen = wrapAotResult(WrapElement{1, 10}, X);

  // The wrapper runs the published specialization over the stable source;
  // the AOT observation distinguishes the admitted path.
  EXPECT_EQ(Wrapper->call(Cell, X), Frozen)
      << "the generated wrapper did not run its admitted published entry";
  EXPECT_EQ(Host->physicalExecutions(), 0u)
      << "the wrapper's own return must have closed its execution";
  EXPECT_EQ(g_aotCalls, 0u);

  // A real entered execution through the emitted hook call, held OPEN.
  ejit_dim_pair_t Dim = {CellSlot, Cell};
  uint64_t Ticket = 0;
  const char *Reason = nullptr;
  uint64_t PolicyEpoch = 0;
  void *Entry = ejit_stab_wrapper_enter(FuncIdx, &Dim, 1, nullptr, 0, &Ticket,
                                        &Reason, &PolicyEpoch);
  ASSERT_NE(Entry, nullptr) << (Reason ? Reason : "");
  ASSERT_NE(Ticket, 0u);
  EXPECT_EQ(Entry, Host->activeEntry())
      << "the hook must hand back the published generation's entry";
  EXPECT_EQ(Host->physicalExecutions(), 1u);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
  EXPECT_GE(Facts->outstandingBorrows(), 1u)
      << "the open execution holds its own protected read";
  const uint64_t RetainedBefore = Host->runtime().retainedGenerationCount();

  // Cancel, create a replacement resource, then ask for the old generation to
  // retire. The source stays stable while the old protected execution is open.
  Host->cancel("test: cancel while a generated-wrapper execution is open");
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_EQ(Host->physicalExecutions(), 1u)
      << "cancel is a logical settlement, not physical completion";
  EXPECT_EQ(Host->logicallyClosedExecutions(), 1u);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
  EXPECT_EQ(Host->activeExecutions(), 1u);
  EXPECT_GE(Facts->outstandingBorrows(), 1u)
      << "cancel must not release the running execution's protected read";
  // The wrapper refuses now and takes its original AOT body. Cancellation
  // retracts dispatch but does not license mutation under the running borrow.
  const int32_t Live = wrapAotResult(g_wrap[Cell], X);
  EXPECT_EQ(Wrapper->call(Cell, X), Live)
      << "a cancelled generation must not be reachable from the wrapper";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(g_aotCalls, 1u);
  EXPECT_EQ(Host->physicalExecutions(), 1u)
      << "the AOT call must not disturb the open execution";

  std::string Why;
  SmallVector<EJitSmallTableRowKey, 4> NoExtra;
  Error Rebuild = Host->beginNextGeneration(NoExtra, Why);
  ASSERT_FALSE(static_cast<bool>(Rebuild))
      << Why << ": " << toString(std::move(Rebuild));
  const uint64_t ReplacementGen = Host->runtime().resourceGeneration();
  EXPECT_GT(ReplacementGen, Gen);
  ASSERT_TRUE(Host->retireGenerationsUpTo(Gen, Why)) << Why;
  EXPECT_TRUE(Host->hasRetiredExecutions())
      << "the retirement is deferred for the open generated-wrapper execution";
  EXPECT_GT(Host->runtime().pendingRetireGenerationCount(), 0u);
  EXPECT_EQ(Host->runtime().stats().retiredGenerations, 0u)
      << "storage a running call reads must not be freed";
  EXPECT_GT(Host->runtime().retainedBytes(), 0u);
  EXPECT_GE(Host->runtime().retainedGenerationCount(), RetainedBefore);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
  EXPECT_EQ(Facts->outstandingBorrows() > 0, true)
      << "the open execution's protected read survives cancel AND retirement";

  // The real return. It is a stale completion for sampling, never applied to a
  // newer session, and it is the event that releases the residual leases and
  // performs the deferred reclaim.
  Host->leave(Ticket);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 0u);
  EXPECT_FALSE(Host->hasRetiredExecutions());
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 0u)
      << "the deferred reclaim must happen at the real return";
  EXPECT_GE(Host->runtime().stats().reclaimedAfterReaders, 1u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u)
      << "every protected read of the retired generation is released";
  EXPECT_GT(Host->staleLeaveCount(), 0u)
      << "the late completion is a stale callback";

  // The current replacement resource survives retiring the old generation.
  EXPECT_FALSE(Host->codeReady());
  EXPECT_NE(PublishedCodeGen, 0u);
  EXPECT_EQ(Host->runtime().resourceGeneration(), ReplacementGen);
}

TEST_F(GeneratedWrapperTest, DisableParksTheOwnerUntilTheRunningExecutionLeaves) {
  // The owner-teardown half of the physical-lifetime contract: a DISABLE (or a
  // host replacement, which performs the same steps) must not destroy the host,
  // its runtime or the table resource while a real execution entered through the
  // generated wrapper's hook is still running. The object is retained and the
  // late `ejit_stab_leave` - the call the emitted IR makes - releases the
  // execution's generation.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());
  const uint64_t Gen = Host->runtime().resourceGeneration();
  ASSERT_NE(Gen, 0u);

  ejit_dim_pair_t Dim = {CellSlot, 0};
  uint64_t Ticket = 0;
  const char *Reason = nullptr;
  uint64_t PolicyEpoch = 0;
  void *Entry = ejit_stab_wrapper_enter(FuncIdx, &Dim, 1, nullptr, 0, &Ticket,
                                        &Reason, &PolicyEpoch);
  ASSERT_NE(Entry, nullptr) << (Reason ? Reason : "");
  ASSERT_NE(Ticket, 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);

  // The disable: logical teardown now, physical destruction deferred.
  const bool Destroyed = Host->beginOwnerTeardown();
  EXPECT_FALSE(Destroyed)
      << "a host with a running execution must not be destroyed";
  EXPECT_EQ(EJitSmallTableHost::global(), nullptr)
      << "the gate must stop handing out new executions immediately";
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_FALSE(Host->codeReady());
  EXPECT_EQ(Host->physicalExecutions(), 1u)
      << "the running execution is a physical reader, not a logical one";
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u)
      << "the table generation must still be retained under the running call";
  EXPECT_GE(Facts->outstandingBorrows(), 1u)
      << "the running execution's protected read must survive the disable";

  // The owner hands ownership to the process-global retained registry, exactly
  // as EJit::disableSmallTable does.
  EJitSmallTableHost *Raw = Host.release();
  EJitSmallTableHost::adoptRetired(std::unique_ptr<EJitSmallTableHost>(Raw));
  EXPECT_EQ(EJitSmallTableHost::retainedOwnerCount(), 1u);
  EXPECT_EQ(EJitSmallTableHost::retainedOwnerOutstandingExecutions(), 1u);

  // After disable the global gate is gone, so the wrapper's hook answers "no
  // policy" and the entry keeps its own cache dispatch.
  EXPECT_EQ(Wrapper->call(0, 5), -777)
      << "the no-policy path must be restored by the disable";
  EXPECT_EQ(g_cellCalls, 1u);

  // The late completion reaches the RETAINED owner (not the uninstalled global)
  // and releases exactly this execution's lease. Its final leave destroys the
  // owner, so observe the surviving fact source and retained-owner registry,
  // never dereference Raw after the completion.
  ejit_stab_leave(Ticket);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u)
      << "the disable must not leak the execution's protected read";
  EXPECT_EQ(EJitSmallTableHost::retainedOwnerCount(), 0u)
      << "the retained owner is released once its last execution returned";
  EXPECT_EQ(EJitSmallTableHost::retainedOwnerOutstandingExecutions(), 0u);
}

TEST_F(GeneratedWrapperTest, HooksOffWrapperIsTheUnchangedWrapper) {
  // Same fixture, hooks OFF: the generated wrapper has no hook call at all, and
  // its result is the live-memory AOT value (the sentinel-formed table sends the
  // probe into MissFn; its real Async resolve retains AOT until ready).
  // This is the default-off regression guard.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/false));
  EXPECT_FALSE(Wrapper->wrappedIR().contains("ejit_stab_"));
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());
  ASSERT_TRUE(driveAndPublish());
  for (unsigned C = 0; C < kWrapCells; ++C) {
    g_wrap[C].bycell += 500;
    EXPECT_EQ(Wrapper->call(static_cast<int32_t>(C), 1),
              wrapAotResult(g_wrap[C], 1))
        << "cell " << C;
  }
  EXPECT_EQ(g_cellCalls, 0u);
}

//===----------------------------------------------------------------------===//
// Part D: real ordinary-PGO control versus early common dispatch
//===----------------------------------------------------------------------===//

/// The base fixture starts real Async/PGO without prewarming. This control first
/// executes genuine ordinary T1/T2 before Host installation, then proves that
/// common-policy traffic bypasses further per-cell ordinary PGO sessions.
/// Common requests also use the real owner worker. Product readiness and board
/// permission transactions remain separate acceptance gates.
class GeneratedWrapperTaskpoolTest : public GeneratedWrapperTest {
protected:
  enum class EpochAction { Install, ReplaceUnbound, DisableUnbound };

  /// Pause only a genuine generated epoch guard. Both guards still execute the
  /// real production check; resolver success/profile/code are never fabricated.
  void checkEpochChangeWindow(unsigned Guard, bool Icache, EpochAction Action) {
    ASSERT_TRUE(Icache ? makeColdIcacheWrapper(/*ObservePolicyEpoch=*/true)
                       : makeResolvePathWrapper(/*ObservePolicyEpoch=*/true));
    auto *Pool = EJitWrapperRuntimeTestAccess::pool();
    ASSERT_NE(Pool, nullptr);
    ASSERT_NE(Pool->state(), nullptr);
    void *OrdinaryT1 = nullptr;
    uint32_t Bucket = ~uint32_t(0);
    const auto Deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    for (unsigned Try = 0; Try != 256 && !OrdinaryT1; ++Try) {
      const auto Status = ejit_taskpool_compile_or_get_1d(
          FuncIdx, CellSlot, 0, &OrdinaryT1, &Bucket);
      if (Status == EJIT_OK)
        break;
      ASSERT_EQ(Status, EJIT_PENDING);
      ASSERT_LT(std::chrono::steady_clock::now(), Deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_NE(OrdinaryT1, nullptr);
    ASSERT_LT(Bucket, kEJitSharedCacheBuckets);
    ejit_taskpool_release_read(Bucket);
    SmallVector<EJitWrapperCounterView, 4> Views;
    bool Captured = false;
    const auto Capture = Pool->runControlOnOwnerAndWait([&] {
      Captured = EJitWrapperRuntimeTestAccess::counters(
          static_cast<uint64_t>(FuncIdx) << 32, Views);
    });
    ASSERT_EQ(Capture.status,
              EJitSharedTaskPool::OwnerControlStatus::Completed);
    ASSERT_TRUE(Captured);
    ASSERT_EQ(Views.size(), 1u);
    const auto *Data =
        reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(
            Views.front().profdAddr);
    ASSERT_EQ(Data->NumCounters, 1u);
    ASSERT_EQ(Data->NameRef,
              IndexedInstrProf::ComputeHash(Views.front().pgoName));
    const auto *Raw = reinterpret_cast<const uint64_t *>(Views.front().profcAddr);
    ASSERT_EQ(*Raw, 0u);
    if (Icache) {
      ASSERT_TRUE(warmGenericPgo(0, 2)); // genuine ordinary T1/T2, no seeded cell
      ASSERT_NE(*static_cast<uintptr_t *>(Wrapper->icacheSlot()), 0u);
    }
    const uint64_t OldCount = *Raw;
    if (Action != EpochAction::Install) {
      ASSERT_TRUE(makeHost()); // unbound/foreign to this call: genuine no-policy
      ASSERT_FALSE(Host->bound());
    }
    ejit_taskpool_stats_t Before{};
    ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
    std::unique_ptr<EJitSmallTableHost> Displaced;
    auto Readers = [&] {
      uint64_t Total = 0;
      for (uint32_t I = 0; I != kEJitSharedCacheBuckets; ++I)
        Total += Pool->state()->buckets[I].readers.loadAcquire();
      return Total;
    };
    ASSERT_EQ(Readers(), 0u);
    bool Changed = false;
    gNoPolicyCheckCalls = 0;
    gNoPolicyCheckObserver = [&](unsigned Call) {
      if (Call != Guard)
        return;
      Changed = true;
      // Plain guard2 runs AFTER actual resolver success, with its real token.
      EXPECT_EQ(Readers(), (!Icache && Guard == 2) ? 1u : 0u);
      if (Action == EpochAction::DisableUnbound) {
        ASSERT_TRUE(Host->beginOwnerTeardown());
        ASSERT_EQ(EJitSmallTableHost::global(), nullptr);
      } else {
        if (Action == EpochAction::ReplaceUnbound)
          Displaced = std::move(Host);
        ASSERT_TRUE(makeHost());
        ASSERT_TRUE(requestEntry());
        ASSERT_TRUE(Host->wrapperAdmissionReady());
        EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);
      }
    };
    auto ClearGuard = make_scope_exit([&] { gNoPolicyCheckObserver = nullptr; });
    EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
    ASSERT_TRUE(Changed);
    EXPECT_EQ(gNoPolicyCheckCalls, Guard);
    EXPECT_EQ(g_aotCalls, 1u);
    EXPECT_EQ(g_cellCalls, 0u);
    EXPECT_EQ(*Raw, OldCount)
        << "stale no-policy dispatch must not execute the ordinary object";
    EXPECT_EQ(Readers(), 0u)
        << "the real success token must be released on the changed-epoch edge";
    gNoPolicyCheckObserver = nullptr;
    if (Action == EpochAction::DisableUnbound) {
      Host.reset();
      ASSERT_TRUE(makeHost());
      ASSERT_TRUE(requestEntry());
    }
    ASSERT_NE(Host, nullptr);
    ASSERT_TRUE(Host->wrapperAdmissionReady());
    EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);
    auto Common = Host->runtime().engine().lookup(1, "__profc_f_entry");
    ASSERT_TRUE(static_cast<bool>(Common)) << toString(Common.takeError());
    const auto *CommonRaw = static_cast<const uint64_t *>(*Common);
    ASSERT_EQ(*CommonRaw, 0u);
    EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
    EXPECT_EQ(g_aotCalls, 1u);
    EXPECT_EQ(gNoPolicyCheckCalls, Guard)
        << "the current common path must bypass both ordinary epoch delegates";
    EXPECT_EQ(*CommonRaw, 1u);
    EXPECT_EQ(*Raw, OldCount);
    EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u);
    EXPECT_EQ(Host->physicalExecutions(), 0u);
    ejit_taskpool_stats_t After{};
    ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
    EXPECT_EQ(After.asyncEnqueues, Before.asyncEnqueues);
    EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles);
    EXPECT_EQ(After.pendingEntries, 0u);
    EXPECT_EQ(After.compileFailed, 0u);
    EXPECT_EQ(After.publishFailed, 0u);
    if (Displaced)
      EXPECT_TRUE(Displaced->beginOwnerTeardown());
    outs() << "[PR231 epoch-window] path="
           << (Icache ? "actual-published-icache-hit" : "actual-plain-resolver")
           << " guard=" << Guard << " stale=AOT real_readers=0"
           << " old_counter_unchanged=" << OldCount
           << " next_common_counter=1 owner_worker_operations="
           << Host->ownerWorkerOperations() << "\n";
  }

  /// Real no-policy control: the generated wrapper itself opens and fills the
  /// ordinary per-cell PGO session, without a prewarmed or seeded cache entry.
  void checkNoPolicyColdOrdinaryWrapper(bool Icache) {
    ASSERT_EQ(EJitSmallTableHost::global(), nullptr);
    if (!Icache) {
      ASSERT_TRUE(makeResolvePathWrapper());
    } else {
      auto W = GeneratedWrapper::create(FuncIdx, CellSlot, /*Hooks=*/true,
                                        /*Icache=*/true, /*BodyText=*/nullptr,
                                        /*SeedCacheHit=*/false);
      ASSERT_TRUE(static_cast<bool>(W)) << toString(W.takeError());
      Wrapper = std::move(*W);
      ASSERT_NE(Wrapper->icacheSlot(), nullptr);
      ASSERT_EQ(*static_cast<uintptr_t *>(Wrapper->icacheSlot()), 0u);
      // Registration is intentionally frozen after init. Re-stage this real
      // image before a new real Async owner starts; payload and AOT cell-table
      // storage survive until TearDown joins that owner.
      ejit_shutdown();
      ejit_register_icache_slot("f_entry", Wrapper->icacheSlot(), 1, nullptr);
      ejit_register_period_array(kWrapPeriod, "g_wrap", &g_wrap[0].mode,
                                 sizeof(g_wrap));
      ejit_register_static_var("g_wrap", &g_wrap[0].mode);
      ejit_register_static_var("g_wrap_out", &g_wrap_out[0]);
      ejit_register_bitcode("f_entry", RegisteredBitcode.data(),
                           RegisteredBitcode.size());
      ejit_config_t Cfg{};
      Cfg.compileMode = EJIT_COMPILE_ASYNC;
      Cfg.optLevel = EJIT_OPT_L2;
      ASSERT_EQ(ejit_init_pgo(&Cfg), EJIT_OK);
      for (unsigned C = 0; C < kWrapReadyCells; ++C)
        ASSERT_EQ(ejit_activate(kWrapPeriod, C), EJIT_OK);
    }
    auto *Pool = EJitWrapperRuntimeTestAccess::pool();
    ASSERT_NE(Pool, nullptr);
    ASSERT_NE(Pool->state(), nullptr);
    ASSERT_EQ(Pool->getSharedMode(), EJitCompileMode::Async);
    ASSERT_EQ(Pool->state()->tier2Threshold.loadAcquire(), 64u);
    ejit_taskpool_stats_t Cold{};
    ASSERT_EQ(ejit_taskpool_get_stats(&Cold), EJIT_OK);
    ASSERT_EQ(Cold.asyncEnqueues, 0u);
    ASSERT_EQ(Cold.asyncCompiles, 0u);
    ASSERT_EQ(Cold.readyEntries, 0u);
    ASSERT_EQ(Cold.pendingEntries, 0u);

    constexpr unsigned MaxCalls = 256;
    unsigned Calls = 0;
    const auto Deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
    ++Calls;
    ASSERT_EQ(g_aotCalls, 1u)
        << "the first real Async miss must execute its untouched AOT body";
    ASSERT_EQ(g_cellCalls, 0u);
    ejit_taskpool_stats_t First{};
    ASSERT_EQ(ejit_taskpool_get_stats(&First), EJIT_OK);
    ASSERT_EQ(First.asyncEnqueues, 1u);

    // Wait for actual publication without using resolver calls to manufacture
    // hit counts. Every profile sample below comes from a generated wrapper.
    EJitSharedDiagnostics Diagnostics{};
    unsigned Waits = 0;
    do {
      Pool->getDiagnostics(Diagnostics);
      if (Diagnostics.tier1Compiles == 1)
        break;
      ASSERT_EQ(Diagnostics.compileFailed, 0u);
      ASSERT_EQ(Diagnostics.publishFailed, 0u);
      ASSERT_LT(++Waits, MaxCalls);
      ASSERT_LT(std::chrono::steady_clock::now(), Deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (true);
    ASSERT_EQ(Diagnostics.tier1Compiles, 1u);
    ASSERT_EQ(Diagnostics.tier2Compiles, 0u);
    ASSERT_EQ(Diagnostics.pgoActiveFunctionCount, 1u);
    if (Icache)
      ASSERT_EQ(*static_cast<uintptr_t *>(Wrapper->icacheSlot()), 0u)
          << "real ordinary T1 must stay out of the inline cache";

    SmallVector<EJitWrapperCounterView, 4> Views;
    bool Captured = false;
    const uint64_t Key = static_cast<uint64_t>(FuncIdx) << 32;
    const auto Capture = Pool->runControlOnOwnerAndWait([&] {
      Captured = EJitWrapperRuntimeTestAccess::counters(Key, Views);
    });
    ASSERT_EQ(Capture.status,
              EJitSharedTaskPool::OwnerControlStatus::Completed);
    ASSERT_TRUE(Captured);
    // This deliberately straight-line ordinary fixture has one real root and
    // one real counter. Full multi-function/CFG profiles have separate tests.
    ASSERT_EQ(Views.size(), 1u);
    ASSERT_NE(Views.front().profcAddr, 0u);
    ASSERT_NE(Views.front().profdAddr, 0u);
    const auto *Data =
        reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(
            Views.front().profdAddr);
    ASSERT_EQ(Data->NumCounters, 1u);
    ASSERT_EQ(Data->NameRef,
              IndexedInstrProf::ComputeHash(Views.front().pgoName));
    const auto *Raw =
        reinterpret_cast<const uint64_t *>(Views.front().profcAddr);
    ASSERT_EQ(*Raw, 0u);
    const unsigned SamplingAot = g_aotCalls;
    for (unsigned Sample = 0; Sample < 64; ++Sample) {
      ASSERT_LT(Calls, MaxCalls);
      ASSERT_LT(std::chrono::steady_clock::now(), Deadline);
      EXPECT_EQ(Wrapper->call(0, static_cast<int32_t>(Sample + 1)),
                wrapAotResult(g_wrap[0], Sample + 1));
      ++Calls;
      ASSERT_EQ(g_aotCalls, SamplingAot) << "real T1 sample " << Sample;
      ASSERT_EQ(*Raw, Sample + 1u)
          << "only an actual instrumented wrapper dispatch fills this counter";
    }
    ASSERT_EQ(*Raw, 64u);
    // The production engine retains both physical code-pool allocations for
    // this runtime lifetime (no releaser is wired); this final snapshot does
    // not assume arbitrary post-shutdown or post-retirement pointer validity.
    Waits = 0;
    do {
      Pool->getDiagnostics(Diagnostics);
      if (Diagnostics.tier2Compiles == 1)
        break;
      ASSERT_EQ(Diagnostics.compileFailed, 0u);
      ASSERT_EQ(Diagnostics.publishFailed, 0u);
      ASSERT_LT(++Waits, MaxCalls);
      ASSERT_LT(Calls, MaxCalls);
      ASSERT_LT(std::chrono::steady_clock::now(), Deadline);
      EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
      ++Calls;
      EXPECT_EQ(*Raw, 64u)
          << "completed ordinary T1 sampling must route pending calls to AOT";
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (true);

    struct Inventory {
      uint32_t func;
      unsigned matches = 0;
      void *entry = nullptr;
      unsigned tier = 0;
    } Published{FuncIdx};
    Pool->forEachCompiled(
        [](const EJitSharedCacheSlot &Slot, void *Ctx) {
          auto &I = *static_cast<struct Inventory *>(Ctx);
          if (Slot.funcIndex != I.func)
            return;
          ++I.matches;
          I.entry = reinterpret_cast<void *>(Slot.fnPtr.loadAcquire());
          I.tier = Slot.tier.loadAcquire();
          EXPECT_EQ(Slot.numDims, 1u);
          EXPECT_EQ(Slot.dims[0].instanceId, 0u);
        }, &Published);
    ASSERT_EQ(Published.matches, 1u);
    ASSERT_EQ(Published.tier, kEJitTierPgoUse);
    ASSERT_NE(Published.entry, nullptr);
    const unsigned SettledAot = g_aotCalls;
    EXPECT_EQ(Wrapper->call(0, 7), wrapAotResult(g_wrap[0], 7));
    ++Calls;
    ASSERT_EQ(g_aotCalls, SettledAot);
    ASSERT_EQ(g_cellCalls, 0u);
    if (Icache) {
      ASSERT_EQ(*static_cast<uintptr_t *>(Wrapper->icacheSlot()),
                reinterpret_cast<uintptr_t>(Published.entry));
      // A second direct inline-cache hit bypasses the ordinary resolver: no
      // taskpool cache-hit statistic can be charged for this real dispatch.
      ejit_taskpool_stats_t BeforeHit{}, AfterHit{};
      ASSERT_EQ(ejit_taskpool_get_stats(&BeforeHit), EJIT_OK);
      EXPECT_EQ(Wrapper->call(0, 8), wrapAotResult(g_wrap[0], 8));
      ++Calls;
      ASSERT_EQ(ejit_taskpool_get_stats(&AfterHit), EJIT_OK);
      EXPECT_EQ(AfterHit.cacheHits, BeforeHit.cacheHits);
      EXPECT_EQ(g_aotCalls, SettledAot);
    }
    ejit_taskpool_stats_t Settled{};
    ASSERT_EQ(ejit_taskpool_get_stats(&Settled), EJIT_OK);
    ASSERT_EQ(Settled.asyncEnqueues, 2u);
    ASSERT_EQ(Settled.asyncCompiles, 2u);
    ASSERT_EQ(Settled.readyEntries, 1u);
    ASSERT_EQ(Settled.pendingEntries, 0u);
    ASSERT_EQ(Settled.queueApproxSize, 0u);
    ASSERT_EQ(Settled.compileFailed, 0u);
    ASSERT_EQ(Settled.publishFailed, 0u);
    Pool->getDiagnostics(Diagnostics);
    ASSERT_EQ(Diagnostics.tier1Compiles, 1u);
    ASSERT_EQ(Diagnostics.tier2Compiles, 1u);
    ASSERT_EQ(Diagnostics.pgoCompletedFunctions, 1u);
    ASSERT_EQ(Diagnostics.pgoActiveFunctionCount, 0u);
    ASSERT_EQ(Diagnostics.profileMergeFails, 0u);

    Pool->setSharedMode(EJitCompileMode::Off);
    ASSERT_EQ(Pool->getSharedMode(), EJitCompileMode::Off);
    EXPECT_EQ(Wrapper->call(0, 9), wrapAotResult(g_wrap[0], 9));
    ++Calls;
    EXPECT_EQ(g_aotCalls, SettledAot)
        << "Off still permits a genuinely published ordinary T2 hit";
    ejit_taskpool_set_instance_enabled(CellSlot, 0, 0);
    ASSERT_FALSE(Pool->isInstanceActive(CellSlot, 0));
    if (Icache)
      ASSERT_EQ(*static_cast<uintptr_t *>(Wrapper->icacheSlot()), 0u);
    EXPECT_EQ(Wrapper->call(0, 10), wrapAotResult(g_wrap[0], 10));
    ++Calls;
    EXPECT_EQ(g_aotCalls, SettledAot + 1);
    EXPECT_EQ(g_cellCalls, 0u);
    ejit_taskpool_stats_t Final{};
    ASSERT_EQ(ejit_taskpool_get_stats(&Final), EJIT_OK);
    EXPECT_EQ(Final.asyncEnqueues, Settled.asyncEnqueues);
    EXPECT_EQ(Final.asyncCompiles, Settled.asyncCompiles);
    EXPECT_EQ(Final.pendingEntries, 0u);
    EXPECT_EQ(Final.queueApproxSize, 0u);
    EXPECT_LE(Calls, MaxCalls);
    EXPECT_EQ(EJitSmallTableHost::global(), nullptr);
    outs() << "[PR231 no-policy cold wrapper] path="
           << (Icache ? "actual-icache-miss->published-T2-hit" : "plain-resolver")
           << " actual_T1_counter=64 tier1=1 tier2=1 completed=1"
           << " active=0 pending=0 calls=" << Calls
           << " Off_hit=compiled disabled=AOT\n";
  }

  bool warmGenericPgo(unsigned Cell, int32_t X) {
    // Independent control: genuinely execute ordinary T1/T2 before installing
    // Host. Common-policy traffic below must not open another per-cell session.
    const auto Deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    void *FirstEntry = nullptr;
    uint64_t Executions = 0;
    while (std::chrono::steady_clock::now() < Deadline) {
      void *Fn = nullptr;
      uint32_t Bucket = ~uint32_t(0);
      const ejit_status_t Status = ejit_taskpool_compile_or_get_1d(
          FuncIdx, CellSlot, Cell, &Fn, &Bucket);
      if (Status == EJIT_OK && Fn) {
        if (!FirstEntry)
          FirstEntry = Fn;
        auto Entry = jitTargetAddressToFunction<int32_t (*)(int32_t, int32_t)>(
            pointerToJITTargetAddress(Fn));
        EXPECT_EQ(Entry(static_cast<int32_t>(Cell), X),
                  wrapAotResult(g_wrap[Cell], X));
        ++Executions;
        const bool Replaced = Fn != FirstEntry;
        ejit_taskpool_release_read(Bucket);
        ejit_taskpool_stats_t Stats{};
        if (ejit_taskpool_get_stats(&Stats) != EJIT_OK ||
            Stats.compileFailed != 0 || Stats.publishFailed != 0) {
          ADD_FAILURE() << "baseline PGO failed during real warm-up";
          return false;
        }
        if (Replaced && Stats.asyncCompiles >= 2 &&
            Stats.pendingEntries == 0 && Stats.queueApproxSize == 0) {
          EXPECT_GE(Executions, 64u)
              << "the generic T1 must have actually executed its samples";
          outs() << "[PR231 taskpool fixture] baseline warm-up: executions="
                 << Executions << " asyncCompiles=" << Stats.asyncCompiles
                 << " pending=0 queue=0; common host not installed\n";
          return true;
        }
      } else if (Status != EJIT_PENDING) {
        ADD_FAILURE() << "baseline PGO warm-up failed: status=" << Status
                      << " fn=" << Fn;
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ADD_FAILURE() << "baseline PGO warm-up timed out after " << Executions
                  << " real executions";
    return false;
  }

};

TEST_F(GeneratedWrapperTaskpoolTest,
       NoPolicyColdPlainWrapperRunsRealOrdinaryPgoAndOffHits) {
  checkNoPolicyColdOrdinaryWrapper(/*Icache=*/false);
}

TEST_F(GeneratedWrapperTaskpoolTest,
       NoPolicyFirstGuardRejectsAnActualNewHostInstallation) {
  checkEpochChangeWindow(1, false, EpochAction::Install);
}

TEST_F(GeneratedWrapperTaskpoolTest,
       NoPolicySecondGuardReleasesTheRealBucketAfterHostReplacement) {
  checkEpochChangeWindow(2, false, EpochAction::ReplaceUnbound);
}

TEST_F(GeneratedWrapperTaskpoolTest,
       NoPolicySecondGuardReleasesTheRealBucketAfterHostDisable) {
  checkEpochChangeWindow(2, false, EpochAction::DisableUnbound);
}

TEST_F(GeneratedWrapperTaskpoolTest,
       NoPolicyRealIcacheHitRejectsAnActualNewHostInstallation) {
  checkEpochChangeWindow(1, true, EpochAction::Install);
}

TEST_F(GeneratedWrapperTaskpoolTest,
       NoPolicyColdIcacheWrapperRunsRealMissAndPublishedHit) {
  checkNoPolicyColdOrdinaryWrapper(/*Icache=*/true);
}

TEST_F(GeneratedWrapperTaskpoolTest,
       LiveOrdinaryT1CallSurvivesRealWorkerCommonTakeover) {
  ASSERT_EQ(EJitSmallTableHost::global(), nullptr);
  auto *Pool = EJitWrapperRuntimeTestAccess::pool();
  ASSERT_NE(Pool, nullptr);
  ASSERT_NE(Pool->state(), nullptr);
  // Admit one genuinely compiled ordinary T1, far below its 64-hit trigger.
  // Keep the actual read token past the worker takeover: takeover must retire
  // logical slots without interpreting an outstanding call as already left.
  void *OldT1 = nullptr;
  uint32_t OldBucket = ~uint32_t(0);
  const auto Deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < Deadline) {
    void *Candidate = nullptr;
    uint32_t Bucket = ~uint32_t(0);
    const auto Status = ejit_taskpool_compile_or_get_1d(
        FuncIdx, CellSlot, 0, &Candidate, &Bucket);
    if (Status == EJIT_OK) {
      OldT1 = Candidate;
      OldBucket = Bucket;
      break;
    }
    ASSERT_EQ(Status, EJIT_PENDING);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_NE(OldT1, nullptr) << "ordinary T1 materialization timed out";
  ASSERT_LT(OldBucket, kEJitSharedCacheBuckets)
      << "this case requires a real retained read token, not a no-token hit";
  bool OldLeaseHeld = true;
  auto ReleaseOldLease = make_scope_exit([&] {
    if (OldLeaseHeld)
      ejit_taskpool_release_read(OldBucket);
  });
  EXPECT_GE(Pool->state()->buckets[OldBucket].readers.loadAcquire(), 1u);
  struct Inventory {
    uint32_t func;
    void *entry;
    unsigned matches = 0;
    unsigned tier = 0;
  } Inventory{FuncIdx, OldT1};
  Pool->forEachCompiled(
      [](const EJitSharedCacheSlot &Slot, void *Ctx) {
        auto &I = *static_cast<struct Inventory *>(Ctx);
        if (Slot.funcIndex == I.func &&
            Slot.fnPtr.loadAcquire() == reinterpret_cast<uintptr_t>(I.entry)) {
          ++I.matches;
          I.tier = Slot.tier.loadAcquire();
          EXPECT_EQ(Slot.numDims, 1u);
          EXPECT_EQ(Slot.dims[0].instanceId, 0u);
        }
      }, &Inventory);
  ASSERT_EQ(Inventory.matches, 1u);
  ASSERT_EQ(Inventory.tier, kEJitTierInstrumented);
  ejit_taskpool_stats_t Ordinary{};
  ASSERT_EQ(ejit_taskpool_get_stats(&Ordinary), EJIT_OK);
  ASSERT_EQ(Ordinary.asyncCompiles, 1u);
  ASSERT_EQ(Ordinary.readyEntries, 1u);
  ASSERT_EQ(Ordinary.pendingEntries, 0u);
  EJitSharedDiagnostics BeforeDiagnostics{};
  Pool->getDiagnostics(BeforeDiagnostics);
  ASSERT_EQ(BeforeDiagnostics.pgoActiveFunctionCount, 1u)
      << "the ordinary sampling session must still be live, not a settled T2";

  // The real driver's complete pair map is copied by its owner worker. This
  // fixture deliberately has ONE root/no helpers; multi-function completeness
  // is covered separately by the frozen-bundle and capture-failure cases.
  SmallVector<EJitWrapperCounterView, 4> OldViews;
  bool Captured = false;
  const uint64_t OrdinaryKey = static_cast<uint64_t>(FuncIdx) << 32;
  const auto Capture = Pool->runControlOnOwnerAndWait([&] {
    Captured = EJitWrapperRuntimeTestAccess::counters(OrdinaryKey, OldViews);
  });
  ASSERT_EQ(Capture.status, EJitSharedTaskPool::OwnerControlStatus::Completed);
  ASSERT_TRUE(Captured);
  ASSERT_EQ(OldViews.size(), 1u);
  const auto &OldCounter = OldViews.front();
  ASSERT_TRUE(StringRef(OldCounter.pgoName).ends_with("f_entry"));
  ASSERT_NE(OldCounter.profcAddr, 0u);
  ASSERT_NE(OldCounter.profdAddr, 0u);
  const auto *OldData =
      reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(
          OldCounter.profdAddr);
  ASSERT_EQ(OldData->NumCounters, 1u);
  const auto *OldRaw = reinterpret_cast<const uint64_t *>(OldCounter.profcAddr);
  ASSERT_EQ(*OldRaw, 0u);
  auto OldEntry = jitTargetAddressToFunction<int32_t (*)(int32_t, int32_t)>(
      pointerToJITTargetAddress(OldT1));
  EXPECT_EQ(OldEntry(0, 2), wrapAotResult(g_wrap[0], 2));
  ASSERT_EQ(*OldRaw, 1u) << "execute the actual ordinary instrumented object";

  ASSERT_TRUE(makeResolvePathWrapper());
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 8;
  Opts.runtime.sampling.freezeWaitMillis = 25;
  ASSERT_TRUE(makeHost(Opts));
  ASSERT_TRUE(requestEntry())
      << "worker takeover must not wait for the old call's bucket to leave";
  ASSERT_GE(Host->ownerWorkerOperations(), 1u);
  EXPECT_GE(Pool->state()->buckets[OldBucket].readers.loadAcquire(), 1u)
      << "logical retirement must not force the old read token to return";
  ejit_taskpool_stats_t TakenOver{};
  ASSERT_EQ(ejit_taskpool_get_stats(&TakenOver), EJIT_OK);
  EXPECT_EQ(TakenOver.readyEntries, 0u);
  EXPECT_EQ(TakenOver.pendingEntries, 0u);
  EXPECT_EQ(TakenOver.queueApproxSize, 0u);
  EXPECT_EQ(TakenOver.asyncCompiles, Ordinary.asyncCompiles);
  EXPECT_EQ(TakenOver.asyncEnqueues, Ordinary.asyncEnqueues);
  EJitSharedDiagnostics AfterDiagnostics{};
  Pool->getDiagnostics(AfterDiagnostics);
  EXPECT_EQ(AfterDiagnostics.pgoActiveFunctionCount, 0u);
  EXPECT_EQ(AfterDiagnostics.tier2Compiles, 0u)
      << "ordinary T1 must be aborted, not silently completed with fake samples";
  EXPECT_EQ(*OldRaw, 1u) << "handoff itself must not execute or reset old counters";
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);

  auto NewProfc = Host->runtime().engine().lookup(1, "__profc_f_entry");
  auto NewProfd = Host->runtime().engine().lookup(1, "__profd_f_entry");
  ASSERT_TRUE(static_cast<bool>(NewProfc)) << toString(NewProfc.takeError());
  ASSERT_TRUE(static_cast<bool>(NewProfd)) << toString(NewProfd.takeError());
  const auto *NewData =
      reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(*NewProfd);
  ASSERT_EQ(NewData->NumCounters, 1u);
  const auto *NewRaw = static_cast<const uint64_t *>(*NewProfc);
  ASSERT_NE(reinterpret_cast<uintptr_t>(NewRaw), OldCounter.profcAddr);
  ASSERT_EQ(*NewRaw, 0u);

  EXPECT_EQ(OldEntry(0, 4), wrapAotResult(g_wrap[0], 4))
      << "the still-running old call must remain executable after takeover";
  EXPECT_EQ(*OldRaw, 2u) << "only its original real profile counter advances";
  EXPECT_EQ(*NewRaw, 0u);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Wrapper->call(0, 3), wrapAotResult(g_wrap[0], 3));
  EXPECT_EQ(*OldRaw, 2u);
  EXPECT_EQ(*NewRaw, 1u);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u);
  EXPECT_EQ(g_aotCalls, 0u);
  EXPECT_EQ(g_cellCalls, 0u);
  const uint64_t Remaining = Host->runtime().sampleBudget() - 1;
  ASSERT_EQ(fillT1ThroughWrapper(1, 3, Remaining + 2), Remaining);
  ASSERT_TRUE(publishNow());
  EXPECT_GE(Host->ownerWorkerOperations(), 2u);
  EXPECT_EQ(Host->bundle()->sampleCount, 8u);
  EXPECT_EQ(Wrapper->call(2, 3), wrapAotResult(g_wrap[2], 3));
  EXPECT_EQ(g_aotCalls, 0u);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
  EXPECT_EQ(*OldRaw, 2u)
      << "new common T1/T2 traffic must never reuse the old ordinary counter";
  // The genuine return finally releases its own original token. Never infer
  // that cancellation/replacement released it on the call's behalf. There are
  // no old counter dereferences after this leave: only live calls require the
  // old allocation to survive, not arbitrary post-return test inspections.
  ejit_taskpool_release_read(OldBucket);
  OldLeaseHeld = false;
  EXPECT_EQ(Pool->state()->buckets[OldBucket].readers.loadAcquire(), 0u);
  outs() << "[PR231 live handoff] ordinary_active=1->0 old_counter=2"
         << " common_samples=" << Host->bundle()->sampleCount
         << " owner_worker_operations=" << Host->ownerWorkerOperations()
         << " old_read_token=held-through-handoff/real-release\n";
}

TEST_F(GeneratedWrapperTaskpoolTest,
       SettledOrdinaryPgoDoesNotStartPerCellSessionsForCommonPolicy) {
  // Base setup is cold, staged Async/PGO and genuinely activated. This one
  // control deliberately warms real ordinary PGO before installing Host.
  ASSERT_EQ(EJitSmallTableHost::global(), nullptr);
  ASSERT_TRUE(warmGenericPgo(/*Cell=*/0, /*X=*/2));
  ejit_taskpool_stats_t AfterWarmup{};
  ASSERT_EQ(ejit_taskpool_get_stats(&AfterWarmup), EJIT_OK);
  ASSERT_GE(AfterWarmup.asyncCompiles, 2u)
      << "the baseline warm-up must complete genuine T1 and T2 compilation";
  ASSERT_GE(AfterWarmup.readyEntries, 1u);
  ASSERT_EQ(AfterWarmup.pendingEntries, 0u);
  ASSERT_EQ(AfterWarmup.queueApproxSize, 0u);

  // Hooks ON, inline cache OFF. The common branch now bypasses ordinary
  // resolution completely; the warmed ordinary session is only a control.
  ASSERT_TRUE(makeResolvePathWrapper());
  ASSERT_TRUE(makeHost());
  ASSERT_TRUE(requestEntry());

  // The common T1 window is filled by the entry's own calls without starting
  // another unrelated ordinary per-cell instrumentation session.
  const uint64_t Budget = Host->runtime().sampleBudget();
  const int32_t X = 6;
  ASSERT_EQ(fillT1ThroughWrapper(/*Cell=*/0, X, Budget + 4), Budget)
      << "the entry's own calls did not fill the common T1 window";
  EXPECT_EQ(g_aotCalls, 0u);
  ASSERT_TRUE(publishNow());
  ASSERT_NE(Host->bundle(), nullptr);
  EXPECT_EQ(Host->bundle()->sampleCount, Budget);
  EXPECT_FALSE(Host->bundle()->profileData.empty());
  outs() << "[PR231 taskpool fixture] common host: samples="
         << Host->bundle()->sampleCount << " common T2 published\n";

  ejit_taskpool_stats_t AfterSampling{};
  ASSERT_EQ(ejit_taskpool_get_stats(&AfterSampling), EJIT_OK);
  EXPECT_EQ(AfterSampling.compileFailed, 0u);
  EXPECT_EQ(AfterSampling.asyncCompiles, AfterWarmup.asyncCompiles)
      << "common-T1 traffic must not compile ordinary per-cell objects";
  EXPECT_EQ(AfterSampling.asyncEnqueues, AfterWarmup.asyncEnqueues);
  EXPECT_EQ(AfterSampling.readyEntries, 0u)
      << "common takeover must retire this function's ordinary logical slots";
  EXPECT_EQ(AfterSampling.pendingEntries, 0u);
  EXPECT_EQ(AfterSampling.queueApproxSize, 0u);
  ASSERT_GE(Host->ownerWorkerOperations(), 2u)
      << "takeover T1 and common T2 must both use the actual owner worker";

  // Preserve the source contract; an AOT-only observation proves early common
  // admission really executes T2 rather than returning an equal fallback.
  const int32_t FrozenCell1 = wrapAotResult(WrapElement{1, 11}, X);

  ejit_taskpool_stats_t Before{};
  ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
  ASSERT_EQ(Before.compileFailed, 0u);

  // Cell 1 has NO ordinary compiled version. The first wrapper call still
  // reaches the common T2 under its own ticket, without generic resolution.
  EXPECT_EQ(Wrapper->call(1, X), FrozenCell1)
      << "a ready common member must not require an ordinary warmup";
  EXPECT_EQ(g_aotCalls, 0u);
  const unsigned BeforeAdmittedAot = g_aotCalls;
  EXPECT_EQ(Wrapper->call(1, X), FrozenCell1)
      << "the early common path did not execute the admitted published entry";
  EXPECT_EQ(Host->physicalExecutions(), 0u)
      << "the admitted common call did not leave its execution";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(g_aotCalls, BeforeAdmittedAot)
      << "common T2 must execute without AOT";

  ejit_taskpool_stats_t After{};
  ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.compileFailed, Before.compileFailed);
  EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles)
      << "common admission must bypass per-cell ordinary compilation";
  EXPECT_EQ(After.asyncEnqueues, Before.asyncEnqueues);
  EXPECT_EQ(After.readyEntries, Before.readyEntries);
  EXPECT_EQ(After.pendingEntries, 0u);
  outs() << "[PR231 taskpool fixture] cell1: worker compile delta="
         << After.asyncCompiles - Before.asyncCompiles
         << " early common-T2 enter/leave; common compile=real-owner-worker"
         << " owner_worker_operations=" << Host->ownerWorkerOperations()
         << " ordinary_ready=" << After.readyEntries << "\n";

  // The published code and the table are the same resource generation the T1
  // session used; repeated common calls do not create ordinary cache entries.
  EXPECT_EQ(Host->publishedResourceGeneration(),
            Host->runtime().resourceGeneration());
  ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
  EXPECT_EQ(Wrapper->call(1, X), FrozenCell1);
  ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles)
      << "a cached published coordinate must not recompile";
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(g_aotCalls, BeforeAdmittedAot);

  // The refusal edge with a live runtime: an unconfirmed member must not reach
  // the taskpool compile at all, and the wrapper keeps its AOT body.
  ASSERT_EQ(ejit_taskpool_get_stats(&Before), EJIT_OK);
  EXPECT_EQ(Wrapper->call(kWrapReadyCells, X),
            wrapAotResult(g_wrap[kWrapReadyCells], X))
      << "an unadmitted coordinate must stay AOT";
  EXPECT_EQ(g_aotCalls, BeforeAdmittedAot + 1);
  ASSERT_EQ(ejit_taskpool_get_stats(&After), EJIT_OK);
  EXPECT_EQ(After.asyncCompiles, Before.asyncCompiles)
      << "an unadmitted coordinate must not reach a taskpool compile";
  EXPECT_EQ(Host->physicalExecutions(), 0u);
}

#if defined(__linux__)
// Actual kernel tasks + actual native mappings, over the new POD control path.
// This is not an SRE SDK, not inter-core MMU/cache-coherence acceptance, and not
// a core-ID stand-in for worker identity. Every C command reaches the existing
// real Async worker, and every business call executes the pass's real wrapper.
std::atomic<bool> gBridgeDenyCallerMapping{false};
std::atomic<uint64_t> gBridgeCallerTask{0};
uint64_t bridgeLinuxTask(void *) { return static_cast<uint64_t>(syscall(SYS_gettid)); }
void bridgeLinuxDelay(void *, uint32_t) { std::this_thread::yield(); }
int bridgeLinuxMapping(void *, uintptr_t Address, uint64_t Bytes, uint32_t Access) {
  if (!Address || !Bytes || Bytes > UINTPTR_MAX - Address ||
      (gBridgeDenyCallerMapping.load() && bridgeLinuxTask(nullptr) == gBridgeCallerTask.load()))
    return -1;
  FILE *Maps = std::fopen("/proc/self/maps", "r");
  if (!Maps) return -1;
  uintptr_t Cursor = Address;
  const uintptr_t End = Address + Bytes;
  char Line[512], Permissions[5] = {};
  while (Cursor < End && std::fgets(Line, sizeof(Line), Maps)) {
    unsigned long long Lo = 0, Hi = 0;
    if (std::sscanf(Line, "%llx-%llx %4s", &Lo, &Hi, Permissions) != 3 ||
        Lo > Cursor || Hi <= Cursor) continue;
    if ((Access & EJIT_STAB_SRE_DATA_READ) && Permissions[0] != 'r') break;
    if ((Access & EJIT_STAB_SRE_DATA_WRITE) && Permissions[1] != 'w') break;
    Cursor = std::min<uintptr_t>(End, Hi);
  }
  std::fclose(Maps);
  return Cursor == End ? 0 : -1;
}

class GeneratedWrapperSreBridgeTest : public GeneratedWrapperTest {
protected:
  ejit_small_table_sre_source_state_t SourceState{};
  ejit_small_table_sre_request_t Request{};
  ejit_small_table_sre_snapshot_t Before{}, After{};
  WrapElement CompilerRows[6][2]{};
  int32_t CompilerOutput[6][2]{};
  std::unique_ptr<orc::LLJIT> CompilerImage;
  int32_t (*CompilerEntry)(int32_t, int32_t, int32_t) = nullptr;
  int32_t (*ObservationCount)() = nullptr;
  void (*volatile *ProbeDispatch)() = nullptr;
  void SetUp() override {
    ejit_shutdown();
    gBridgeDenyCallerMapping = false;
    gBridgeCallerTask = bridgeLinuxTask(nullptr);
    ejit_small_table_sre_bindings_t Bindings{};
    Bindings.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
    Bindings.structSize = sizeof(Bindings);
    Bindings.waitRounds = 65536;
    // Explicit worker context supplies authority. No SDK query is fabricated
    // from a core id, TLS, callback flag or diagnostic TaskCreate placeholder.
    Bindings.current_task_id = nullptr;
    Bindings.delay_ticks = bridgeLinuxDelay;
    Bindings.prepare_shared_data = bridgeLinuxMapping;
    ASSERT_EQ(ejit_small_table_sre_prepare(&Bindings), EJIT_STAB_SRE_OK);
    GeneratedWrapperTest::SetUp();
  }
  void TearDown() override {
    gBridgeDenyCallerMapping = false;
    GeneratedWrapperTest::TearDown();
  }
  bool buildWrapper(bool Icache = false, bool Reentrant = false,
                    const std::string *Override = nullptr) {
    std::string Body = Override ? *Override : Reentrant
        ? reentrantEntryBodyText("g_wrap", "g_wrap_out", kWrapCells)
        : entryBodyText("g_wrap", "g_wrap_out", kWrapCells);
    auto W = GeneratedWrapper::create(FuncIdx, CellSlot, true, Icache, &Body,
                                      /*SeedCacheHit=*/false);
    if (!W) { ADD_FAILURE() << toString(W.takeError()); return false; }
    Wrapper = std::move(*W);
    if (!Reentrant && !Icache && !Override) return true;
    ejit_shutdown();
    // Same registered body as the AOT image. Payload survives the joined worker.
    LLVMContext Ctx;
    auto M = parseModule(Ctx, Body, "SRE bridge actual body");
    if (!M) return false;
    SmallVector<char, 0> Buffer;
    raw_svector_ostream OS(Buffer);
    WriteBitcodeToFile(*M, OS);
    RegisteredBitcode.assign(Buffer.begin(), Buffer.end());
    ejit_register_period_array(kWrapPeriod, "g_wrap", &g_wrap[0].mode, sizeof(g_wrap));
    ejit_register_static_var("g_wrap", &g_wrap[0].mode);
    ejit_register_static_var("g_wrap_out", &g_wrap_out[0]);
    ejit_register_bitcode("f_entry", RegisteredBitcode.data(), RegisteredBitcode.size());
    if (Reentrant) ejit_register_symbol("wrap_observe", reinterpret_cast<void *>(&wrapObserve));
    if (Icache) ejit_register_icache_slot("f_entry", Wrapper->icacheSlot(), 1, nullptr);
    ejit_config_t Cfg{};
    Cfg.compileMode = EJIT_COMPILE_ASYNC; Cfg.optLevel = EJIT_OPT_L2;
    if (ejit_init_pgo(&Cfg) != EJIT_OK) return false;
    for (unsigned C = 0; C < kWrapReadyCells; ++C)
      if (ejit_activate(kWrapPeriod, C) != EJIT_OK) return false;
    return true;
  }
  bool buildActualCompilerRegistry2d(bool SingleTuCallback = false) {
    ejit_shutdown();
    for (unsigned C = 0; C < 6; ++C)
      for (unsigned T = 0; T < 2; ++T)
        CompilerRows[C][T] = {1, static_cast<int32_t>(10 + 2 * C + T)};
    std::string Body = SingleTuCallback
        ? singleTuCallbackBodyText("g_wrap", "g_wrap_out", kWrapCells)
        : entryBodyText("g_wrap", "g_wrap_out", kWrapCells);
    auto Replace = [&](StringRef From, StringRef To) {
      size_t Pos = 0;
      while ((Pos = Body.find(From.str(), Pos)) != std::string::npos) {
        Body.replace(Pos, From.size(), To.str()); Pos += To.size();
      }
    };
    Replace("@g_wrap = external global [4 x %A]",
            "@g_wrap = external global [6 x [2 x %A]], !ejit.metadata !6");
    Replace("[4 x i32]", "[12 x i32]");
    Replace("@f_entry(i32 %cell, i32 %x)",
            "@f_entry(i32 %cell, i32 %trp, i32 %x)");
    Replace("getelementptr inbounds [4 x %A], ptr @g_wrap, i64 0, i32 %cell",
            "getelementptr inbounds [6 x [2 x %A]], ptr @g_wrap, i64 0, i32 %cell, i32 %trp");
    Replace("      %op = getelementptr inbounds [12 x i32], ptr @g_wrap_out, i64 0, i32 %cell",
            "      %c2 = mul i32 %cell, 2\n      %linear = add i32 %c2, %trp\n"
            "      %op = getelementptr inbounds [12 x i32], ptr @g_wrap_out, i64 0, i32 %linear");
    Replace("!0 = !{!1, !2}", "!0 = !{!1, !2, !5}");
    Body += "    !5 = !{!\"ejit_period_arr_ind\", !\"bridge_trp\", i32 1}\n"
            "    !6 = !{!7}\n"
            "    !7 = !{!\"ejit_period_arr\", !\"tenant_cell\", i32 6}\n";
    // This is exactly the compiler's relevant shape: nested source arrays,
    // one OUTER period-array count, two parameter lifecycle dimensions and
    // genuine per-load may_const markers (no invented nested-global fallback).
    auto Context = std::make_unique<LLVMContext>();
    auto M = parseModule(*Context, Body, "actual compiler outer-element registry");
    if (!M || verifyModule(*M, &errs())) return false;
    SmallVector<char, 0> Buffer;
    raw_svector_ostream OS(Buffer);
    WriteBitcodeToFile(*M, OS);
    RegisteredBitcode.assign(Buffer.begin(), Buffer.end());
    const bool SavedCtors = EnableEJitGlobalCtors;
    const bool Hooks = EnableEJitSmallTableHooks, Icache = EJitInlineCache;
    auto RestoreOptions = make_scope_exit([&] {
      EnableEJitGlobalCtors = SavedCtors;
      EnableEJitSmallTableHooks = Hooks; EJitInlineCache = Icache;
    });
    EnableEJitGlobalCtors = false; EnableEJitSmallTableHooks = true; EJitInlineCache = false;
    Analyses A;
    if (SingleTuCallback) {
      // Run true PASS1 over the WHOLE single-TU image. Runtime input is the
      // exact embedded closure, not the unextracted test/controller module.
      ModulePassManager Extract;
      Extract.addPass(EJitRegisterBitcodePass());
      Extract.run(*M, A.MAM);
      LLVMContext PayloadContext;
      auto Payload = extractedRegistrationPayload(*M, PayloadContext);
      if (!Payload) { ADD_FAILURE() << toString(Payload.takeError()); return false; }
      if ((*Payload)->getFunction("pr231_probe_inflight") ||
          (*Payload)->getNamedGlobal("g_pr231_observer_calls") ||
          (*Payload)->getNamedGlobal("g_pr231_controller_state") ||
          (*Payload)->getFunction("test_ejit_period")) return false;
      auto *Slot = (*Payload)->getNamedGlobal("g_pr231_probe_dispatch");
      if (!Slot || !Slot->isDeclaration() || Slot->isConstant()) return false;
      auto *Embedded = cast<ConstantDataSequential>(
          M->getGlobalVariable(GV_EJIT_BITCODE, true)->getInitializer());
      StringRef Bytes = Embedded->getRawDataValues();
      RegisteredBitcode.assign(Bytes.bytes_begin(), Bytes.bytes_end());
    }
    ModulePassManager PM;
    PM.addPass(EJitRegisterPeriodPass());
    PM.run(*M, A.MAM);
    auto *Table = M->getGlobalVariable(".ejit.registry.period", true);
    if (!Table || !Table->hasInitializer()) return false;
    auto *Record = dyn_cast<ConstantStruct>(Table->getInitializer()->getAggregateElement(0u));
    if (!Record || cast<ConstantInt>(Record->getOperand(4))->getZExtValue() != 6)
      return false;
    if (M->getDataLayout().getTypeAllocSize(M->getNamedGlobal("g_wrap")->getValueType()) !=
        sizeof(CompilerRows)) return false;
    auto *F = M->getFunction("f_entry");
    IRBuilder<> Mark(&*F->getEntryBlock().getFirstInsertionPt());
    Mark.CreateCall(M->getOrInsertFunction("wrap_aot_observe", FunctionType::get(Mark.getVoidTy(), false)));
    runWrapperGen(*M, A);
    EnableEJitGlobalCtors = SavedCtors; EnableEJitSmallTableHooks = Hooks; EJitInlineCache = Icache;
    auto *Auto = M->getFunction(FN_AUTO_REGISTER);
    if (!Auto || verifyModule(*M, &errs())) return false;
    Auto->setLinkage(GlobalValue::ExternalLinkage);
    auto J = orc::LLJITBuilder().create();
    if (!J) { ADD_FAILURE() << toString(J.takeError()); return false; }
    CompilerImage = std::move(*J);
    orc::SymbolMap Symbols;
    auto Add = [&](StringRef Name, const void *Address) {
      Symbols[CompilerImage->getExecutionSession().intern(Name)] = orc::ExecutorSymbolDef(
          orc::ExecutorAddr::fromPtr(Address), JITSymbolFlags::Exported | JITSymbolFlags::Callable);
    };
    Add("g_wrap", CompilerRows); Add("g_wrap_out", CompilerOutput);
    Add("wrap_aot_observe", reinterpret_cast<const void *>(&wrapAotObserve));
    if (SingleTuCallback) {
      Add("wrap_observe", reinterpret_cast<const void *>(&wrapObserve));
      Add("ejit_register_bitcode", reinterpret_cast<const void *>(&ejit_register_bitcode));
      Add("ejit_register_symbol", reinterpret_cast<const void *>(&ejit_register_symbol));
    }
    Add("ejit_register_period_array", reinterpret_cast<const void *>(&ejit_register_period_array));
    Add("ejit_register_static_var", reinterpret_cast<const void *>(&ejit_register_static_var));
    Add("ejit_register_funcindex", reinterpret_cast<const void *>(&ejit_register_funcindex));
    Add("ejit_register_lifecycle", reinterpret_cast<const void *>(&ejit_register_lifecycle));
    Add("ejit_stab_wrapper_enter", reinterpret_cast<const void *>(&ejit_stab_wrapper_enter));
    Add("ejit_stab_wrapper_no_policy_current", reinterpret_cast<const void *>(&ejit_stab_wrapper_no_policy_current));
    Add("ejit_stab_leave", reinterpret_cast<const void *>(&ejit_stab_leave));
    Add("ejit_taskpool_compile_or_get_2d", reinterpret_cast<const void *>(&ejit_taskpool_compile_or_get_2d));
    Add("ejit_taskpool_release_read", reinterpret_cast<const void *>(&ejit_taskpool_release_read));
    if (Error E = CompilerImage->getMainJITDylib().define(orc::absoluteSymbols(std::move(Symbols)))) {
      ADD_FAILURE() << toString(std::move(E)); return false;
    }
    if (Error E = CompilerImage->addIRModule(orc::ThreadSafeModule(std::move(M), std::move(Context)))) {
      ADD_FAILURE() << toString(std::move(E)); return false;
    }
    auto AutoAddress = CompilerImage->lookup(FN_AUTO_REGISTER);
    if (!AutoAddress) { ADD_FAILURE() << toString(AutoAddress.takeError()); return false; }
    // EXECUTE the real pass-generated registration function. It registers the
    // same outer count as its static record, and fixes both real wrapper slots.
    reinterpret_cast<void (*)()>(AutoAddress->getValue())();
    auto Entry = CompilerImage->lookup("f_entry");
    if (!Entry) { ADD_FAILURE() << toString(Entry.takeError()); return false; }
    CompilerEntry = reinterpret_cast<int32_t (*)(int32_t, int32_t, int32_t)>(Entry->getValue());
    if (SingleTuCallback) {
      auto Count = CompilerImage->lookup("pr231_observation_count");
      if (!Count) { ADD_FAILURE() << toString(Count.takeError()); return false; }
      ObservationCount = reinterpret_cast<int32_t (*)()>(Count->getValue());
      auto Slot = CompilerImage->lookup("g_pr231_probe_dispatch");
      if (!Slot) { ADD_FAILURE() << toString(Slot.takeError()); return false; }
      ProbeDispatch = reinterpret_cast<void (*volatile *)()>(Slot->getValue());
      if (!ProbeDispatch || !*ProbeDispatch || ObservationCount() != 0) return false;
    }
    ejit_register_static_var("g_wrap", CompilerRows);
    ejit_register_static_var("g_wrap_out", CompilerOutput);
    if (!SingleTuCallback)
      ejit_register_bitcode("f_entry", RegisteredBitcode.data(), RegisteredBitcode.size());
    ejit_config_t Cfg{};
    Cfg.compileMode = EJIT_COMPILE_ASYNC; Cfg.optLevel = EJIT_OPT_L2;
    if (ejit_init_pgo(&Cfg) != EJIT_OK) return false;
    for (unsigned C = 0; C < 6; ++C) if (ejit_activate(kWrapPeriod, C) != EJIT_OK) return false;
    for (unsigned T = 0; T < 2; ++T) if (ejit_activate("bridge_trp", T) != EJIT_OK) return false;
    return true;
  }
  void requestCompiler2d(uint64_t Limit) {
    SourceState = {0xF00D, 1, 0, 0};
    Request = {};
    Request.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
    Request.structSize = sizeof(Request);
    std::strcpy(Request.entryName, "f_entry");
    std::strcpy(Request.sourceVarName, "g_wrap");
    Request.sourceAddress = reinterpret_cast<uintptr_t>(CompilerRows);
    Request.sourceBytes = sizeof(CompilerRows);
    Request.sourceState = reinterpret_cast<uintptr_t>(&SourceState);
    Request.aotEntry = reinterpret_cast<uintptr_t>(CompilerEntry);
    Request.sourceEpoch = SourceState.epoch;
    Request.configurationRevision = SourceState.revision;
    Request.codeGeneration = 1;
    Request.sampleLimit = Limit;
    Request.numDims = 2;
    Request.numMembers = 12;
    Request.domainCoverage = 1;
    Request.dims[0].argumentIndex = 0;
    Request.dims[0].extent = 6;
    Request.dims[1].argumentIndex = 1;
    Request.dims[1].extent = 2;
    std::strcpy(Request.dims[0].periodName, kWrapPeriod);
    std::strcpy(Request.dims[1].periodName, "bridge_trp");
    for (unsigned I = 0; I < 12; ++I) {
      Request.members[I].coordinate[0] = I / 2;
      Request.members[I].coordinate[1] = I % 2;
      Request.members[I].configurationGeneration = 1;
      Request.members[I].fieldsInitialized = 1;
    }
    ASSERT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_OK);
  }
  void singleTuCold(uint64_t Limit, bool WorkerSixCallerSixteen = false) {
    auto RestoreCore = make_scope_exit([] { EJitCoreId::resetForTest(); });
    if (WorkerSixCallerSixteen)
      EJitCoreId::setCurrentForTest(6);
    ASSERT_TRUE(buildActualCompilerRegistry2d(/*SingleTuCallback=*/true));
    if (WorkerSixCallerSixteen) {
      auto *Pool = EJitWrapperRuntimeTestAccess::pool();
      ASSERT_NE(Pool, nullptr);
      const auto Caller = std::this_thread::get_id();
      std::thread::id WorkerTask;
      auto Prepared = EJitSharedTaskPoolTestAccess::runControlOnOwnerAndWait(
          *Pool, [&](const llvm::ejit::detail::OwnerWorkerContext &Context) {
            ASSERT_TRUE(EJitSharedTaskPoolTestAccess::isCurrentOwnerWorker(
                *Pool, Context));
            WorkerTask = std::this_thread::get_id();
            // Simulate the product's 6/16 roles, not SDK task identity. The
            // capability still came from the real native worker-entry stack.
            EJitCoreId::setCurrentForTest(6);
          });
      ASSERT_EQ(Prepared.status,
                EJitSharedTaskPool::OwnerControlStatus::Completed);
      ASSERT_NE(WorkerTask, Caller);
      ASSERT_EQ(Pool->state()->ownerCoreId.loadAcquire(), 6u);
      EJitCoreId::setCurrentForTest(16);
    }
    requestCompiler2d(Limit);
    ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before),
              EJIT_STAB_SRE_OK);
    EXPECT_EQ(Before.workerTaskIdentity, 0u);
    EXPECT_NE(Before.ownerIdentity, 0u);
    EXPECT_GT(Before.ownerWorkerOperations, 0u);
    ASSERT_EQ(Before.admittedMembers, 12u);
    ASSERT_EQ(Before.genericAsyncEnqueues, 0u);
    EXPECT_EQ(ObservationCount(), 0);
    for (uint64_t I = 0; I < Limit; ++I) {
      const unsigned C = (I % 12) / 2, T = I % 2;
      EXPECT_EQ(CompilerEntry(C, T, 4), wrapAotResult(CompilerRows[C][T], 4));
      EXPECT_EQ(CompilerOutput[C][T], 4);
      EXPECT_EQ(ObservationCount(), static_cast<int32_t>(I + 1));
    }
    ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before),
              EJIT_STAB_SRE_OK);
    EXPECT_EQ(Before.sampleCount, Limit);
    EXPECT_EQ(Before.physicalExecutions, 0u);
    ASSERT_EQ(Before.counterPairs, 1u)
        << "the AOT controller/observer must not get private JIT PGO counters";
    ASSERT_EQ(Before.counterWordCount, 1u);
    EXPECT_EQ(Before.counts[0], Limit);
    EXPECT_EQ(g_aotCalls, 0u);
    EXPECT_EQ(CompilerEntry(5, 1, 4), wrapAotResult(CompilerRows[5][1], 4));
    EXPECT_EQ(g_aotCalls, 1u);
    EXPECT_EQ(ObservationCount(), static_cast<int32_t>(Limit + 1));
    ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After),
              EJIT_STAB_SRE_OK);
    EXPECT_EQ(After.countersDigest, Before.countersDigest);
    ASSERT_EQ(ejit_small_table_sre_finish(FuncIdx), EJIT_STAB_SRE_OK);
    ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After),
              EJIT_STAB_SRE_OK);
    EXPECT_EQ(After.tier, 2u);
    EXPECT_EQ(After.fullProfileValid, 1u);
    EXPECT_GT(After.profileBytes, 0u);
    EXPECT_EQ(After.rootEntryCountValid, 1u);
    EXPECT_EQ(After.rootEntryCount, Limit);
    EXPECT_EQ(After.counterPairs, After.expectedCounterPairs);
    EXPECT_EQ(After.countersDigest, Before.countersDigest);
    EXPECT_EQ(After.publishedSlots, 12u);
    EXPECT_EQ(After.borrowReaders, 0u);
    EXPECT_EQ(After.genericAsyncEnqueues, 0u);
    EXPECT_EQ(After.genericAsyncCompiles, 0u);
    EXPECT_EQ(After.genericPending, 0u);
    EXPECT_EQ(After.workerTaskIdentity, 0u);
    EXPECT_GT(After.ownerWorkerOperations, Before.ownerWorkerOperations);
    for (unsigned C = 0; C < 6; ++C)
      for (unsigned T = 0; T < 2; ++T) {
        EXPECT_EQ(CompilerEntry(C, T, 5), wrapAotResult(CompilerRows[C][T], 5));
        EXPECT_EQ(CompilerOutput[C][T], 5);
      }
    EXPECT_EQ(g_aotCalls, 1u);
    EXPECT_EQ(ObservationCount(), static_cast<int32_t>(Limit + 13));
    // The emitted T2 must reload the real mutable slot, not devirtualize its
    // original AOT initializer. Changing to another real same-image callback
    // modifies the SAME private observer object, without JIT/controller clones.
    auto Alternate = CompilerImage->lookup("pr231_probe_alternate");
    ASSERT_TRUE(static_cast<bool>(Alternate))
        << toString(Alternate.takeError());
    *ProbeDispatch = reinterpret_cast<void (*)()>(Alternate->getValue());
    EXPECT_EQ(CompilerEntry(0, 0, 6), wrapAotResult(CompilerRows[0][0], 6));
    EXPECT_EQ(ObservationCount(), static_cast<int32_t>(Limit + 23));
    EXPECT_EQ(g_aotCalls, 1u);
  }
  void request(uint64_t Limit = 64) {
    SourceState = {0xF00D, 1, 0, 0};
    Request = {};
    Request.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
    Request.structSize = sizeof(Request);
    std::strcpy(Request.entryName, "f_entry");
    std::strcpy(Request.sourceVarName, "g_wrap");
    Request.sourceAddress = reinterpret_cast<uintptr_t>(&g_wrap[0]);
    Request.sourceBytes = sizeof(g_wrap);
    Request.sourceState = reinterpret_cast<uintptr_t>(&SourceState);
    Request.aotEntry = reinterpret_cast<uintptr_t>(&wrapAotEntry);
    Request.sourceEpoch = SourceState.epoch;
    Request.configurationRevision = SourceState.revision;
    Request.codeGeneration = 1;
    Request.sampleLimit = Limit;
    Request.numDims = 1;
    Request.numMembers = kWrapReadyCells;
    Request.domainCoverage = 1;
    Request.dims[0].argumentIndex = 0;
    Request.dims[0].extent = kWrapCells;
    std::strcpy(Request.dims[0].periodName, kWrapPeriod);
    for (unsigned I = 0; I < kWrapReadyCells; ++I) {
      Request.members[I].coordinate[0] = I;
      Request.members[I].configurationGeneration = 1;
      Request.members[I].fieldsInitialized = 1;
    }
    ASSERT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_OK);
  }
  void cold(uint64_t Limit, bool Icache = false) {
    ASSERT_TRUE(buildWrapper(Icache)); request(Limit);
    ASSERT_EQ(ejit_small_table_sre_get_snapshot(UINT32_MAX, &Before), EJIT_STAB_SRE_OK);
    ASSERT_EQ(Before.funcIndex, FuncIdx);
    ASSERT_EQ(Before.workerTaskIdentity, 0u)
        << "SDK task id is unknown in the explicit-context path";
    ASSERT_NE(Before.ownerIdentity, 0u);
    ASSERT_GT(Before.ownerWorkerOperations, 0u);
    ASSERT_EQ(Before.tier, 1u);
    ASSERT_GT(Before.expectedCounterPairs, 0u);
    ASSERT_EQ(Before.counterPairs, Before.expectedCounterPairs);
    ASSERT_EQ(Before.genericAsyncEnqueues, 0u);
    for (uint64_t I = 0; I < Limit; ++I)
      ASSERT_EQ(Wrapper->call(I % kWrapReadyCells, 4),
                wrapAotResult(g_wrap[I % kWrapReadyCells], 4));
    ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_OK);
    EXPECT_EQ(Before.sampleCount, Limit);
    EXPECT_EQ(Before.rootEntryCount, 0u);
    EXPECT_EQ(Before.rootEntryCountValid, 0u);
    ASSERT_EQ(Before.counterWordCount, 1u);
    EXPECT_EQ(Before.counts[0], Limit);
    EXPECT_EQ(Before.physicalExecutions, 0u);
    EXPECT_EQ(g_aotCalls, 0u);
    EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
    EXPECT_EQ(g_aotCalls, 1u);
    ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
    EXPECT_EQ(After.countersDigest, Before.countersDigest);
    EXPECT_EQ(After.rootEntryCountValid, 0u);
    EXPECT_EQ(After.rootEntryCount, 0u);
    EXPECT_EQ(After.sampleCount, Limit);
    ASSERT_EQ(ejit_small_table_sre_finish(FuncIdx), EJIT_STAB_SRE_OK);
    ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
    EXPECT_EQ(After.tier, 2u);
    EXPECT_EQ(After.fullProfileValid, 1u);
    EXPECT_EQ(After.rootEntryCountValid, 1u);
    EXPECT_EQ(After.rootEntryCount, Limit);
    EXPECT_GT(After.profileBytes, 0u);
    EXPECT_EQ(After.counterPairs, After.expectedCounterPairs);
    EXPECT_EQ(After.countersDigest, Before.countersDigest);
    EXPECT_EQ(After.borrowReaders, 0u);
    for (unsigned I = 0; I < kWrapReadyCells; ++I)
      EXPECT_EQ(Wrapper->call(I, 5), wrapAotResult(g_wrap[I], 5));
    EXPECT_EQ(g_aotCalls, 1u);
    EXPECT_EQ(After.genericAsyncEnqueues, 0u);
    EXPECT_EQ(After.genericAsyncCompiles, 0u);
    EXPECT_EQ(After.genericPending, 0u);
  }
};
TEST_F(GeneratedWrapperSreBridgeTest, ColdRealWrapperFull64ProfileAndCommonT2) { cold(64); }
TEST_F(GeneratedWrapperSreBridgeTest, ColdRealWrapperConfigurable8ProfileAndCommonT2) { cold(8); }
TEST_F(GeneratedWrapperSreBridgeTest, ColdActualZeroIcacheFull64ProfileAndCommonT2) { cold(64, true); }
TEST_F(GeneratedWrapperSreBridgeTest,
       SingleTuActualPass1Cold64CompleteProfileAndCommonT2) {
  singleTuCold(64);
}
TEST_F(GeneratedWrapperSreBridgeTest,
       SingleTuActualPass1Configurable8CompleteProfileAndCommonT2) {
  singleTuCold(8);
}
TEST_F(GeneratedWrapperSreBridgeTest,
       SingleTuActualPass1WorkerSixCallerSixteenWithoutTaskIdCold64ToT2) {
  singleTuCold(64, /*WorkerSixCallerSixteen=*/true);
}
TEST_F(GeneratedWrapperSreBridgeTest,
       MissingSharedDataPermissionHookRemainsFailClosedWithoutTaskId) {
  ejit_small_table_sre_bindings_t Missing{};
  Missing.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
  Missing.structSize = sizeof(Missing);
  Missing.waitRounds = 65536;
  Missing.delay_ticks = bridgeLinuxDelay;
  Missing.current_task_id = nullptr;
  EXPECT_EQ(ejit_small_table_sre_prepare(&Missing), EJIT_STAB_SRE_BLOCKED)
      << "an unknown diagnostic task id never replaces real data preparation";
  // Failed preparation must not clobber the existing immutable, real mapping
  // binding. The normal cold path must still execute actual counters/profile.
  cold(8);
}
TEST_F(GeneratedWrapperSreBridgeTest,
       PublicPrepareCannotReplaceExistingUnknownTaskIdBinding) {
  ejit_small_table_sre_bindings_t Changed{};
  Changed.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
  Changed.structSize = sizeof(Changed);
  Changed.waitRounds = 65536;
  Changed.delay_ticks = bridgeLinuxDelay;
  Changed.prepare_shared_data = bridgeLinuxMapping;
  Changed.current_task_id = bridgeLinuxTask;
  EXPECT_EQ(ejit_small_table_sre_prepare(&Changed), EJIT_STAB_SRE_BUSY);
  cold(8);
}
TEST_F(GeneratedWrapperSreBridgeTest,
       SingleTuActualPass1LastCallCancelRetainsBorrowUntilRealLeave) {
  ASSERT_TRUE(buildActualCompilerRegistry2d(/*SingleTuCallback=*/true));
  requestCompiler2d(8);
  for (unsigned I = 0; I < 7; ++I) {
    const unsigned C = (I % 12) / 2, T = I % 2;
    EXPECT_EQ(CompilerEntry(C, T, 4), wrapAotResult(CompilerRows[C][T], 4));
  }
  ASSERT_EQ(ObservationCount(), 7);
  unsigned Held = 0;
  g_observe = [&] {
    ++Held;
    EXPECT_EQ(ObservationCount(), 8);
    EXPECT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_BUSY);
    EXPECT_EQ(Before.sampleCount, 8u);
    EXPECT_EQ(Before.physicalExecutions, 1u);
    EXPECT_GT(Before.borrowReaders, 0u);
    EXPECT_GT(SourceState.readers, 0u);
    EXPECT_EQ(ejit_small_table_sre_finish(FuncIdx), EJIT_STAB_SRE_BUSY);
    EXPECT_EQ(ejit_small_table_sre_cancel(FuncIdx), EJIT_STAB_SRE_OK);
    EXPECT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_BUSY);
    EXPECT_EQ(After.physicalExecutions, 1u);
    EXPECT_GT(After.borrowReaders, 0u);
    EXPECT_GT(SourceState.readers, 0u)
        << "cancel is not the final real return, even for the last sample";
  };
  EXPECT_EQ(CompilerEntry(3, 1, 4), wrapAotResult(CompilerRows[3][1], 4));
  g_observe = nullptr;
  EXPECT_EQ(Held, 1u);
  EXPECT_EQ(g_aotCalls, 0u);
  EXPECT_EQ(SourceState.readers, 0u);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.physicalExecutions, 0u);
  EXPECT_EQ(After.borrowReaders, 0u);
  EXPECT_EQ(After.genericAsyncEnqueues, 0u);

  SourceState.writerBlocked = 1;
  CompilerRows[3][1].bycell += 20;
  SourceState.epoch = 0xF00E;
  SourceState.revision = 2;
  SourceState.writerBlocked = 0;
  Request.sourceEpoch = SourceState.epoch;
  Request.configurationRevision = SourceState.revision;
  Request.codeGeneration = 2;
  for (unsigned I = 0; I < 12; ++I)
    Request.members[I].configurationGeneration = 2;
  ASSERT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_OK);
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_EQ(CompilerEntry(3, 1, 5), wrapAotResult(CompilerRows[3][1], 5));
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_OK);
  EXPECT_EQ(Before.sampleCount, 8u);
  ASSERT_EQ(Before.counterWordCount, 1u);
  EXPECT_EQ(Before.counts[0], 8u);
  ASSERT_EQ(ejit_small_table_sre_finish(FuncIdx), EJIT_STAB_SRE_OK);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.codeGeneration, 2u);
  EXPECT_EQ(After.tier, 2u);
  EXPECT_EQ(After.fullProfileValid, 1u);
  EXPECT_EQ(After.rootEntryCountValid, 1u);
  EXPECT_EQ(After.rootEntryCount, 8u);
  EXPECT_EQ(After.countersDigest, Before.countersDigest);
  EXPECT_EQ(CompilerEntry(3, 1, 6), wrapAotResult(CompilerRows[3][1], 6));
  EXPECT_EQ(ObservationCount(), 17);
  EXPECT_EQ(g_aotCalls, 0u);
}
TEST_F(GeneratedWrapperSreBridgeTest, ActualCompilerOuterCountSixByTwoCold64CompleteProfileAndT2) {
  ASSERT_TRUE(buildActualCompilerRegistry2d());
  SourceState = {0xF00D, 1, 0, 0};
  Request = {};
  Request.abiVersion = EJIT_STAB_SRE_ABI_VERSION; Request.structSize = sizeof(Request);
  std::strcpy(Request.entryName, "f_entry"); std::strcpy(Request.sourceVarName, "g_wrap");
  Request.sourceAddress = reinterpret_cast<uintptr_t>(CompilerRows);
  Request.sourceBytes = sizeof(CompilerRows);
  Request.sourceState = reinterpret_cast<uintptr_t>(&SourceState);
  Request.aotEntry = reinterpret_cast<uintptr_t>(CompilerEntry);
  Request.sourceEpoch = SourceState.epoch; Request.configurationRevision = SourceState.revision;
  Request.codeGeneration = 1; Request.sampleLimit = 64;
  Request.numDims = 2; Request.numMembers = 12; Request.domainCoverage = 1;
  Request.dims[0].argumentIndex = 0; Request.dims[0].extent = 6;
  Request.dims[1].argumentIndex = 1; Request.dims[1].extent = 2;
  std::strcpy(Request.dims[0].periodName, kWrapPeriod);
  std::strcpy(Request.dims[1].periodName, "bridge_trp");
  for (unsigned I = 0; I < 12; ++I) {
    Request.members[I].coordinate[0] = I / 2; Request.members[I].coordinate[1] = I % 2;
    Request.members[I].configurationGeneration = 1; Request.members[I].fieldsInitialized = 1;
  }
  // Bad byte requests cannot reinterpret a compiler row count as accessible
  // bytes, cannot over-read, and cannot install a partial common policy.
  --Request.sourceBytes;
  EXPECT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_INVALID);
  Request.sourceBytes += 2;
  EXPECT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_INVALID);
  --Request.sourceBytes;
  ASSERT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_OK);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_OK);
  EXPECT_EQ(Before.admittedMembers, 12u);
  for (unsigned I = 0; I < 64; ++I) {
    const unsigned C = (I % 12) / 2, T = I % 2;
    EXPECT_EQ(CompilerEntry(C, T, 4), wrapAotResult(CompilerRows[C][T], 4));
    EXPECT_EQ(CompilerOutput[C][T], 4);
  }
  EXPECT_EQ(g_aotCalls, 0u);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_OK);
  EXPECT_EQ(Before.sampleCount, 64u);
  EXPECT_EQ(Before.physicalExecutions, 0u);
  EXPECT_EQ(Before.rootEntryCountValid, 0u);
  ASSERT_EQ(Before.counterWordCount, 1u); EXPECT_EQ(Before.counts[0], 64u);
  EXPECT_EQ(CompilerEntry(5, 1, 4), wrapAotResult(CompilerRows[5][1], 4));
  EXPECT_EQ(g_aotCalls, 1u);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.countersDigest, Before.countersDigest);
  ASSERT_EQ(ejit_small_table_sre_finish(FuncIdx), EJIT_STAB_SRE_OK);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.tier, 2u); EXPECT_EQ(After.fullProfileValid, 1u);
  EXPECT_EQ(After.rootEntryCountValid, 1u); EXPECT_EQ(After.rootEntryCount, 64u);
  EXPECT_EQ(After.publishedSlots, 12u); EXPECT_EQ(After.borrowReaders, 0u);
  EXPECT_EQ(After.countersDigest, Before.countersDigest);
  EXPECT_EQ(After.genericAsyncEnqueues, 0u); EXPECT_EQ(After.genericPending, 0u);
  for (unsigned C = 0; C < 6; ++C)
    for (unsigned T = 0; T < 2; ++T) {
      EXPECT_EQ(CompilerEntry(C, T, 5), wrapAotResult(CompilerRows[C][T], 5));
      EXPECT_EQ(CompilerOutput[C][T], 5);
    }
  EXPECT_EQ(g_aotCalls, 1u);
}
TEST_F(GeneratedWrapperSreBridgeTest, BranchCountersAreNotEntryCountAndCompleteProfileFeedsT2) {
  std::string Body = entryBodyText("g_wrap", "g_wrap_out", kWrapCells);
  const std::string Needle = "      %xm = mul i32 %x, 3\n";
  ASSERT_NE(Body.find(Needle), std::string::npos);
  Body.replace(Body.find(Needle), Needle.size(),
      "      %positive = icmp sge i32 %x, 0\n"
      "      br i1 %positive, label %pos, label %neg\n"
      "    pos:\n"
      "      %pv = call i32 @bridge_positive(i32 %x)\n"
      "      br label %join\n"
      "    neg:\n"
      "      %nv = call i32 @bridge_negative(i32 %x)\n"
      "      br label %join\n"
      "    join:\n"
      "      %xm = phi i32 [ %pv, %pos ], [ %nv, %neg ]\n");
  Body += "\n    define internal i32 @bridge_positive(i32 %x) noinline {\n"
          "      %v = mul i32 %x, 3\n      ret i32 %v\n    }\n"
          "    define internal i32 @bridge_negative(i32 %x) noinline {\n"
          "      %v = mul i32 %x, 5\n      ret i32 %v\n    }\n";
  ASSERT_TRUE(buildWrapper(false, false, &Body)); request(64);
  for (unsigned I = 0; I < 64; ++I) {
    const int X = I % 2 ? -4 : 4;
    EXPECT_EQ(Wrapper->call(I % kWrapReadyCells, X),
              wrapAotResult(g_wrap[I % kWrapReadyCells], X) + (X < 0 ? 2 * X : 0));
  }
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_OK);
  ASSERT_EQ(Before.sampleCount, 64u);
  EXPECT_EQ(Before.rootEntryCountValid, 0u);
  EXPECT_EQ(Before.rootEntryCount, 0u);
  const ejit_small_table_sre_counter_t *Root = nullptr;
  for (uint32_t I = 0; I < Before.counterPairs; ++I)
    if (StringRef(Before.counters[I].name) == "f_entry") Root = &Before.counters[I];
  ASSERT_NE(Root, nullptr);
  ASSERT_EQ(Root->wordCount, 2u);
  EXPECT_EQ(Before.counts[Root->firstWord], 32u);
  EXPECT_EQ(Before.counts[Root->firstWord + 1], 32u);
  ASSERT_GE(Before.counterPairs, 3u);
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  EXPECT_EQ(g_aotCalls, 1u);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.countersDigest, Before.countersDigest);
  ASSERT_EQ(ejit_small_table_sre_finish(FuncIdx), EJIT_STAB_SRE_OK);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.tier, 2u);
  EXPECT_EQ(After.fullProfileValid, 1u);
  EXPECT_EQ(After.rootEntryCountValid, 1u);
  EXPECT_EQ(After.rootEntryCount, 64u);
  EXPECT_EQ(After.countersDigest, Before.countersDigest);
  for (unsigned I = 0; I < kWrapReadyCells; ++I)
    EXPECT_EQ(Wrapper->call(I, -5), wrapAotResult(g_wrap[I], -5) - 10);
  EXPECT_EQ(g_aotCalls, 1u);
  EXPECT_EQ(After.genericPending, 0u);
  EXPECT_EQ(After.genericAsyncEnqueues, 0u);
}
TEST_F(GeneratedWrapperSreBridgeTest, RefusedSourceBorrowClosesAllocatedPreparationPin) {
  ASSERT_TRUE(buildWrapper()); request(8);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_OK);
  SourceState.writerBlocked = 1;
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  SourceState.writerBlocked = 0;
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.sampleCount, 0u);
  EXPECT_EQ(After.physicalExecutions, 0u);
  EXPECT_EQ(After.borrowReaders, 0u);
  EXPECT_EQ(After.countersDigest, Before.countersDigest);
  EXPECT_EQ(After.genericAsyncEnqueues, 0u);
  EXPECT_EQ(g_aotCalls, 1u);
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.sampleCount, 1u);
  EXPECT_EQ(After.physicalExecutions, 0u);
  EXPECT_EQ(g_aotCalls, 1u);
}
TEST_F(GeneratedWrapperSreBridgeTest, RealCallerPermissionRefusalClosesPreparationWithoutSample) {
  ASSERT_TRUE(buildWrapper()); request();
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_OK);
  gBridgeDenyCallerMapping = true;
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  gBridgeDenyCallerMapping = false;
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.sampleCount, 0u);
  EXPECT_EQ(After.physicalExecutions, 0u);
  EXPECT_EQ(After.borrowReaders, 0u);
  EXPECT_EQ(After.countersDigest, Before.countersDigest);
  EXPECT_EQ(After.genericAsyncEnqueues, 0u);
  EXPECT_EQ(g_aotCalls, 1u);
}
TEST_F(GeneratedWrapperSreBridgeTest, CancelDuringRealCallRetainsBorrowUntilActualWrapperLeave) {
  ASSERT_TRUE(buildWrapper(false, true)); request(8);
  unsigned Observations = 0;
  g_observe = [&] {
    ++Observations;
    EXPECT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_BUSY);
    EXPECT_EQ(Before.physicalExecutions, 1u);
    EXPECT_GT(Before.borrowReaders, 0u);
    EXPECT_EQ(ejit_small_table_sre_finish(FuncIdx), EJIT_STAB_SRE_BUSY);
    EXPECT_EQ(ejit_small_table_sre_cancel(FuncIdx), EJIT_STAB_SRE_OK);
    EXPECT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_BUSY);
    EXPECT_EQ(After.physicalExecutions, 1u);
    EXPECT_GT(After.borrowReaders, 0u);
  };
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  g_observe = nullptr;
  EXPECT_EQ(Observations, 1u);
  EXPECT_EQ(SourceState.readers, 0u);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.physicalExecutions, 0u);
  EXPECT_EQ(After.borrowReaders, 0u);
  EXPECT_EQ(After.genericAsyncEnqueues, 0u);
  // Config writing only AFTER the actual leave; no mutation under old borrow.
  SourceState.writerBlocked = 1;
  g_wrap[0].bycell += 20;
  SourceState.epoch = 0xF00E; SourceState.revision = 2;
  SourceState.writerBlocked = 0;
  Request.sourceEpoch = SourceState.epoch;
  Request.configurationRevision = SourceState.revision;
  Request.codeGeneration = 2;
  for (unsigned I = 0; I < kWrapReadyCells; ++I)
    Request.members[I].configurationGeneration = 2;
  ASSERT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_OK);
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
}
TEST_F(GeneratedWrapperSreBridgeTest,
       ShutdownInsideRealFacadeOwnerJobJoinsReturnBeforeCallerReclaims) {
  ASSERT_TRUE(buildWrapper());
  request(8);
  auto *ActualHost = EJitSmallTableHost::global();
  auto *ActualPool = EJitWrapperRuntimeTestAccess::pool();
  ASSERT_NE(ActualHost, nullptr);
  ASSERT_NE(ActualPool, nullptr);
  const auto Caller = std::this_thread::get_id();
  std::thread::id Executed;
  bool ShutdownReturned = false;
  Error Result = runSmallTableOwnerRequest(
      FuncIdx, ActualHost,
      [&](const llvm::ejit::detail::OwnerWorkerContext &Context) -> Error {
        EXPECT_TRUE(EJitSharedTaskPoolTestAccess::isCurrentOwnerWorker(
            *ActualPool, Context));
        Executed = std::this_thread::get_id();
        EXPECT_NE(Executed, Caller);
        // This is the actual facade/owner handoff, not a raw pool test callback
        // that would bypass the runtime's owner-job lifetime pin.
        ejit_shutdown();
        ShutdownReturned = true;
        ejit_config_t Cfg{};
        Cfg.compileMode = EJIT_COMPILE_ASYNC;
        Cfg.optLevel = EJIT_OPT_L2;
        EXPECT_NE(ejit_init_pgo(&Cfg), EJIT_OK)
            << "the still-running owner job forbids facade replacement";
        return Error::success();
      },
      /*InitialHandoff=*/false);
  EXPECT_TRUE(static_cast<bool>(Result))
      << "shutdown invalidated the joined handoff's original runtime owner";
  if (Result)
    consumeError(std::move(Result));
  EXPECT_TRUE(ShutdownReturned) << "worker-side shutdown must not self-join";
  EXPECT_NE(Executed, Caller);
  EXPECT_EQ(SourceState.readers, 0u);
  // The old job has now really returned. Reclamation/join occurs from the
  // caller, then a fresh facade consumes the original payload and registration.
  ejit_shutdown();
  ejit_register_period_array(kWrapPeriod, "g_wrap", &g_wrap[0].mode,
                             sizeof(g_wrap));
  ejit_register_static_var("g_wrap", &g_wrap[0].mode);
  ejit_register_static_var("g_wrap_out", &g_wrap_out[0]);
  ejit_register_bitcode("f_entry", RegisteredBitcode.data(),
                        RegisteredBitcode.size());
  ejit_config_t Cfg{};
  Cfg.compileMode = EJIT_COMPILE_ASYNC;
  Cfg.optLevel = EJIT_OPT_L2;
  ASSERT_EQ(ejit_init_pgo(&Cfg), EJIT_OK);
  for (unsigned I = 0; I < kWrapReadyCells; ++I)
    ASSERT_EQ(ejit_activate(kWrapPeriod, I), EJIT_OK);
  EXPECT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After),
            EJIT_STAB_SRE_INVALID);
  Request.codeGeneration = 2;
  ASSERT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_OK);
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After),
            EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.sampleCount, 1u);
  EXPECT_EQ(After.workerTaskIdentity, 0u);
  EXPECT_NE(After.ownerIdentity, 0u);
  EXPECT_GT(After.ownerWorkerOperations, 0u);
}
TEST_F(GeneratedWrapperSreBridgeTest, ShutdownDuringRealCallPreservesLateLeaveAndReinitDropsOldControl) {
  ASSERT_TRUE(buildWrapper(false, true)); request(8);
  unsigned Observations = 0;
  g_observe = [&] {
    ++Observations;
    EXPECT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &Before), EJIT_STAB_SRE_BUSY);
    EXPECT_EQ(Before.physicalExecutions, 1u);
    EXPECT_GT(SourceState.readers, 0u);
    ejit_shutdown();
    EXPECT_GT(SourceState.readers, 0u) << "shutdown is not an actual leave";
    ejit_config_t Cfg{};
    Cfg.compileMode = EJIT_COMPILE_ASYNC; Cfg.optLevel = EJIT_OPT_L2;
    EXPECT_NE(ejit_init_pgo(&Cfg), EJIT_OK) << "old live code forbids runtime replacement";
    EXPECT_GT(SourceState.readers, 0u);
  };
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  g_observe = nullptr;
  EXPECT_EQ(Observations, 1u);
  EXPECT_EQ(SourceState.readers, 0u) << "exact late leave reached the still-pinned original worker";
  // Join/deallocate the retired facade from the caller, not its own worker.
  ejit_shutdown();
  // Runtime registration storage is consumed by init, not a persistent image
  // cache. Reinitialize from the SAME real body and symbols, as the product's
  // per-core registration/bootstrap does; do not weaken exact dense identity.
  ejit_register_period_array(kWrapPeriod, "g_wrap", &g_wrap[0].mode, sizeof(g_wrap));
  ejit_register_static_var("g_wrap", &g_wrap[0].mode);
  ejit_register_static_var("g_wrap_out", &g_wrap_out[0]);
  ejit_register_bitcode("f_entry", RegisteredBitcode.data(), RegisteredBitcode.size());
  ejit_register_symbol("wrap_observe", reinterpret_cast<void *>(&wrapObserve));
  ejit_config_t Cfg{};
  Cfg.compileMode = EJIT_COMPILE_ASYNC; Cfg.optLevel = EJIT_OPT_L2;
  ASSERT_EQ(ejit_init_pgo(&Cfg), EJIT_OK);
  for (unsigned I = 0; I < kWrapReadyCells; ++I)
    ASSERT_EQ(ejit_activate(kWrapPeriod, I), EJIT_OK);
  EXPECT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_INVALID);
  EXPECT_EQ(ejit_small_table_sre_finish(FuncIdx), EJIT_STAB_SRE_INVALID);
  EXPECT_EQ(ejit_small_table_sre_cancel(FuncIdx), EJIT_STAB_SRE_INVALID);
  Request.codeGeneration = 2;
  ASSERT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_OK);
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.sampleCount, 1u);
  EXPECT_EQ(After.physicalExecutions, 0u);
}
TEST_F(GeneratedWrapperSreBridgeTest, RepeatedOldTicketCannotCloseReplacementRealExecution) {
  ASSERT_TRUE(buildWrapper(false, true)); request(8);
  const ejit_dim_pair_t Dim{CellSlot, 0};
  uint64_t Old = 0, Epoch = 0;
  const char *Why = nullptr;
  void *Entry = ejit_stab_wrapper_enter(FuncIdx, &Dim, 1, nullptr, 0, &Old, &Why, &Epoch);
  ASSERT_NE(Entry, nullptr); ASSERT_NE(Old, 0u); ASSERT_EQ(Why, nullptr);
  auto Fn = reinterpret_cast<int32_t (*)(int32_t, int32_t)>(Entry);
  EXPECT_EQ(Fn(0, 4), wrapAotResult(g_wrap[0], 4));
  ASSERT_EQ(ejit_small_table_sre_cancel(FuncIdx), EJIT_STAB_SRE_OK);
  EXPECT_GT(SourceState.readers, 0u);
  ejit_stab_leave(Old);
  EXPECT_EQ(SourceState.readers, 0u);
  Request.codeGeneration = 2;
  ASSERT_EQ(ejit_small_table_sre_request(&Request), EJIT_STAB_SRE_OK);
  unsigned Observations = 0;
  g_observe = [&] {
    ++Observations;
    ejit_stab_leave(Old);
    ejit_stab_leave(Old);
    EXPECT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_BUSY);
    EXPECT_EQ(After.physicalExecutions, 1u);
    EXPECT_GT(After.borrowReaders, 0u);
    EXPECT_EQ(After.codeGeneration, 2u);
  };
  EXPECT_EQ(Wrapper->call(0, 4), wrapAotResult(g_wrap[0], 4));
  g_observe = nullptr;
  EXPECT_EQ(Observations, 1u);
  ASSERT_EQ(ejit_small_table_sre_get_snapshot(FuncIdx, &After), EJIT_STAB_SRE_OK);
  EXPECT_EQ(After.physicalExecutions, 0u);
  EXPECT_EQ(After.sampleCount, 1u);
}
TEST_F(GeneratedWrapperSreBridgeTest, BoundedConcurrentPreparationAndControlsAcrossShutdown) {
  ASSERT_TRUE(buildWrapper()); request(256);
  std::atomic<unsigned> Calls{0}, Controls{0};
  std::atomic<bool> Start{false};
  std::thread Caller([&] {
    while (!Start.load()) std::this_thread::yield();
    for (unsigned I = 0; I < 96; ++I) {
      const unsigned Cell = I % kWrapReadyCells;
      EXPECT_EQ(Wrapper->call(Cell, 4), wrapAotResult(g_wrap[Cell], 4));
      ++Calls;
    }
  });
  std::thread Controller([&] {
    while (!Start.load()) std::this_thread::yield();
    ejit_small_table_sre_snapshot_t S{};
    for (unsigned I = 0; I < 96; ++I) {
      const int R = ejit_small_table_sre_get_snapshot(FuncIdx, &S);
      EXPECT_TRUE(R == EJIT_STAB_SRE_OK || R == EJIT_STAB_SRE_BUSY ||
                  R == EJIT_STAB_SRE_BLOCKED || R == EJIT_STAB_SRE_INVALID);
      ++Controls;
    }
  });
  Start = true;
  const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while ((!Calls.load() || !Controls.load()) &&
         std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
  EXPECT_GT(Calls.load(), 0u);
  EXPECT_GT(Controls.load(), 0u);
  ejit_shutdown();
  Caller.join(); Controller.join();
  EXPECT_EQ(Calls.load(), 96u);
  EXPECT_EQ(Controls.load(), 96u);
  ejit_shutdown(); // finish retired-owner destruction on the non-worker
  EXPECT_EQ(SourceState.readers, 0u);
}
#endif // __linux__

} // namespace
