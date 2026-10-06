//===-- EJitSmallTableHostTest.cpp - normal-path integration tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// These tests exercise the NORMAL path, not the runtime's component API:
//
//   server facts -> EJitSmallTableHost::requestEntry  (application request)
//                -> EJitSmallTableHost::dispatch      (application dispatch)
//                -> EJitSmallTableHost::enter/leave   (wrapper hooks)
//
// Every value they compare against comes from the real AOT baseline the same
// call would have taken, and every table assertion is made against the resource
// the COMPILED code actually bound (engine lookup), never against a symbol name.
//
// The readiness fact source is the explicitly labeled host adapter
// (`EJitSmallTableHostFactSource`, label "host-adapter.pr231-not-product"), so
// nothing here is presented as product readiness. The product binding is a
// separate, recorded gate.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h" // the wrapper C ABI hooks
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableHost.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace llvm {
namespace ejit {
/// This access is deliberately private to the capture regressions. Neither
/// production callers nor the test fixture can replace an ORC lookup result.
struct EJitSmallTableCounterCaptureTestAccess {
  using BeforeCapture =
      std::function<void(EJitOrcEngine &, uint64_t, ArrayRef<std::string>)>;

  static void install(EJitSmallTableRuntime &Runtime, BeforeCapture Hook) {
    Runtime.beforeCounterCaptureForTesting_ = std::move(Hook);
  }
  static bool installed(const EJitSmallTableRuntime &Runtime) {
    return static_cast<bool>(Runtime.beforeCounterCaptureForTesting_);
  }
  static size_t capturedNames(const EJitSmallTableRuntime &Runtime) {
    return Runtime.counterNames_.size();
  }
  static size_t capturedAddresses(const EJitSmallTableRuntime &Runtime) {
    return Runtime.counterAddrs_.size();
  }
  static Error remove(EJitOrcEngine &Engine, uint64_t Generation,
                      StringRef Symbol) {
    return Engine.removeCounterSymbolForTesting(Generation, Symbol);
  }
};
} // namespace ejit
} // namespace llvm

namespace {

//===----------------------------------------------------------------------===//
// Fixtures
//===----------------------------------------------------------------------===//

/// The entry-class ceiling stated in the delivery plan (16 x 32). Used for
/// layout/stride checks; the entry MATRIX below uses a 6 x 20 window of it, so
/// no tiny fixture is treated as a product capacity.
constexpr unsigned kCeilingCells = 16;
constexpr unsigned kCeilingTrps = 32;
/// The entry matrix's window.
constexpr unsigned kMatrixWindowCells = 6;
constexpr unsigned kMatrixTrps = 20;
/// The focused fixture's window: one constant field, one per-axis field, one
/// joint field, plus a second constant field.
constexpr unsigned kCells = 4;
constexpr unsigned kTrps = 3;

struct alignas(4) HostElement {
  int32_t mode;   // offset 0: identical on the whole domain -> constant
  int32_t byCell; // offset 4: differs per cell only      -> axis 0 dropped
  int32_t byTrp;  // offset 8: differs per TRP only       -> axis 1 dropped
  int32_t joint;  // offset 12: differs jointly           -> both axes kept
};
static_assert(sizeof(HostElement) == 16, "layout must match the IR type");

/// The 16x32 ceiling fixture: several entries share it, exactly as a product
/// has several entries over one configured table.
HostElement g_matrix[kCeilingCells][kCeilingTrps];
int32_t g_matrix_out[kCeilingCells];

/// The focused fixture: the schema declares `kTrps + 1` TRPs while only `kTrps`
/// are confirmed ready, so a late member at TRP index `kTrps` is a real, readable
/// row of the declared index space (never an out-of-bounds read).
HostElement g_focus[kCells][kTrps + 1];
int32_t g_focus_out[kCells];
uint64_t g_profilePositiveCalls = 0;
uint64_t g_profileNegativeCalls = 0;

LLVM_ATTRIBUTE_NOINLINE int32_t hostProfilePositive(int32_t Value) {
  ++g_profilePositiveCalls;
  return Value + 17;
}

LLVM_ATTRIBUTE_NOINLINE int32_t hostProfileNegative(int32_t Value) {
  ++g_profileNegativeCalls;
  return Value - 23;
}

constexpr const char *kCellPeriod = "tenant_cell";
constexpr const char *kTrpPeriod = "tenant_trp";

void fillMatrixConfig() {
  for (unsigned C = 0; C < kCeilingCells; ++C)
    for (unsigned T = 0; T < kCeilingTrps; ++T) {
      HostElement &E = g_matrix[C][T];
      E.mode = 1;
      E.byCell = static_cast<int32_t>(100 + C);
      E.byTrp = static_cast<int32_t>(7 + T);
      E.joint = static_cast<int32_t>(C * 32 + T);
    }
  std::memset(g_matrix_out, 0, sizeof(g_matrix_out));
}

void fillFocusConfig() {
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T <= kTrps; ++T) {
      HostElement &E = g_focus[C][T];
      E.mode = 1;
      E.byCell = static_cast<int32_t>(7 + C);
      E.byTrp = static_cast<int32_t>(2 + T);
      E.joint = static_cast<int32_t>(C * 3 + T);
    }
  std::memset(g_focus_out, 0, sizeof(g_focus_out));
}

/// The real AOT baseline result of `eval`: this is what the baseline body of
/// the same entry computes without any specialization.
int32_t aotResult(const HostElement &E, int32_t X) {
  return E.mode * 1000 + E.byCell * 100 + E.byTrp * 10 + E.joint + X * 3;
}

/// The AOT baseline stand-in with the entry's exact signature: the pointer
/// `requestEntry` records as the AOT side of every decision.
int32_t aotFocusEntry(int32_t Cell, int32_t Trp, int32_t X) {
  return aotResult(g_focus[Cell][Trp], X);
}

/// The `target datalayout` / `target triple` header every module this suite
/// compiles must carry. The real engine compiles with detectHost()'s target
/// machine, and LLJIT rejects a module whose data layout differs from the JIT's
/// ("Added modules have incompatible data layouts"), so both are derived from
/// the same host target. Test-harness support, not product behavior.
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

/// The entry text: two dimension arguments (cell, trp), one dynamic argument
/// (x), four authorized may_const loads in one element.
std::string hostModuleText(StringRef Entry, StringRef Global, StringRef Out,
                           unsigned Cells, unsigned Trps,
                           bool BranchProfile = false) {
  const std::string CA = Twine(Cells).str();
  const std::string TA = Twine(Trps).str();
  std::string Text = hostTargetHeader();
  if (BranchProfile)
    Text += "declare i32 @host_profile_positive(i32)\n"
            "declare i32 @host_profile_negative(i32)\n";
  Text += "\n    %A = type { i32, i32, i32, i32 }\n";
  Text += "    @" + Global.str() + " = external global [" + CA + " x [" + TA +
          " x %A]]\n";
  Text += "    @" + Out.str() + " = external global [" + CA + " x i32]\n";
  Text += "\n    define i32 @" + Entry.str() +
          "(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {\n";
  Text += "    entry:\n";
  Text += "      %row = getelementptr inbounds [" + CA + " x [" + TA +
          " x %A]], ptr @" + Global.str() + ", i64 0, i32 %cell, i32 %trp\n";
  Text += "      %p0 = getelementptr inbounds %A, ptr %row, i32 0, i32 0\n";
  Text += "      %mode = load i32, ptr %p0, align 4, !ejit.may_const !1\n";
  Text += "      %p1 = getelementptr inbounds %A, ptr %row, i32 0, i32 1\n";
  Text += "      %bycell = load i32, ptr %p1, align 4, !ejit.may_const !1\n";
  Text += "      %p2 = getelementptr inbounds %A, ptr %row, i32 0, i32 2\n";
  Text += "      %bytrp = load i32, ptr %p2, align 4, !ejit.may_const !1\n";
  Text += "      %p3 = getelementptr inbounds %A, ptr %row, i32 0, i32 3\n";
  Text += "      %joint = load i32, ptr %p3, align 4, !ejit.may_const !1\n";
  Text += "      %outp = getelementptr inbounds [" + CA + " x i32], ptr @" +
          Out.str() + ", i64 0, i32 %cell\n";
  Text += "      store i32 %x, ptr %outp, align 4\n";
  Text += "      %m1 = mul i32 %mode, 1000\n";
  Text += "      %m2 = mul i32 %bycell, 100\n";
  Text += "      %m3 = mul i32 %bytrp, 10\n";
  Text += "      %s1 = add i32 %m1, %m2\n";
  Text += "      %s2 = add i32 %s1, %m3\n";
  Text += "      %s3 = add i32 %s2, %joint\n";
  Text += "      %xm = mul i32 %x, 3\n";
  Text += "      %sum = add i32 %s3, %xm\n";
  Text += "      %isone = icmp eq i32 %mode, 1\n";
  Text += "      %res = select i1 %isone, i32 %sum, i32 -1\n";
  if (BranchProfile)
    Text += "      %negative = icmp slt i32 %x, 0\n"
            "      br i1 %negative, label %neg, label %pos\n"
            "    neg:\n"
            "      %n = call i32 @profile_negative_inner(i32 %res)\n"
            "      ret i32 %n\n"
            "    pos:\n"
            "      %p = call i32 @profile_positive_inner(i32 %res)\n"
            "      ret i32 %p\n";
  else
    Text += "      ret i32 %res\n";
  Text += "    }\n\n";
  if (BranchProfile)
    Text += "define internal i32 @profile_positive_inner(i32 %v) noinline {\n"
            "entry:\n"
            "  %r = call i32 @host_profile_positive(i32 %v)\n"
            "  ret i32 %r\n"
            "}\n"
            "define internal i32 @profile_negative_inner(i32 %v) noinline {\n"
            "entry:\n"
            "  %r = call i32 @host_profile_negative(i32 %v)\n"
            "  ret i32 %r\n"
            "}\n";
  Text += "    !0 = !{!2}\n    !1 = !{}\n";
  Text += "    !2 = !{!\"ejit_entry\"}\n";
  return Text;
}

SmallVector<EJitSmallTableDim, 2> hostDims(unsigned Cells, unsigned Trps) {
  SmallVector<EJitSmallTableDim, 2> Dims;
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, Cells});
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 1, 0, Trps});
  return Dims;
}

/// The normal-path fixture: one labeled host fact source, one host, the
/// process-global registries a real start-up would have filled, and helpers that
/// drive the application API.
class SmallTableHostTest : public testing::Test {
protected:
  LLVMContext Ctx;
  EJitRuntimeState State;
  /// The engine keeps a pointer to the active context while compiling, so it
  /// must outlive every lookup.
  SpecializationContext compileCtx_;

  std::shared_ptr<EJitSmallTableHostFactSource> Facts;
  std::unique_ptr<EJitSmallTableHost> Host;
  /// The previous process-global host from `installGlobal`, restored on teardown
  /// so one test cannot leak a gate into another.
  EJitSmallTableHost *prevGlobal_ = nullptr;

  void SetUp() override {
    // Start every test from empty process-global registries: the wrapper's
    // dimType/funcIndex assignment is process-global and single-assignment, so a
    // previous test's names would otherwise pin slots here.
    EJitLifecycleRegistry::instance().reset();
    EJitFuncRegistry::instance().reset();
    prevGlobal_ = EJitSmallTableHost::installGlobal(nullptr);
    fillMatrixConfig();
    fillFocusConfig();
    g_profilePositiveCalls = 0;
    g_profileNegativeCalls = 0;
    // The entry's own external globals must be resolvable by the engine, which
    // reads them from this instance's registry (the AOT registration callbacks
    // fill it in the product). Registered once here: the engine resolves them
    // by name when a module is loaded.
    State.getRegistry().registerStaticVar("g_focus", &g_focus[0][0]);
    State.getRegistry().registerStaticVar("g_focus_out", &g_focus_out[0]);
    State.getRegistry().registerStaticVar("g_matrix", &g_matrix[0][0]);
    State.getRegistry().registerStaticVar("g_matrix_out", &g_matrix_out[0]);
  }

  void TearDown() override {
    if (Host) {
      if (EJitSmallTableHost::global() == Host.get())
        EJitSmallTableHost::installGlobal(nullptr);
      Host.reset();
    }
    EJitSmallTableHost::installGlobal(prevGlobal_);
    prevGlobal_ = nullptr;
  }

  /// Register the lifecycle names the wrapper would have registered and return
  /// the dimType slot the runtime resolves them to.
  uint32_t registerLifecycle(const char *Name) {
    return EJitLifecycleRegistry::instance().resolveAssign(Name);
  }

  /// An AOT baseline stand-in with the entry's exact signature. Every test
  /// compares the specialized result against this function's value.
  int32_t aotEntry(int32_t Cell, int32_t Trp, int32_t X) const {
    return aotFocusEntry(Cell, Trp, X);
  }

  std::unique_ptr<Module> parseHost(StringRef Entry, StringRef Global,
                                    StringRef Out, unsigned Cells,
                                    unsigned Trps,
                                    bool BranchProfile = false) {
    SMDiagnostic Err;
    auto M = parseAssemblyString(hostModuleText(Entry, Global, Out, Cells, Trps,
                                              BranchProfile),
                                 Err, Ctx);
    if (!M)
      Err.print("SmallTableHostTest", errs());
    return M;
  }

  std::shared_ptr<EJitSmallTableHostFactSource>
  makeFacts(const char *SourceVar, const void *Base, uint64_t Bytes,
            uint64_t Epoch, unsigned Cells, unsigned Trps,
            unsigned ReadyCells = kCells, unsigned ReadyTrps = kTrps) {
    auto F = std::make_shared<EJitSmallTableHostFactSource>(
        SourceVar, Base, Bytes, Epoch);
    for (unsigned C = 0; C < ReadyCells; ++C)
      for (unsigned T = 0; T < ReadyTrps; ++T)
        F->addReadyMember({C, T}, 0xA000 + C * 32 + T);
    return F;
  }

  /// Build the host through the NORMAL path: a labeled fact source, the
  /// instance's runtime state, and the process-global gate installed.
  bool makeHost(std::shared_ptr<EJitSmallTableHostFactSource> InFacts,
                EJitSmallTableHost::Options Opts = {}) {
    Facts = std::move(InFacts);
    Config Cfg;
    auto H = EJitSmallTableHost::create(Cfg, State.getRegistry(), State, Facts,
                                        Opts);
    if (!H) {
      ADD_FAILURE() << "host create failed: " << toString(H.takeError());
      return false;
    }
    Host = std::move(*H);
    EJitSmallTableHost::installGlobal(Host.get());
    return true;
  }

  /// Install the same retraction path a product wiring does: the runtime's
  /// shared dispatch-cache retirement.
  void installRetractionHook() {
    Host->setInvalidationHook([this]() { ++retractions_; });
  }
  unsigned retractions_ = 0;

  /// The full request path for the focused fixture. The module declares the
  /// fixture array's REAL shape (`kTrps + 1` TRPs, matching `g_focus`), so the
  /// planner's source strides are the array's own; \p ReadyTrps bounds only the
  /// confirmed-ready window inside that schema.
  bool requestFocusEntry(unsigned Cells = kCells, unsigned Trps = kTrps,
                         unsigned ReadyCells = kCells,
                         unsigned ReadyTrps = kTrps,
                         EJitSmallTableHost::Options Opts = {},
                         bool BranchProfile = false) {
    auto M = parseHost("f_entry", "g_focus", "g_focus_out", Cells, kTrps + 1,
                       BranchProfile);
    if (!M)
      return false;
    Modules.push_back(std::move(M));
    Facts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D,
                      Cells, Trps, ReadyCells, ReadyTrps);
    if (!makeHost(Facts, Opts))
      return false;
    if (BranchProfile) {
      Host->registerExtraSymbol("host_profile_positive",
                                reinterpret_cast<void *>(&hostProfilePositive));
      Host->registerExtraSymbol("host_profile_negative",
                                reinterpret_cast<void *>(&hostProfileNegative));
    }
    installRetractionHook();
    if (!Host->retractionAvailable()) {
      ADD_FAILURE() << "the owner invalidation hook was not installed";
      return false;
    }
    const uint32_t CellSlot = registerLifecycle(kCellPeriod);
    const uint32_t TrpSlot = registerLifecycle(kTrpPeriod);
    if (CellSlot == kEJitInvalidDimType || TrpSlot == kEJitInvalidDimType)
      return false;

    EJitSmallTableHost::EntryRequest Req;
    Req.module = Modules.front().get();
    Req.entryName = "f_entry";
    Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
    Req.sourceVarName = "g_focus";
    Dims = hostDims(Cells, kTrps + 1);
    Req.dims = Dims;
    Periods = {kCellPeriod, kTrpPeriod};
    Req.dimPeriodNames = Periods;
    Req.codeGeneration = 1;
    std::string Why;
    if (Error E = Host->requestEntry(
            Req, reinterpret_cast<void *>(&aotFocusEntry), Why)) {
      ADD_FAILURE() << "requestEntry failed: " << toString(std::move(E))
                    << " (" << Why << ")";
      return false;
    }
    return true;
  }

  /// Sampling window: real dispatched executions through enter/leave until the
  /// runtime's aggregate budget is reached, then freeze + T2 + publication.
  bool driveAndPublish(uint64_t MaxExecutions = 1000) {
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

  std::vector<std::unique_ptr<Module>> Modules;
  SmallVector<EJitSmallTableDim, 2> Dims;
  /// Keeps a request's dims object alive while the request is used (an ArrayRef
  /// must not point at a destroyed temporary).
  SmallVector<EJitSmallTableDim, 2> LocalDims;
  std::vector<std::string> Periods;

  uint32_t cellSlot() const {
    return EJitLifecycleRegistry::instance().lookup(kCellPeriod);
  }
  uint32_t trpSlot() const {
    return EJitLifecycleRegistry::instance().lookup(kTrpPeriod);
  }
  /// One real sampling-window execution through the ordinary entered/left
  /// protocol. During the initial window the entry runs on the INSTRUMENTED
  /// tier, which the product-facing `dispatch` deliberately never exposes (it
  /// requires a published code generation); `enter` is the same hook the AOT
  /// wrapper calls.
  bool sampleOnce(unsigned Cell, unsigned Trp, int64_t Arg, uint64_t *TicketOut) {
    const uint32_t D[2] = {cellSlot(), trpSlot()};
    const uint32_t I[2] = {Cell, Trp};
    std::string Why;
    void *Entry = Host->enterInstrumented(D, I, TicketOut, &Why);
    if (!Entry) {
      ADD_FAILURE() << "sampling entry refused: " << Why;
      return false;
    }
    if (TicketOut && *TicketOut == 0) {
      // A successful T1 admission always provides a completion ticket.
      ADD_FAILURE() << "callable T1 was admitted without physical protection";
      return false;
    }
    using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
    reinterpret_cast<EntryFn>(Entry)(Cell, Trp, Arg);
    Host->leave(*TicketOut);
    return true;
  }

  /// The declared dimensions of the focused fixture: the array's real shape,
  /// `kTrps + 1` TRPs, of which only `kTrps` are confirmed ready.
  SmallVector<EJitSmallTableDim, 2> focusDims(unsigned Cells = kCells) const {
    return hostDims(Cells, kTrps + 1);
  }

  /// The dimType slot each declared dimension was resolved to.
  uint32_t dimTypeOf(unsigned D) const { return Host->dimTypes()[D]; }

  /// Every admitted coordinate whose instance of dimension \p Dim equals
  /// \p Instance. Used to state lifecycle assertions over the real coordinate
  /// set instead of assuming a particular index.
  std::vector<std::vector<uint64_t>> coordinatesWith(unsigned Dim,
                                                     uint64_t Instance) const {
    std::vector<std::vector<uint64_t>> Out;
    for (const EJitSmallTableLogicalSlot &Slot : Host->slots())
      if (Dim < Slot.coordinate.size() && Slot.coordinate[Dim] == Instance)
        Out.push_back(Slot.coordinate);
    return Out;
  }

  /// One application call with the wrapper's real dim argument shape.
  EJitSmallTableDispatchResult call(uint32_t Cell, uint32_t Trp, int64_t X) {
    const uint32_t D[2] = {cellSlot(), trpSlot()};
    const uint32_t I[2] = {Cell, Trp};
    return Host->dispatch(D, I, X);
  }

  /// The REAL wrapper ABI (`ejit_stab_enter`), the same entry the generated
  /// dispatch calls: it resolves through the process-global host, gates on
  /// admission/publication and hands back a ticket. Used by the paused-execution
  /// tests so the lease under test is created by the production hook, not by a
  /// test-only call.
  void *wrapperEnter(uint32_t Cell, uint32_t Trp, uint64_t *Ticket) {
    ejit_dim_pair_t Pairs[2];
    Pairs[0].dimType = cellSlot();
    Pairs[0].instanceId = Cell;
    Pairs[1].dimType = trpSlot();
    Pairs[1].instanceId = Trp;
    const char *Why = nullptr;
    return ejit_stab_enter(Host->funcIndex(), Pairs, 2, Ticket, &Why);
  }

  /// Run one specialized call to COMPLETION: enter through the wrapper hook, call
  /// the returned entry with the real AOT signature, leave. Returns the value.
  int64_t runWrapperCall(uint32_t Cell, uint32_t Trp, int64_t X) {
    uint64_t Ticket = 0;
    void *Entry = wrapperEnter(Cell, Trp, &Ticket);
    if (!Entry)
      return std::numeric_limits<int64_t>::min();
    using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
    const int64_t Value =
        reinterpret_cast<EntryFn>(Entry)(Cell, Trp, X);
    ejit_stab_leave(Ticket);
    return Value;
  }

  /// Publish the NEXT resource generation through the ordinary path (prepare ->
  /// T1 -> real sampling window -> freeze -> T2 -> slots) for the given extra
  /// members. `False` with a GoogleTest failure on any refused step.
  bool driveNextGeneration(ArrayRef<EJitSmallTableRowKey> Extra) {
    std::string Why;
    if (Error E = Host->beginNextGeneration(Extra, Why)) {
      ADD_FAILURE() << "beginNextGeneration failed: " << toString(std::move(E))
                    << " (" << Why << ")";
      return false;
    }
    if (auto E = Host->compileT1(Why); !E) {
      ADD_FAILURE() << "compileT1 failed: " << toString(E.takeError())
                    << " (" << Why << ")";
      return false;
    }
    if (Host->driveSampling(4096, 3) == 0) {
      ADD_FAILURE() << "the sampling driver performed no real execution";
      return false;
    }
    if (Error E = Host->publishGeneration(Why)) {
      ADD_FAILURE() << "publishGeneration failed: " << toString(std::move(E))
                    << " (" << Why << ")";
      return false;
    }
    return true;
  }

  /// Fault injection is AFTER a genuine multi-function T1 materializes and
  /// BEFORE capture opens the sampling session. Erasing either real symbol of
  /// any sorted inventory pair must force capture to fail closed, regardless
  /// of how many preceding pairs had already been captured.
  /// Recovery recompiles the same source, then consumes a fresh full profile.
  void checkMissingCounterFailsClosedAndRecovers(StringRef Prefix,
                                                unsigned MissingIndex) {
    ASSERT_LT(MissingIndex, 3u);
    using Access = EJitSmallTableCounterCaptureTestAccess;
    auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1,
                       /*BranchProfile=*/true);
    ASSERT_NE(M, nullptr);
    Modules.push_back(std::move(M));
    auto InFacts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D,
                             kCells, kTrps);
    EJitSmallTableHost::Options Opts;
    Opts.runtime.sampling.freezeWaitMillis = 25;
    ASSERT_TRUE(makeHost(InFacts, Opts));
    Host->registerExtraSymbol("host_profile_positive",
                              reinterpret_cast<void *>(&hostProfilePositive));
    Host->registerExtraSymbol("host_profile_negative",
                              reinterpret_cast<void *>(&hostProfileNegative));
    installRetractionHook();
    ASSERT_TRUE(Host->retractionAvailable());
    ASSERT_NE(registerLifecycle(kCellPeriod), kEJitInvalidDimType);
    ASSERT_NE(registerLifecycle(kTrpPeriod), kEJitInvalidDimType);

    EJitSmallTableHost::EntryRequest Req;
    Req.module = Modules.front().get();
    Req.entryName = "f_entry";
    Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
    Req.sourceVarName = "g_focus";
    Dims = focusDims();
    Req.dims = Dims;
    Periods = {kCellPeriod, kTrpPeriod};
    Req.dimPeriodNames = Periods;
    Req.codeGeneration = 1;

    auto &Runtime = Host->runtime();
    const uint64_t SessionBefore = Runtime.sessionId();
    unsigned HookCalls = 0;
    bool Removed = false;
    std::vector<std::string> OriginalNames;
    std::vector<std::string> OriginalProfileNames;
    std::string MissingSymbol;
    Access::install(Runtime, [&](EJitOrcEngine &Engine, uint64_t Generation,
                                 ArrayRef<std::string> Names) {
      ++HookCalls;
      ASSERT_NE(Engine.getActiveContext(), nullptr);
      EXPECT_EQ(Engine.getActiveContext()->tier, CompileTier::Instrumented);
      ASSERT_EQ(Names.size(), 3u)
          << "this exact inventory is the root and both real helpers";
      OriginalNames.assign(Names.begin(), Names.end());
      ASSERT_EQ(std::set<std::string>(Names.begin(), Names.end()).size(),
                Names.size());
      // Prove these are genuine emitted pairs, not invented names or addresses.
      // No saved pointer is dereferenced after removing its lookup definition.
      for (const std::string &Name : Names) {
        StringRef ProfileName = Engine.getCounterProfileName(Name);
        ASSERT_FALSE(ProfileName.empty());
        OriginalProfileNames.push_back(ProfileName.str());
        for (StringRef PairPrefix : {"__profc_", "__profd_"}) {
          auto Address = Engine.lookup(Generation, PairPrefix.str() + Name);
          ASSERT_TRUE(static_cast<bool>(Address))
              << Name << ": " << toString(Address.takeError());
          ASSERT_NE(*Address, nullptr);
          if (PairPrefix == "__profd_") {
            const auto *Data =
                reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(
                    *Address);
            EXPECT_EQ(IndexedInstrProf::ComputeHash(ProfileName), Data->NameRef)
                << "canonical PGO name must identify the genuine emitted pair";
          }
        }
      }
      std::vector<std::string> SymbolNames(Names.begin(), Names.end());
      std::sort(SymbolNames.begin(), SymbolNames.end());
      MissingSymbol = Prefix.str() + SymbolNames[MissingIndex];
      Error RemoveError = Access::remove(Engine, Generation, MissingSymbol);
      ASSERT_FALSE(static_cast<bool>(RemoveError))
          << toString(std::move(RemoveError));
      Removed = true;
      auto Missing = Engine.lookup(Generation, MissingSymbol);
      ASSERT_FALSE(static_cast<bool>(Missing));
      const std::string OrcError = toString(Missing.takeError());
      EXPECT_NE(OrcError.find("Symbols not found"), std::string::npos)
          << OrcError;
      EXPECT_NE(OrcError.find(MissingSymbol), std::string::npos) << OrcError;
    });

    std::string Why;
    Error RequestError = Host->requestEntry(
        Req, reinterpret_cast<void *>(&aotFocusEntry), Why);
    ASSERT_TRUE(static_cast<bool>(RequestError))
        << "a missing real counter may not open a partial-profile session";
    const std::string Failure = toString(std::move(RequestError));
    ASSERT_EQ(HookCalls, 1u);
    ASSERT_TRUE(Removed);
    ASSERT_EQ(OriginalNames.size(), 3u);
    EXPECT_NE(Failure.find("incomplete common T1 profile counter capture"),
              std::string::npos) << Failure;
    EXPECT_NE(Failure.find(MissingSymbol), std::string::npos) << Failure;
    EXPECT_NE(Failure.find("Symbols not found"), std::string::npos) << Failure;
    EXPECT_EQ(Why, Failure);
    EXPECT_EQ(Runtime.cancellationReason(), Failure);
    EXPECT_EQ(Runtime.engine().getActiveContext(), nullptr);
    EXPECT_FALSE(Access::installed(Runtime)) << "injection must be one-shot";
    EXPECT_EQ(Access::capturedNames(Runtime), 0u);
    EXPECT_EQ(Access::capturedAddresses(Runtime), 0u);
    const auto EmittedNames = Runtime.engine().getLastCounterNames();
    EXPECT_EQ(std::vector<std::string>(EmittedNames.begin(), EmittedNames.end()),
              OriginalNames) << "capture failure must not shrink the expected set";
    EXPECT_EQ(Runtime.sessionId(), SessionBefore);
    EXPECT_FALSE(Runtime.sessionOpen());
    EXPECT_EQ(Runtime.currentSessionSamples(), 0u);
    EXPECT_EQ(Runtime.inFlight(), 0u);
    EXPECT_EQ(Runtime.physicalReaders(Runtime.resourceGeneration()), 0u);
    EXPECT_FALSE(Runtime.samplingProtected());
    EXPECT_GT(Facts->borrowCount(), 0u) << "planning and publication really borrowed";
    EXPECT_EQ(Facts->outstandingBorrows(), 0u);
    EXPECT_EQ(Runtime.bundle(), nullptr);
    EXPECT_EQ(Host->instrumentedEntry(), nullptr);
    EXPECT_EQ(Host->activeEntry(), nullptr);
    EXPECT_EQ(Host->publishedSlots(), 0u);
    EXPECT_FALSE(Host->codeReady());

    const auto Refused = call(0, 0, 4);
    EXPECT_EQ(Refused.status, EJitSmallTableDispatch::Aot) << Refused.why;
    EXPECT_FALSE(Refused.counted);
    EXPECT_EQ(Host->aotEntry(), reinterpret_cast<void *>(&aotFocusEntry));
    uint64_t Ticket = 99;
    EXPECT_EQ(wrapperEnter(0, 0, &Ticket), nullptr);
    EXPECT_EQ(Ticket, 0u);
    EXPECT_EQ(Host->physicalExecutions(), 0u);
    EXPECT_EQ(g_profilePositiveCalls, 0u);
    EXPECT_EQ(g_profileNegativeCalls, 0u);
    auto Frozen = Runtime.freeze(Why, /*Force=*/true);
    ASSERT_FALSE(static_cast<bool>(Frozen));
    EXPECT_NE(toString(Frozen.takeError()).find(MissingSymbol),
              std::string::npos);
    auto T2WithoutProfile = Runtime.compileCommonT2(Why);
    ASSERT_FALSE(static_cast<bool>(T2WithoutProfile));
    EXPECT_NE(toString(T2WithoutProfile.takeError()).find("without a frozen bundle"),
              std::string::npos);
    EXPECT_EQ(Runtime.engine().getActiveContext(), nullptr);

    // A fresh real materialization re-emits the removed definition. It is not
    // restored with an absolute/dummy symbol or by reducing the captured set.
    auto Recovered = Host->compileT1(Why);
    ASSERT_TRUE(static_cast<bool>(Recovered))
        << Why << ": " << toString(Recovered.takeError());
    ASSERT_NE(*Recovered, nullptr);
    EXPECT_EQ(HookCalls, 1u);
    EXPECT_EQ(Runtime.engine().getActiveContext(), nullptr);
    ASSERT_TRUE(Runtime.sessionOpen());
    EXPECT_EQ(Runtime.sessionId(), SessionBefore + 1);
    EXPECT_EQ(Runtime.currentSessionSamples(), 0u);
    EXPECT_EQ(Access::capturedNames(Runtime), OriginalNames.size());
    EXPECT_EQ(Access::capturedAddresses(Runtime), OriginalNames.size());
    auto Restored = Runtime.engine().lookup(Req.codeGeneration, MissingSymbol);
    ASSERT_TRUE(static_cast<bool>(Restored)) << toString(Restored.takeError());
    ASSERT_NE(*Restored, nullptr);
    const uint64_t Budget = Runtime.sampleBudget();
    ASSERT_EQ(Budget, 64u);
    for (uint64_t I = 0; I < Budget; ++I) {
      const unsigned Cell = I % kCells;
      const unsigned Trp = (I / kCells) % kTrps;
      const int32_t X = I % 2 ? -3 : 4;
      const auto Result = call(Cell, Trp, X);
      ASSERT_EQ(Result.status, EJitSmallTableDispatch::Dispatched) << Result.why;
      EXPECT_TRUE(Result.counted);
      EXPECT_EQ(Result.value,
                aotEntry(Cell, Trp, X) + (X < 0 ? -23 : 17));
    }
    EXPECT_EQ(Runtime.currentSessionSamples(), Budget);
    EXPECT_EQ(g_profilePositiveCalls, Budget / 2);
    EXPECT_EQ(g_profileNegativeCalls, Budget / 2);
    auto Complete = Runtime.freeze(Why);
    ASSERT_TRUE(static_cast<bool>(Complete))
        << Why << ": " << toString(Complete.takeError());
    const auto *Bundle = *Complete;
    ASSERT_NE(Bundle, nullptr);
    EXPECT_EQ(Bundle->sampleCount, Budget);
    EXPECT_EQ(Bundle->sessionId, SessionBefore + 1);
    EXPECT_EQ(Bundle->counters.size(), OriginalNames.size());
    auto Reader = InstrProfReader::create(
        MemoryBuffer::getMemBufferCopy(Bundle->profileData));
    ASSERT_TRUE(static_cast<bool>(Reader)) << toString(Reader.takeError());
    const std::set<std::string> ExpectedNames(OriginalProfileNames.begin(),
                                             OriginalProfileNames.end());
    ASSERT_EQ(ExpectedNames.size(), OriginalNames.size());
    std::set<std::string> CapturedNames;
    for (const auto &Counter : Bundle->counters) {
      ASSERT_NE(Counter.pgoName, nullptr);
      ASSERT_TRUE(CapturedNames.insert(Counter.pgoName).second);
    }
    EXPECT_EQ(CapturedNames, ExpectedNames);
    std::set<std::string> ProfileNames;
    bool SawRoot = false;
    for (const auto &Record : **Reader) {
      ASSERT_TRUE(ProfileNames.insert(Record.Name.str()).second);
      const PgoCounterRef *Counter = nullptr;
      for (const auto &Candidate : Bundle->counters)
        if (Record.Name == Candidate.pgoName)
          Counter = &Candidate;
      ASSERT_NE(Counter, nullptr);
      ASSERT_NE(Counter->profdAddr, 0u);
      ASSERT_NE(Counter->profcAddr, 0u);
      const auto *Data =
          reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(
              Counter->profdAddr);
      EXPECT_EQ(IndexedInstrProf::ComputeHash(Record.Name), Data->NameRef)
          << "profile lookup must use the canonical PGO function name, not "
             "the legalized counter-symbol suffix: " << Record.Name.str();
      ASSERT_EQ(Record.Hash, Data->FuncHash);
      ASSERT_EQ(Record.Counts.size(), Data->NumCounters);
      const auto *Counts = reinterpret_cast<const uint64_t *>(Counter->profcAddr);
      for (size_t I = 0; I < Record.Counts.size(); ++I)
        EXPECT_EQ(Record.Counts[I], Counts[I]) << Record.Name.str() << ": " << I;
      if (Record.Name == "f_entry") {
        SawRoot = true;
        ASSERT_GT(Record.Counts.size(), 1u);
      }
    }
    EXPECT_TRUE(SawRoot);
    EXPECT_FALSE((*Reader)->hasError());
    EXPECT_EQ(ProfileNames, ExpectedNames);

    // IR PGO numbers the non-MST edges, not a universal entry-counter slot.
    // In this two-path CFG slot zero is a branch edge (32), while PGOUse
    // reconstructs the real function entry count (64) from the full profile.
    // Replay the same plan/prefix and the ACTUAL bundle; do not infer the
    // entry count from an arbitrary counter or from the logical sample count.
    auto ProfileReplay = CloneModule(*Req.module);
    auto Plans = std::make_shared<EJitSmallTablePlanSet>();
    ASSERT_NE(Host->plan(), nullptr);
    Plans->add(std::make_shared<const EJitSmallTablePlan>(*Host->plan()));
    EJitOptimizer ProfileOptimizer(State.getRegistry());
    ProfileOptimizer.setSmallTablePlans(Plans);
    SpecializationContext ProfileContext;
    ProfileContext.fnName = Req.entryName;
    ProfileContext.cacheKey = Req.codeGeneration;
    ProfileContext.optLevel = Opts.runtime.optLevel;
    ProfileContext.tier = CompileTier::PGOUse;
    ProfileContext.profileData = Bundle->profileData;
    ProfileOptimizer.runPipeline(*ProfileReplay, ProfileContext);
    for (StringRef FunctionName : {"f_entry", "profile_positive_inner",
                                   "profile_negative_inner"}) {
      Function *F = ProfileReplay->getFunction(FunctionName);
      ASSERT_NE(F, nullptr) << FunctionName.str();
      auto EntryCount = F->getEntryCount();
      ASSERT_TRUE(EntryCount.has_value()) << FunctionName.str();
      EXPECT_EQ(EntryCount->getCount(),
                FunctionName == "f_entry" ? Budget : Budget / 2)
          << FunctionName.str();
    }
    EXPECT_EQ(Facts->outstandingBorrows(), 0u);
    ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why))) << Why;
    EXPECT_EQ(Host->activeTier(), "final");
    for (int32_t X : {-2, 3}) {
      const auto Result = call(0, 0, X);
      ASSERT_EQ(Result.status, EJitSmallTableDispatch::Dispatched) << Result.why;
      EXPECT_EQ(Result.value, aotEntry(0, 0, X) + (X < 0 ? -23 : 17));
    }
    EXPECT_EQ(Facts->outstandingBorrows(), 0u);
    EXPECT_EQ(Runtime.physicalInFlight(), 0u);
    EXPECT_EQ(Host->physicalExecutions(), 0u);
  }
};

//===----------------------------------------------------------------------===//
// 1. The default-off / unbound boundary
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, UnboundFunctionIndexNeverEntersTheFeature) {
  // No host installed at all: the wrapper hook refuses and the call takes AOT.
  ASSERT_EQ(EJitSmallTableHost::global(), nullptr);
  uint64_t Ticket = 12345;
  const char *Why = nullptr;
  const uint32_t D[2] = {cellSlot(), trpSlot()};
  EXPECT_EQ(ejit_stab_enter(0, reinterpret_cast<const ejit_dim_pair_t *>(D), 2,
                            &Ticket, &Why),
            nullptr);
  EXPECT_EQ(Ticket, 0u) << "an uninstalled hook must not hand out a ticket";
  ejit_stab_leave(0);
  EXPECT_FALSE(ejit_small_table_host_installed());
  EXPECT_EQ(ejit_small_table_published_slots(), 0u);
}

TEST_F(SmallTableHostTest, BoundEntrySamplesTheCommonT1UntilCodeIsReady) {
  // The plan exists and some members are admitted, but no FINAL code generation
  // is published yet. Two rules hold at once:
  //   * no FINAL entry is exposed: `wouldDispatch` stays false and the tier is
  //     the instrumented one, so publication is what gates the T2 entry;
  //   * an admitted member's ordinary call is a REAL sample of the ONE common T1
  //     window (spec §10) -- that is what fills the aggregate budget from
  //     business traffic -- while a coordinate outside the admitted set still
  //     takes the AOT path and consumes nothing.
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, /*ReadyCells=*/kCells - 1, kTrps));
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_EQ(Host->activeTier(), "instrumented");
  EXPECT_FALSE(Host->codeReady());

  const uint64_t Budget = Host->runtime().sampleBudget();
  ASSERT_LT(kTrps, Budget);
  for (unsigned T = 0; T < kTrps; ++T) {
    std::string Why;
    EXPECT_FALSE(Host->wouldDispatch({0, T}, &Why))
        << "no FINAL entry may be exposed before publication";
    const auto R = call(0, T, 2);
    EXPECT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
    EXPECT_TRUE(R.counted)
        << "an admitted pre-publication call is a real sample of the common T1";
    EXPECT_EQ(R.value, aotEntry(0, T, 2));
  }
  EXPECT_EQ(Host->runtime().currentSessionSamples(), kTrps)
      << "exactly the admitted members' real calls are counted";
  EXPECT_EQ(Host->physicalExecutions(), 0u)
      << "each sampling execution completed before the next one";

  // The unconfirmed member of the SAME declared schema still fails closed.
  const auto Refused = call(kCells - 1, 0, 2);
  EXPECT_EQ(Refused.status, EJitSmallTableDispatch::Aot) << Refused.why;
  EXPECT_FALSE(Refused.counted);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), kTrps)
      << "an AOT call never consumes the aggregate budget";
}

TEST_F(SmallTableHostTest, MissingRealProfcFailsClosedAndRecoversCompleteProfile) {
  checkMissingCounterFailsClosedAndRecovers("__profc_", /*MissingIndex=*/2);
}

TEST_F(SmallTableHostTest, MissingRealProfdFailsClosedAndRecoversCompleteProfile) {
  checkMissingCounterFailsClosedAndRecovers("__profd_", /*MissingIndex=*/2);
}

TEST_F(SmallTableHostTest,
       MissingFirstRealProfcFailsClosedAndRecoversCompleteProfile) {
  checkMissingCounterFailsClosedAndRecovers("__profc_", /*MissingIndex=*/0);
}

TEST_F(SmallTableHostTest,
       MissingFirstRealProfdFailsClosedAndRecoversCompleteProfile) {
  checkMissingCounterFailsClosedAndRecovers("__profd_", /*MissingIndex=*/0);
}

TEST_F(SmallTableHostTest,
       MissingSecondRealProfcFailsClosedAndRecoversCompleteProfile) {
  checkMissingCounterFailsClosedAndRecovers("__profc_", /*MissingIndex=*/1);
}

TEST_F(SmallTableHostTest,
       MissingSecondRealProfdFailsClosedAndRecoversCompleteProfile) {
  checkMissingCounterFailsClosedAndRecovers("__profd_", /*MissingIndex=*/1);
}

//===----------------------------------------------------------------------===//
// 2. The published path: constant / one-axis / two-axis fields
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, PublishedSlotsRunTheAxisPerFieldSpecialization) {
  ASSERT_TRUE(requestFocusEntry());
  ASSERT_TRUE(driveAndPublish());

  const EJitSmallTablePlan *Plan = Host->plan();
  ASSERT_NE(Plan, nullptr);
  // The automatic solver's per-field outcome on this fixture: one constant
  // field, two single-axis tables (each dropping the irrelevant axis), one
  // joint table.
  ASSERT_EQ(Plan->fields.size(), 4u);
  EXPECT_EQ(Plan->fields[0].strategy, EJitSmallTableStrategy::Uniform);
  EXPECT_EQ(Plan->fields[1].strategy, EJitSmallTableStrategy::Table);
  EXPECT_EQ(Plan->fields[1].retainedAxes.size(), 1u);
  EXPECT_EQ(Plan->fields[1].tableRows, kCells)
      << "byCell keeps the CELL axis only";
  EXPECT_EQ(Plan->fields[2].strategy, EJitSmallTableStrategy::Table);
  EXPECT_EQ(Plan->fields[2].retainedAxes.size(), 1u);
  EXPECT_EQ(Plan->fields[2].tableRows, kTrps + 1)
      << "byTrp keeps the TRP axis only, sized by the DECLARED extent";
  EXPECT_EQ(Plan->fields[3].strategy, EJitSmallTableStrategy::Table);
  EXPECT_EQ(Plan->fields[3].retainedAxes.size(), 2u);
  EXPECT_EQ(Plan->fields[3].tableRows, kCells * (kTrps + 1))
      << "joint keeps both axes";

  EXPECT_TRUE(Host->codeReady());
  EXPECT_EQ(Host->activeTier(), "final");
  EXPECT_EQ(Host->publishedSlots(), kCells * kTrps);

  // Real dispatched calls: the value comes from the compiled code, and every
  // value matches the AOT baseline the same call would have produced.
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T)
      for (int32_t X = -3; X <= 4; ++X) {
        const auto R = call(C, T, X);
        ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
        EXPECT_EQ(R.value, aotEntry(C, T, X))
            << "cell=" << C << " trp=" << T << " x=" << X;
      }
  // The dynamic store still happens: the entry writes x into the per-cell
  // output slot, so a wrongly specialized store would be visible here.
  EXPECT_EQ(g_focus_out[0], 4);
  EXPECT_EQ(g_focus_out[kCells - 1], 4);
}

TEST_F(SmallTableHostTest, ExactTableIdentityNotSymbolName) {
  ASSERT_TRUE(requestFocusEntry());
  ASSERT_TRUE(driveAndPublish());

  const auto *Resource = Host->runtime().resource();
  ASSERT_NE(Resource, nullptr);
  const EJitSmallTablePlan *Plan = Host->plan();
  ASSERT_NE(Plan, nullptr);

  // Every column the compiled T1/T2 code reads must resolve, through the
  // ENGINE, to this resource's own column address. A same-named foreign table
  // would resolve elsewhere.
  for (unsigned F = 0; F < Plan->fields.size(); ++F) {
    if (Plan->fields[F].strategy != EJitSmallTableStrategy::Table)
      continue;
    auto AddrOrErr = Host->runtime().engine().lookup(
        Host->publishedCodeGeneration(), Plan->fields[F].columnName);
    ASSERT_TRUE(static_cast<bool>(AddrOrErr))
        << Plan->fields[F].columnName << ": "
        << toString(AddrOrErr.takeError());
    EXPECT_EQ(*AddrOrErr, Resource->columnAddress(F))
        << "column " << Plan->fields[F].columnName
        << " did not resolve to this generation's resource";
  }

  // Both tiers bound the SAME resource identity, and the frozen bundle says so.
  const EJitSmallTableProfileBundle *B = Host->bundle();
  ASSERT_NE(B, nullptr);
  EXPECT_EQ(B->resourceAddress, reinterpret_cast<uintptr_t>(Resource->base()));
  EXPECT_EQ(B->resourceGeneration, Resource->generation());
  EXPECT_EQ(B->resourceGeneration, Host->publishedResourceGeneration());
  EXPECT_EQ(B->entryName, "f_entry");
  EXPECT_EQ(B->readinessProvider, EJitSmallTableHostFactSource::Label)
      << "the artifact must record that this is the HOST adapter, not product "
         "readiness";
  EXPECT_EQ(Host->runtime().providerLabel(), EJitSmallTableHostFactSource::Label);
}

//===----------------------------------------------------------------------===//
// 3. Readiness: ready / unready / delayed borrow, and stale epochs
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, OnlyConfirmedReadyMembersAreAdmitted) {
  // Cells 0..1 x all TRPs are confirmed ready; cells 2..3 are not, even though
  // they exist in the declared schema and are readable.
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, /*ReadyCells=*/2, kTrps));
  ASSERT_TRUE(driveAndPublish());

  EXPECT_EQ(Host->publishedSlots(), 2u * kTrps);
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T) {
      const auto R = call(C, T, 1);
      if (C < 2) {
        EXPECT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
        EXPECT_EQ(R.value, aotEntry(C, T, 1));
      } else {
        EXPECT_EQ(R.status, EJitSmallTableDispatch::Aot) << R.why;
        EXPECT_NE(R.why.find("never admitted"), std::string::npos) << R.why;
      }
    }
}

TEST_F(SmallTableHostTest, MissingOrStaleFactsNeverSpecialize) {
  auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1);
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> D = focusDims();
  std::vector<std::string> P = {kCellPeriod, kTrpPeriod};
  registerLifecycle(kCellPeriod);
  registerLifecycle(kTrpPeriod);

  // (a) No fact source: a host bound to an existing runtime refuses, because
  //     without the product's commit facts readiness cannot be proven.
  {
    Config Cfg;
    auto RT = EJitSmallTableRuntime::create(Cfg, State.getRegistry(), State,
                                            nullptr);
    ASSERT_TRUE(static_cast<bool>(RT));
    auto H = EJitSmallTableHost::create(**RT, nullptr,
                                        EJitSmallTableHost::Options{});
    ASSERT_FALSE(static_cast<bool>(H));
    const std::string Msg = toString(H.takeError());
    EXPECT_NE(Msg.find("no fact source"), std::string::npos) << Msg;
  }

  // (b) The configuration transaction is locked: the borrow is refused and
  //     nothing is planned.
  {
    auto F = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D, kCells,
                       kTrps);
    F->refuseBorrow("product configuration transaction unavailable");
    ASSERT_TRUE(makeHost(F));
    EJitSmallTableHost::EntryRequest Req;
    Req.module = M.get();
    Req.entryName = "f_entry";
    Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
    Req.sourceVarName = "g_focus";
    Req.dims = D;
    Req.dimPeriodNames = P;
    std::string Why;
    auto R = Host->planEntry(Req, Why);
    EXPECT_FALSE(static_cast<bool>(R));
    EXPECT_NE(Why.find("product configuration transaction unavailable"),
              std::string::npos)
        << Why;
    EXPECT_EQ(Host->plan(), nullptr);
    EXPECT_EQ(Host->publishedSlots(), 0u);
    consumeError(R.takeError());
  }
}

TEST_F(SmallTableHostTest, DelayedBorrowDoesNotPoisonTheSession) {
  auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1);
  ASSERT_TRUE(M);
  auto F = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D, kCells,
                     kTrps);
  // The product's configuration transaction is momentarily unavailable.
  F->refuseBorrow("configuration transaction busy");
  ASSERT_TRUE(makeHost(F));
  installRetractionHook();
  registerLifecycle(kCellPeriod);
  registerLifecycle(kTrpPeriod);

  EJitSmallTableHost::EntryRequest Req;
  Req.module = M.get();
  Req.entryName = "f_entry";
  Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
  Req.sourceVarName = "g_focus";
  LocalDims = focusDims();
  Req.dims = LocalDims;
  std::vector<std::string> P = {kCellPeriod, kTrpPeriod};
  Req.dimPeriodNames = P;
  std::string Why;
  auto First = Host->planEntry(Req, Why);
  EXPECT_FALSE(static_cast<bool>(First)) << "a refused borrow must fail closed";
  consumeError(First.takeError());

  // The transaction becomes available: the SAME host must now complete the
  // normal path (a transient refusal is not a permanent poisoning).
  F->allowBorrow();
  // planEntry refuses a second call on the same host only when it already
  // bound; this host is still unbound, so the retry is the normal request.
  auto Second = Host->planEntry(Req, Why);
  ASSERT_TRUE(static_cast<bool>(Second)) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;
  // A real dispatched T1 execution, one per admitted member to reach the
  // aggregate budget, then the published generation.
  ASSERT_TRUE(driveAndPublish());
  const auto R = call(1, 1, 3);
  ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
  EXPECT_EQ(R.value, aotEntry(1, 1, 3));
}

TEST_F(SmallTableHostTest, MovedConfigurationGenerationDrainsEverySlot) {
  ASSERT_TRUE(requestFocusEntry());
  ASSERT_TRUE(driveAndPublish());
  ASSERT_EQ(Host->publishedSlots(), kCells * kTrps);
  const unsigned Retractions = retractions_;
  EXPECT_TRUE(Host->retractionAvailable())
      << "the fixture installed the owner's invalidation hook";
  EXPECT_EQ(Host->activeExecutions(), 0u)
      << "every sampling execution must have been closed";
  EXPECT_EQ(Facts->outstandingBorrows(), 0u)
      << "every sampling execution must have released its protected read";
  EXPECT_EQ(Host->retractPublishedSlots(), static_cast<uint64_t>(kCells * kTrps));
  EXPECT_GT(retractions_, Retractions)
      << "the real invalidation path must run for the drained slots";
  // The generation change itself must also retract through the same path.
  EXPECT_GE(retractions_, Retractions + 1u);

  // The provider moves the configuration generation: the old facts stop being
  // current. Nothing published may survive that.
  Facts->invalidateGeneration();
  Host->noteConfigurationChange("configuration revision moved");
  EXPECT_GE(retractions_, Retractions + 1u)
      << "the real invalidation path must run for the drained slots";
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_EQ(Host->drainedSlotCount(), kCells * kTrps);

  const auto R = call(0, 0, 1);
  EXPECT_EQ(R.status, EJitSmallTableDispatch::Aot) << R.why;
  // The refused call ran the AOT baseline body, which stores its `x` argument.
  EXPECT_EQ(g_focus_out[0], 1) << "the refused call ran the AOT body";
}

//===----------------------------------------------------------------------===//
// 4. Aggregate sampling and the held execution
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, AggregateBudgetIsSharedAcrossMembersAndHoldsInFlight) {
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 5;
  Opts.runtime.sampling.freezeWaitMillis = 100;
  auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1);
  ASSERT_TRUE(M);
  Facts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D, kCells,
                    kTrps);
  ASSERT_TRUE(makeHost(Facts, Opts));
  registerLifecycle(kCellPeriod);
  registerLifecycle(kTrpPeriod);
  EJitSmallTableHost::EntryRequest Req;
  Req.module = M.get();
  Req.entryName = "f_entry";
  Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
  Req.sourceVarName = "g_focus";
  LocalDims = focusDims();
  Req.dims = LocalDims;
  std::vector<std::string> P = {kCellPeriod, kTrpPeriod};
  Req.dimPeriodNames = P;
  std::string Why;
  ASSERT_TRUE(static_cast<bool>(Host->planEntry(Req, Why))) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;

  // Four real samples spread over three DIFFERENT members: the budget is
  // aggregate across admitted members, not per member.
  const uint32_t SampleCells[4] = {0, 1, 3, 1};
  for (unsigned I = 0; I < 4; ++I) {
    uint64_t Ticket = 0;
    ASSERT_TRUE(sampleOnce(SampleCells[I], I % kTrps, 0, &Ticket));
    ASSERT_NE(Ticket, 0u);
    EXPECT_EQ(Host->activeExecutions(), 0u) << "the sample was closed";
    EXPECT_EQ(Host->runtime().currentSessionSamples(), I + 1);
  }
  EXPECT_TRUE(Host->samplingProtected())
      << "the sampling window holds the protected read borrow";

  // The fifth admitted execution is left IN FLIGHT: freeze must refuse rather
  // than read half a sample.
  {
    const uint32_t D[2] = {cellSlot(), trpSlot()};
    const uint32_t Inst[2] = {2, 2};
    uint64_t Ticket = 0;
    void *Entry = Host->enterInstrumented(D, Inst, &Ticket, &Why);
    ASSERT_NE(Entry, nullptr) << Why;
    ASSERT_NE(Ticket, 0u);
    EXPECT_EQ(Host->activeExecutions(), 1u);
    EXPECT_EQ(Host->runtime().inFlight(), 1u);
    std::string FreezeError;
    auto TooEarly = Host->runtime().freeze(FreezeError);
    EXPECT_FALSE(static_cast<bool>(TooEarly));
    EXPECT_NE(FreezeError.find("still in flight"), std::string::npos)
        << FreezeError;
    consumeError(TooEarly.takeError());
    Host->leave(Ticket);
  }
  EXPECT_EQ(Host->activeExecutions(), 0u);
  EXPECT_EQ(Host->runtime().inFlight(), 0u);
  EXPECT_TRUE(Host->runtime().samplingExhausted());

  ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why))) << Why;
  const EJitSmallTableProfileBundle *B = Host->bundle();
  ASSERT_NE(B, nullptr);
  EXPECT_EQ(B->sampleCount, 5u);
  EXPECT_EQ(B->participatingMembers, 5u)
      << "the bundle must show the real member spread, not one representative "
         "(the five sampled members are distinct: (0,0) (1,1) (3,2) (1,0) "
         "(2,2))";
  EXPECT_FALSE(B->profileData.empty()) << "the bundle carries a real profile";
  EXPECT_FALSE(B->counters.empty()) << "the bundle carries real counter refs";
  EXPECT_FALSE(Host->samplingProtected())
      << "freeze ends the sampling window and releases the protected read";

  // Once T2 is published, final-code executions run normally and consume no
  // further T1 quota. Quota-exhausted pre-publication calls instead use AOT.
  const auto R = call(2, 2, 4);
  ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
  EXPECT_FALSE(R.counted);
  EXPECT_EQ(R.value, aotEntry(2, 2, 4));
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 5u);
}

//===----------------------------------------------------------------------===//
// 5. Compatibility, conflicts, coalesced rebuild, migration
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, CompatibleLateMemberJoinsWithoutRestartingQuota) {
  // A 1-cell schema: `byCell` is uniform on the proven domain (one cell), so
  // `byTrp` and `joint` keep the TRP axis and every new TRP whose projection is
  // still unpublished can join the SAME generation without a rebuild.
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, /*ReadyCells=*/1,
                                /*ReadyTrps=*/3));
  ASSERT_TRUE(driveAndPublish());
  const uint64_t Session = Host->runtime().sessionId();
  const uint64_t SamplesBefore = Host->runtime().sampleCount();
  const uint64_t PublishedBefore = Host->publishedSlots();
  const uint64_t ResourceBefore = Host->publishedResourceGeneration();
  ASSERT_EQ(PublishedBefore, 3u) << "one cell x three TRPs";

  // The fourth TRP of cell 0 becomes ready in the SAME configuration
  // generation: its values are compatible with the exported contract, so it
  // joins the existing code generation.
  Facts->addReadyMember({0, 3}, 0xB000);
  std::string AdmitWhy;
  EXPECT_EQ(Host->runtime().admitMember({0, 3}, &AdmitWhy),
            EJitSmallTableAdmission::Extendable)
      << AdmitWhy;
  std::string Why;
  EXPECT_TRUE(Host->onProductActivated(kTrpPeriod, 3, &Why)) << Why;

  EXPECT_EQ(Host->runtime().sessionId(), Session)
      << "a compatible late member must not open a new session";
  EXPECT_EQ(Host->runtime().sampleCount(), SamplesBefore)
      << "a late member must not restart the aggregate quota";
  EXPECT_EQ(Host->publishedSlots(), PublishedBefore + 1);
  EXPECT_EQ(Host->publishedResourceGeneration(), ResourceBefore)
      << "a compatible member uses the SAME generation, no rebuild";
  const auto R = call(0, 3, 1);
  ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
  EXPECT_EQ(R.value, aotEntry(0, 3, 1));
  // The already-published members keep dispatching in the same generation.
  const auto R0 = call(0, 0, 1);
  ASSERT_EQ(R0.status, EJitSmallTableDispatch::Dispatched) << R0.why;
  EXPECT_EQ(R0.value, aotEntry(0, 0, 1));
}

TEST_F(SmallTableHostTest, ExtendableMemberJoinsTheLiveGenerationOnActivation) {
  // Cells 0..1 are the confirmed-ready members of a 4-cell schema, so the
  // `byCell` table was sized for the DECLARED extent and cell 2's projection is
  // a supported, not-yet-published coordinate: the contract calls it
  // Extendable, and the contract's capacity is what decides, not the size of
  // the ready set. Its activation therefore publishes it into the SAME
  // generation - no rebuild, no recompile.
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, /*ReadyCells=*/2, kTrps));
  ASSERT_TRUE(driveAndPublish());
  const uint64_t PublishedBefore = Host->publishedSlots();
  const uint64_t ResourceBefore = Host->publishedResourceGeneration();
  EXPECT_EQ(Host->slots().size(), PublishedBefore);

  Facts->addReadyMember({2, 0}, 0xB000);
  std::string AdmitWhy;
  ASSERT_EQ(Host->runtime().admitMember({2, 0}, &AdmitWhy),
            EJitSmallTableAdmission::Extendable)
      << AdmitWhy;
  std::string Why;
  EXPECT_TRUE(Host->onProductActivated(kCellPeriod, 2, &Why)) << Why;
  EXPECT_EQ(Host->publishedSlots(), PublishedBefore + 1)
      << "the extendable member joins the live generation";
  EXPECT_EQ(Host->publishedResourceGeneration(), ResourceBefore)
      << "no rebuild was needed";

  const auto R = call(2, 0, 1);
  ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
  EXPECT_EQ(R.value, aotEntry(2, 0, 1));
  // The already-published members are unaffected.
  const auto R1 = call(1, 1, 1);
  ASSERT_EQ(R1.status, EJitSmallTableDispatch::Dispatched) << R1.why;
  EXPECT_EQ(R1.value, aotEntry(1, 1, 1));
}

TEST_F(SmallTableHostTest, MemberOutsideTheDeclaredSchemaStaysAot) {
  // A coordinate the contract can never serve - one that leaves the declared
  // schema - is Unusable: it gets no slot, no activation can publish it, and the
  // ordinary dispatch keeps taking the baseline path.
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps));
  ASSERT_TRUE(driveAndPublish());
  const uint64_t PublishedBefore = Host->publishedSlots();
  const std::vector<uint64_t> Outside = {kCells + 3, 0};
  std::string AdmitWhy;
  EXPECT_EQ(Host->runtime().admitMember(Outside, &AdmitWhy),
            EJitSmallTableAdmission::Unusable)
      << AdmitWhy;
  EXPECT_EQ(Host->findSlot(Outside), nullptr)
      << "an unusable member has no logical slot";
  const auto R = Host->dispatchMember(Outside, 1);
  EXPECT_EQ(R.status, EJitSmallTableDispatch::CoordinateUnprovable)
      << "a coordinate outside the declared schema is not a member of it";
  EXPECT_EQ(Host->publishedSlots(), PublishedBefore);
}

TEST_F(SmallTableHostTest, ConflictingMemberBuildsOneCoalescedGenerationAndMigrates) {
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, /*ReadyCells=*/2, kTrps));
  ASSERT_TRUE(driveAndPublish());
  const uint64_t OldResourceGeneration = Host->publishedResourceGeneration();

  // A member OUTSIDE the proven projection: with cells 0..1 proven, `byCell`
  // keeps the CELL axis with two rows, and cell 2 extends that axis to a still
  // unpublished coordinate. The contract reports it as Extendable (a rebuild
  // can serve it), so it is NOT published by an activation that arrives while
  // the current generation is live.
  std::string OutsideWhy;
  ASSERT_EQ(Host->runtime().admitMember({2, 0}, &OutsideWhy),
            EJitSmallTableAdmission::Extendable)
      << OutsideWhy;
  const auto OutsideCall = call(2, 0, 1);
  EXPECT_EQ(OutsideCall.status, EJitSmallTableDispatch::Aot) << OutsideCall.why
      << "(the slot is not published: the current generation has no row for it)";

  // A CONFLICT: cell 0 IS inside the proven projection and its `mode` value
  // contradicts the published uniform contract. It must stay AOT rather than be
  // handed code that hard-codes another value, and nothing is republished.
  g_focus[0][0].mode = 999;
  std::string ConflictWhy;
  EXPECT_EQ(Host->runtime().admitMember({0, 0}, &ConflictWhy),
            EJitSmallTableAdmission::Conflict)
      << ConflictWhy;
  const auto ConflictCall = call(0, 0, 1);
  EXPECT_EQ(ConflictCall.status, EJitSmallTableDispatch::Aot) << ConflictCall.why;
  EXPECT_EQ(Host->publishedSlots(), kCells * kTrps / 2)
      << "nothing is republished for a conflicted member";
  g_focus[0][0].mode = 1;
  // The other members keep their published generation.
  const auto Old = call(1, 1, 1);
  ASSERT_EQ(Old.status, EJitSmallTableDispatch::Dispatched) << Old.why;
  EXPECT_EQ(Old.value, aotEntry(1, 1, 1));

  // The coalesced rebuild: a new resource generation over the union of the
  // members this host already admitted plus the new one, with the previous
  // resource retained for code that may still dispatch to it.
  SmallVector<EJitSmallTableRowKey, 4> Extra;
  for (unsigned T = 0; T < kTrps; ++T)
    Extra.push_back({{2, T}});
  std::string Why;
  // While the old code generation is published, the rebuild is refused: a
  // generation is not replaced under a reachable entry.
  Error Refused = Host->beginNextGeneration(Extra, Why);
  ASSERT_TRUE(static_cast<bool>(Refused)) << "the previous generation is live";
  EXPECT_FALSE(Why.empty());
  consumeError(std::move(Refused));
  Host->cancel("conflicting member: preparing a new generation");
  Facts->addReadyMember({2, 0}, 0xC000);
  Facts->addReadyMember({2, 1}, 0xC001);
  Facts->addReadyMember({2, 2}, 0xC002);
  ASSERT_FALSE(static_cast<bool>(Host->beginNextGeneration(Extra, Why))) << Why;
  EXPECT_GT(Host->runtime().resourceGeneration(), OldResourceGeneration);
  EXPECT_EQ(Host->runtime().stats().generationsPrepared, 1u)
      << "one coalesced rebuild, not one per member";
  EXPECT_GT(Host->runtime().stats().migratedRows, 0u)
      << "the still-valid members must be migrated into the new resource";
  EXPECT_GT(Host->runtime().retainedBytes(), 0u)
      << "the previous resource is retained, not freed";

  // Compile and publish the new generation, then re-validate every member.
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;
  EXPECT_TRUE(Host->driveSampling(4096, 1) > 0);
  ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why))) << Why;
  EXPECT_EQ(Host->publishedResourceGeneration(),
            Host->runtime().resourceGeneration());

  // The new member now dispatches with ITS value, and the old members migrated
  // to the same new generation.
  for (unsigned C = 0; C < 3; ++C)
    for (unsigned T = 0; T < kTrps; ++T) {
      const auto R = call(C, T, 2);
      ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
      EXPECT_EQ(R.value, aotEntry(C, T, 2))
          << "cell=" << C << " trp=" << T << " (migrated member value)";
    }

  // The rebuilt generation is what makes the lifecycle activation succeed:
  // admission alone is not publication.
  std::string Why2;
  EXPECT_TRUE(Host->onProductActivated(kCellPeriod, 2, &Why2)) << Why2;
  const auto Late = call(2, 0, 2);
  ASSERT_EQ(Late.status, EJitSmallTableDispatch::Dispatched) << Late.why;
  EXPECT_EQ(Late.value, aotEntry(2, 0, 2));
  EXPECT_GE(Host->runtime().stats().generationsPrepared, 1u)
      << "the coalesced rebuild prepared exactly one new generation";
}

TEST_F(SmallTableHostTest, RetireRefusesWhileAPublishedSlotStillReadsTheResource) {
  ASSERT_TRUE(requestFocusEntry());
  ASSERT_TRUE(driveAndPublish());
  std::string Why;
  EXPECT_FALSE(Host->retireGenerationsUpTo(Host->publishedResourceGeneration(),
                                           Why));
  EXPECT_NE(Why.find("still reads resource generation"), std::string::npos)
      << Why;
  // After the slots are drained the retirement is allowed.
  Host->cancel("drained for retirement");
  EXPECT_TRUE(Host->retireGenerationsUpTo(Host->publishedResourceGeneration(),
                                          Why))
      << Why;
}

//===----------------------------------------------------------------------===//
// 5b. PHYSICAL execution lifetime: the table survives a logical cancel, a
//     rebuild and a retirement until the real call returns (P1 repair).
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, PublishedExecutionKeepsItsGenerationThroughCancelRebuildAndRetire) {
  // The coordinator's source-derived counterexample, as a real paused execution:
  // Publish generation 1, enter through the wrapper ABI and PAUSE before leave;
  // cancel; rebuild to generation 2; ask to retire generation 1. The old table
  // must stay alive - the live call still reads its raw column addresses - and
  // the reclaim must happen only when that call really returns.
  ASSERT_TRUE(requestFocusEntry());
  ASSERT_TRUE(driveAndPublish());

  const uint64_t OldGen = Host->runtime().resourceGeneration();
  const uint64_t OldCodeGen = Host->publishedCodeGeneration();
  ASSERT_NE(OldGen, 0u);

  // A published slot the engine resolved, and the wrapper ABI entry that hands
  // out the ticket exactly as the generated dispatch does.
  const auto Live = call(1, 1, 5);
  ASSERT_EQ(Live.status, EJitSmallTableDispatch::Dispatched) << Live.why;
  EXPECT_EQ(Live.value, aotEntry(1, 1, 5));

  uint64_t Ticket = 0;
  void *Entry = wrapperEnter(1, 1, &Ticket);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Ticket, 0u);
  EXPECT_EQ(Entry, Host->activeEntry())
      << "the wrapper hook must hand back the published generation's entry";
  using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
  const int64_t PausedValue = reinterpret_cast<EntryFn>(Entry)(1, 1, 5);
  EXPECT_EQ(PausedValue, aotEntry(1, 1, 5))
      << "the paused execution really entered the specialized code";

  const uint64_t BorrowsWhileRunning = Facts->outstandingBorrows();
  EXPECT_GE(BorrowsWhileRunning, 1u)
      << "a published execution runs under its own protected read";
  EXPECT_EQ(Host->physicalExecutions(), 1u);
  EXPECT_EQ(Host->runtime().physicalReaders(OldGen), 1u);

  // The real return of the EARLIER dispatch already released its lease; the
  // paused one is the only reader left.
  Host->cancel("timeout while a published call is running");
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_EQ(Host->physicalExecutions(), 1u)
      << "cancel is logical: the running call keeps its generation";
  EXPECT_EQ(Host->logicallyClosedExecutions(), 1u);
  EXPECT_EQ(Host->runtime().physicalReaders(OldGen), 1u);
  EXPECT_EQ(Host->oldestExecutionGeneration(), OldGen);

  // A new generation may be prepared and compiled while the old call runs: the
  // rebuild is not blocked by a physical reader, and it must not disturb it.
  SmallVector<EJitSmallTableRowKey, 4> Extra;
  for (unsigned T = 0; T < kTrps; ++T)
    Extra.push_back({{2, T}});
  Facts->addReadyMember({2, 0}, 0xC000);
  Facts->addReadyMember({2, 1}, 0xC001);
  Facts->addReadyMember({2, 2}, 0xC002);

  std::string Why;
  Error Rebuild = Host->beginNextGeneration(Extra, Why);
  if (Rebuild) {
    ADD_FAILURE() << "beginNextGeneration refused while only a PUBLISHED call "
                     "of the old generation is running: "
                  << toString(std::move(Rebuild)) << " (" << Why << ")";
    consumeError(std::move(Rebuild));
    return;
  }
  const uint64_t NewGen = Host->runtime().resourceGeneration();
  ASSERT_GT(NewGen, OldGen) << "a new resource generation is adopted";
  EXPECT_EQ(Host->runtime().physicalReaders(OldGen), 1u)
      << "adopting a new generation must not release the old reader";
  EXPECT_GT(Host->runtime().retainedBytes(), 0u)
      << "the old generation is retained while its reader runs";
  EXPECT_GT(Facts->outstandingBorrows(), 0u)
      << "the paused call's protected read is still held after the rebuild";

  // Ask to retire the OLD generation while the call is still inside it. The
  // retirement is accepted but DEFERRED: nothing may be freed yet.
  const uint64_t RetainedBefore = Host->runtime().retainedGenerationCount();
  ASSERT_TRUE(Host->retireGenerationsUpTo(OldGen, Why)) << Why;
  EXPECT_TRUE(Host->hasRetiredExecutions())
      << "the retirement waits for the physical reader";
  EXPECT_EQ(Host->retiredExecutionGeneration(), OldGen);
  EXPECT_EQ(Host->runtime().physicalReaders(OldGen), 1u)
      << "the generation is still physically read";
  EXPECT_GT(Host->runtime().pendingRetireGenerationCount(), 0u)
      << "the retirement is recorded as pending, not performed";
  EXPECT_EQ(Host->runtime().retainedGenerationCount(), RetainedBefore)
      << "no retained generation may be dropped under a running call";
  EXPECT_EQ(Host->runtime().stats().retiredGenerations, 0u)
      << "nothing was freed while the call was still reading the table";
  EXPECT_GE(Host->runtime().stats().deferredRetirements, 1u);

  // The real return releases exactly this execution's lease and completes the
  // deferred reclaim.
  const uint64_t BytesBeforeLeave = Host->runtime().pendingRetireBytes();
  EXPECT_GT(BytesBeforeLeave, 0u);
  ejit_stab_leave(Ticket);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(OldGen), 0u);
  EXPECT_FALSE(Host->hasRetiredExecutions());
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 0u)
      << "the deferred retirement was reclaimed at the real return";
  EXPECT_GE(Host->runtime().stats().reclaimedAfterReaders, 1u)
      << "the safe reclamation is recorded, not silent";
  EXPECT_LT(Host->runtime().retainedGenerationCount(), RetainedBefore)
      << "the old generation's storage is released now that no one reads it";
  EXPECT_EQ(Host->oldestExecutionGeneration(), 0u)
      << "the late leave must not settle into the replacement session";

  // The replacement session is untouched by the late completion, and it works.
  EXPECT_EQ(Host->runtime().resourceGeneration(), NewGen);
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;
  EXPECT_TRUE(Host->driveSampling(4096, 7) > 0);
  ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why))) << Why;
  EXPECT_EQ(Host->publishedResourceGeneration(), NewGen);
  EXPECT_NE(Host->publishedCodeGeneration(), OldCodeGen)
      << "the replacement is a distinct code generation";
  const auto NewCall = call(2, 0, 5);
  ASSERT_EQ(NewCall.status, EJitSmallTableDispatch::Dispatched) << NewCall.why;
  EXPECT_EQ(NewCall.value, aotEntry(2, 0, 5))
      << "the replacement session's values come from its own resource";
  EXPECT_EQ(Host->staleLeaveCount(), 1u)
      << "exactly the late completion is counted stale, not the new session";
}

TEST_F(SmallTableHostTest, SamplingExecutionKeepsItsResourceAndBorrowThroughCancelRebuild) {
  // The T1 half of the same rule: a real sampling execution is paused inside the
  // instrumented entry, the session is cancelled and a new generation is
  // prepared. The runtime's in-flight count and the window's protected read are
  // physical and must survive until the execution returns.
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 4;
  Opts.runtime.sampling.freezeWaitMillis = 100;
  auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1);
  ASSERT_TRUE(M);
  Facts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D, kCells,
                    kTrps);
  ASSERT_TRUE(makeHost(Facts, Opts));
  installRetractionHook();
  registerLifecycle(kCellPeriod);
  registerLifecycle(kTrpPeriod);
  EJitSmallTableHost::EntryRequest Req;
  Req.module = M.get();
  Req.entryName = "f_entry";
  Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
  Req.sourceVarName = "g_focus";
  LocalDims = focusDims();
  Req.dims = LocalDims;
  std::vector<std::string> P = {kCellPeriod, kTrpPeriod};
  Req.dimPeriodNames = P;
  std::string Why;
  ASSERT_TRUE(static_cast<bool>(Host->planEntry(Req, Why))) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;

  const uint64_t Gen = Host->runtime().resourceGeneration();
  ASSERT_NE(Gen, 0u);
  const uint32_t D[2] = {cellSlot(), trpSlot()};
  const uint32_t I[2] = {0, 0};
  uint64_t Ticket = 0;
  void *Entry = Host->enterInstrumented(D, I, &Ticket, &Why);
  ASSERT_NE(Entry, nullptr) << Why;
  ASSERT_NE(Ticket, 0u);
  // The real instrumented execution is entered but NOT left: it is paused inside
  // the sampling window, exactly where a cancel would previously have zeroed the
  // runtime's in-flight count and released the window's read.
  using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
  EXPECT_EQ(reinterpret_cast<EntryFn>(Entry)(0, 0, 4), aotEntry(0, 0, 4));
  EXPECT_EQ(Host->runtime().inFlight(), 1u);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
  EXPECT_TRUE(Host->runtime().samplingProtected());
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);

  Host->cancel("timeout with a sampling execution paused");
  EXPECT_EQ(Host->runtime().inFlight(), 1u)
      << "the runtime's in-flight count is physical and survives cancel";
  EXPECT_EQ(Host->runtime().sessionInFlight(), 0u)
      << "the cancelled session gave up its sample accounting";
  EXPECT_TRUE(Host->runtime().samplingProtected())
      << "the window's protected read guards the paused execution";
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u);
  EXPECT_EQ(Host->physicalExecutions(), 1u);
  EXPECT_GE(Facts->outstandingBorrows(), 1u);

  // A new generation may still be prepared: the cancelled session's sample is
  // not a barrier, but its RESOURCE stays retained while it runs.
  SmallVector<EJitSmallTableRowKey, 4> Extra;
  for (unsigned T = 0; T < kTrps; ++T)
    Extra.push_back({{2, T}});
  Facts->addReadyMember({2, 0}, 0xD000);
  Facts->addReadyMember({2, 1}, 0xD001);
  Facts->addReadyMember({2, 2}, 0xD002);
  Error Rebuild = Host->beginNextGeneration(Extra, Why);
  ASSERT_FALSE(static_cast<bool>(Rebuild)) << toString(std::move(Rebuild)) << " "
                                           << Why;
  EXPECT_GT(Host->runtime().resourceGeneration(), Gen);
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 1u)
      << "the old generation is still physically read after the rebuild";

  // The generation-1 storage must not be reclaimable yet.
  ASSERT_TRUE(Host->retireGenerationsUpTo(Gen, Why)) << Why;
  EXPECT_GT(Host->runtime().pendingRetireGenerationCount(), 0u);
  EXPECT_EQ(Host->runtime().stats().retiredGenerations, 0u);

  // The paused execution returns: the stale sample is counted, the physical
  // lease and the window's protected read are released, and the deferred
  // generation can be reclaimed.
  Host->leave(Ticket);
  EXPECT_EQ(Host->runtime().inFlight(), 0u);
  EXPECT_FALSE(Host->runtime().samplingProtected())
      << "the cancelled window's borrow is released once its last reader left";
  EXPECT_EQ(Host->runtime().physicalReaders(Gen), 0u);
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 0u);
  EXPECT_GE(Host->runtime().stats().staleCallbacks, 1u)
      << "the late sample is a stale callback, never a completion of the new "
         "session";
  EXPECT_EQ(Host->staleLeaveCount(), 1u);
  EXPECT_GT(Host->runtime().stats().reclaimedAfterReaders, 0u);

  // The replacement session's own aggregate budget is untouched by the stale
  // sample: it starts from zero and reaches its own full quota.
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u)
      << "a stale completion must not settle into the replacement session";
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;
  EXPECT_TRUE(Host->driveSampling(4096, 11) > 0);
  EXPECT_EQ(Host->runtime().currentSessionSamples(),
            Host->runtime().sampleBudget());
  ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why))) << Why;
  const auto Ok = call(0, 0, 11);
  ASSERT_EQ(Ok.status, EJitSmallTableDispatch::Dispatched) << Ok.why;
  EXPECT_EQ(Ok.value, aotEntry(0, 0, 11));
}

TEST_F(SmallTableHostTest, NormalReturnControlLeavesNoLeaseOrDeferredRetirement) {
  // The control for both paused tests: the SAME transitions with every call
  // completed normally must leave no reader, no deferral and no pending bytes,
  // and the resources must be reclaimable immediately.
  ASSERT_TRUE(requestFocusEntry());
  ASSERT_TRUE(driveAndPublish());
  const uint64_t OldGen = Host->runtime().resourceGeneration();
  for (int32_t X = -2; X <= 2; ++X) {
    EXPECT_EQ(runWrapperCall(1, 1, X), aotEntry(1, 1, X));
  }
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().physicalReaders(OldGen), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u)
      << "every normal return releases its own protected read";
  EXPECT_EQ(Host->runtime().inFlight(), 0u);

  SmallVector<EJitSmallTableRowKey, 4> Extra;
  for (unsigned T = 0; T < kTrps; ++T)
    Extra.push_back({{2, T}});
  Facts->addReadyMember({2, 0}, 0xE000);
  Facts->addReadyMember({2, 1}, 0xE001);
  Facts->addReadyMember({2, 2}, 0xE002);
  std::string Why;
  Host->cancel("normal-return control drains the published generation");
  ASSERT_FALSE(static_cast<bool>(Host->beginNextGeneration(Extra, Why))) << Why;
  ASSERT_TRUE(Host->retireGenerationsUpTo(OldGen, Why)) << Why;
  EXPECT_FALSE(Host->hasRetiredExecutions())
      << "no reader is left, so nothing is deferred";
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 0u);
  EXPECT_EQ(Host->runtime().pendingRetireBytes(), 0u);
  EXPECT_EQ(Host->runtime().stats().deferredRetirements, 0u);
  EXPECT_GT(Host->runtime().stats().retiredGenerations, 0u)
      << "the unread generation is really released";
}

TEST_F(SmallTableHostTest, LastQuotaT1SurvivesWhileLaterCallsStayAot) {
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 1;
  Opts.runtime.sampling.waitForInFlightOnFreeze = false;
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, kCells, kTrps, Opts));
  const uint64_t OldGen = Host->runtime().resourceGeneration();
  uint64_t Ticket = 0;
  void *Entry = wrapperEnter(1, 1, &Ticket);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Ticket, 0u) << "the last admitted T1 still needs its real leave";
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
  EXPECT_EQ(Host->physicalExecutions(), 1u);
  EXPECT_EQ(Host->runtime().physicalReaders(OldGen), 1u);
  EXPECT_EQ(Facts->outstandingBorrows(), 1u);

  std::vector<std::pair<const uint64_t *, std::vector<uint64_t>>> RawSnapshots;
  for (const std::string &Name : Host->runtime().engine().getLastCounterNames()) {
    auto Counters = Host->runtime().engine().lookup(1, "__profc_" + Name);
    auto Data = Host->runtime().engine().lookup(1, "__profd_" + Name);
    ASSERT_TRUE(static_cast<bool>(Counters)) << toString(Counters.takeError());
    ASSERT_TRUE(static_cast<bool>(Data)) << toString(Data.takeError());
    const auto *Header =
        reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(*Data);
    const auto *Values = reinterpret_cast<const uint64_t *>(*Counters);
    ASSERT_GT(Header->NumCounters, 0u);
    RawSnapshots.push_back(
        {Values, std::vector<uint64_t>(Values, Values + Header->NumCounters)});
  }
  ASSERT_FALSE(RawSnapshots.empty());
  uint64_t LaterTicket = 123;
  EXPECT_EQ(wrapperEnter(0, 0, &LaterTicket), nullptr);
  EXPECT_EQ(LaterTicket, 0u);
  const auto Later = call(0, 0, 3);
  EXPECT_EQ(Later.status, EJitSmallTableDispatch::Aot);
  EXPECT_EQ(aotFocusEntry(0, 0, 3), aotEntry(0, 0, 3))
      << "the caller's AOT fallback still performs the business computation";
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 1u);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
  for (const auto &Snapshot : RawSnapshots)
    for (size_t I = 0; I < Snapshot.second.size(); ++I)
      EXPECT_EQ(Snapshot.first[I], Snapshot.second[I])
          << "quota-exhausted AOT must not alter any real T1 edge counter";

  std::string Why;
  auto Early = Host->runtime().freeze(Why);
  EXPECT_FALSE(static_cast<bool>(Early)) << "freeze must drain the last real T1";
  if (!Early)
    consumeError(Early.takeError());
  Host->cancel("last-quota T1 is still executing");
  ASSERT_FALSE(static_cast<bool>(Host->beginNextGeneration({}, Why))) << Why;
  ASSERT_TRUE(Host->retireGenerationsUpTo(OldGen, Why)) << Why;
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 1u);
  EXPECT_EQ(Facts->outstandingBorrows(), 1u);
  using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
  EXPECT_EQ(reinterpret_cast<EntryFn>(Entry)(1, 1, 5), aotEntry(1, 1, 5))
      << "the old physical call's code and table survive replacement";
  ejit_stab_leave(Ticket);
  EXPECT_EQ(Host->physicalExecutions(), 0u);
  EXPECT_EQ(Host->runtime().inFlight(), 0u);
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 0u);
  EXPECT_EQ(Host->runtime().retainedGenerationCount(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
}

TEST_F(SmallTableHostTest, OldSampleLeaveCannotCompleteAReplacementSample) {
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 1;
  Opts.runtime.sampling.waitForInFlightOnFreeze = false;
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, kCells, kTrps, Opts));
  const uint64_t OldGen = Host->runtime().resourceGeneration();
  uint64_t OldTicket = 0;
  void *OldEntry = wrapperEnter(0, 0, &OldTicket);
  ASSERT_NE(OldEntry, nullptr);
  ASSERT_NE(OldTicket, 0u);
  Host->cancel("replace a paused sampled call");
  std::string Why;
  ASSERT_FALSE(static_cast<bool>(Host->beginNextGeneration({}, Why))) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;
  ASSERT_TRUE(Host->retireGenerationsUpTo(OldGen, Why)) << Why;
  uint64_t NewTicket = 0;
  void *NewEntry = wrapperEnter(1, 1, &NewTicket);
  ASSERT_NE(NewEntry, nullptr);
  ASSERT_NE(NewTicket, 0u);
  EXPECT_EQ(Host->runtime().inFlight(), 2u);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
  EXPECT_EQ(Facts->outstandingBorrows(), 2u)
      << "each overlapping sampling session owns its own protected read";

  using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
  EXPECT_EQ(reinterpret_cast<EntryFn>(OldEntry)(0, 0, 2), aotEntry(0, 0, 2));
  ejit_stab_leave(OldTicket);
  EXPECT_EQ(Host->runtime().inFlight(), 1u);
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u)
      << "the old return must not decrement the new session's freeze barrier";
  EXPECT_EQ(Facts->outstandingBorrows(), 1u);
  ejit_stab_leave(OldTicket); // duplicate: no execution may be closed twice
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
  auto Early = Host->runtime().freeze(Why);
  EXPECT_FALSE(static_cast<bool>(Early));
  if (!Early)
    consumeError(Early.takeError());
  EXPECT_EQ(reinterpret_cast<EntryFn>(NewEntry)(1, 1, 4), aotEntry(1, 1, 4));
  ejit_stab_leave(NewTicket);
  ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why))) << Why;
  ASSERT_NE(Host->bundle(), nullptr);
  EXPECT_EQ(Host->bundle()->sampleCount, 1u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
}

TEST_F(SmallTableHostTest, ReplacementSamplingMustObtainItsOwnBorrow) {
  ASSERT_TRUE(requestFocusEntry());
  uint64_t OldTicket = 0;
  ASSERT_NE(wrapperEnter(0, 0, &OldTicket), nullptr);
  ASSERT_NE(OldTicket, 0u);
  Host->cancel("retain old sampling borrow");
  std::string Why;
  ASSERT_FALSE(static_cast<bool>(Host->beginNextGeneration({}, Why))) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;
  Facts->refuseBorrow("replacement transaction is not ready");
  uint64_t NewTicket = 0;
  EXPECT_EQ(wrapperEnter(1, 1, &NewTicket), nullptr)
      << "the retained old borrow cannot authorize a new session";
  EXPECT_EQ(NewTicket, 0u);
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u);
  EXPECT_EQ(Host->runtime().inFlight(), 1u);
  EXPECT_EQ(Facts->outstandingBorrows(), 1u);
  Facts->allowBorrow();
  ASSERT_NE(wrapperEnter(1, 1, &NewTicket), nullptr);
  EXPECT_EQ(Facts->outstandingBorrows(), 2u);
  ejit_stab_leave(OldTicket);
  EXPECT_EQ(Facts->outstandingBorrows(), 1u);
  ejit_stab_leave(NewTicket);
  Host->cancel("close replacement window");
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
}

TEST_F(SmallTableHostTest, RetiringTwoLiveGenerationsDrainsBothReaderSets) {
  ASSERT_TRUE(requestFocusEntry());
  const uint64_t G1 = Host->runtime().resourceGeneration();
  uint64_t T1 = 0;
  ASSERT_NE(wrapperEnter(0, 0, &T1), nullptr);
  Host->cancel("prepare second generation while first call runs");
  std::string Why;
  ASSERT_FALSE(static_cast<bool>(Host->beginNextGeneration({}, Why))) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;
  const uint64_t G2 = Host->runtime().resourceGeneration();
  uint64_t T2 = 0;
  ASSERT_NE(wrapperEnter(1, 1, &T2), nullptr);
  Host->cancel("prepare third generation while both old calls run");
  ASSERT_FALSE(static_cast<bool>(Host->beginNextGeneration({}, Why))) << Why;
  ASSERT_TRUE(Host->retireGenerationsUpTo(G2, Why)) << Why;
  EXPECT_EQ(Host->physicalExecutions(), 2u);
  EXPECT_EQ(Host->retiredExecutionGeneration(), G1);
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 2u);
  ejit_stab_leave(T1);
  EXPECT_EQ(Host->physicalExecutions(), 1u);
  EXPECT_TRUE(Host->hasRetiredExecutions());
  EXPECT_EQ(Host->retiredExecutionGeneration(), G2);
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 1u);
  ejit_stab_leave(T2);
  EXPECT_FALSE(Host->hasRetiredExecutions());
  EXPECT_EQ(Host->runtime().pendingRetireGenerationCount(), 0u);
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
}

TEST_F(SmallTableHostTest, LastLeaveDoesNotImplicitlyRetireAnOldGeneration) {
  ASSERT_TRUE(requestFocusEntry());
  const uint64_t OldGen = Host->runtime().resourceGeneration();
  uint64_t Ticket = 0;
  ASSERT_NE(wrapperEnter(0, 0, &Ticket), nullptr);
  Host->cancel("prepare replacement without retiring the old generation");
  std::string Why;
  ASSERT_FALSE(static_cast<bool>(Host->beginNextGeneration({}, Why))) << Why;
  EXPECT_EQ(Host->runtime().retainedGenerationCount(), 1u);
  ejit_stab_leave(Ticket);
  EXPECT_EQ(Host->runtime().retainedGenerationCount(), 1u)
      << "retention remains until the explicit retirement protocol permits it";
  EXPECT_EQ(Host->runtime().stats().retiredGenerations, 0u);
  ASSERT_TRUE(Host->retireGenerationsUpTo(OldGen, Why)) << Why;
  EXPECT_EQ(Host->runtime().retainedGenerationCount(), 0u);
}

TEST_F(SmallTableHostTest, ReplacementHostCannotStealARetainedOwnersTicket) {
  ASSERT_TRUE(requestFocusEntry());
  ASSERT_TRUE(driveAndPublish());
  uint64_t OldTicket = 0;
  void *OldEntry = wrapperEnter(0, 0, &OldTicket);
  ASSERT_NE(OldEntry, nullptr);
  ASSERT_NE(OldTicket, 0u);
  std::shared_ptr<EJitSmallTableHostFactSource> OldFacts = Facts;
  ASSERT_FALSE(Host->beginOwnerTeardown());
  EJitSmallTableHost::adoptRetired(std::move(Host));
  Modules.clear();
  ASSERT_TRUE(requestFocusEntry());
  uint64_t NewTicket = 0;
  ASSERT_NE(wrapperEnter(1, 1, &NewTicket), nullptr);
  ASSERT_NE(NewTicket, 0u);
  EXPECT_NE(NewTicket, OldTicket)
      << "tokens identify physical executions across every host owner";
  using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
  EXPECT_EQ(reinterpret_cast<EntryFn>(OldEntry)(0, 0, 3), aotEntry(0, 0, 3));
  ejit_stab_leave(OldTicket);
  EXPECT_EQ(EJitSmallTableHost::retainedOwnerCount(), 0u);
  EXPECT_EQ(OldFacts->outstandingBorrows(), 0u);
  EXPECT_EQ(Host->physicalExecutions(), 1u)
      << "old-owner completion cannot close the replacement host's execution";
  EXPECT_EQ(Host->runtime().sessionInFlight(), 1u);
  ejit_stab_leave(NewTicket);
  Host->cancel("finish replacement test window");
  EXPECT_EQ(Facts->outstandingBorrows(), 0u);
}

//===----------------------------------------------------------------------===//
// 5c. ONE common T1 window filled by REAL executions, and the common T2 that
//     consumes the whole frozen bundle.
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, RealExecutionsFillOneCommonT1WindowOfSixtyFour) {
  // The aggregate budget is the contract: ONE common window of 64 REAL admitted
  // executions shared by every admitted ready member (not 64 per member, and
  // not a representative-only sample). The window here is filled by ordinary
  // admitted calls through enter/leave - not by the sampling driver - so what
  // fills it is real execution completion, and the freeze happens only after
  // every one of them returned.
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 64;
  Opts.runtime.sampling.freezeWaitMillis = 100;
  auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1);
  ASSERT_TRUE(M);
  Facts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D, kCells,
                    kTrps);
  ASSERT_TRUE(makeHost(Facts, Opts));
  installRetractionHook();
  registerLifecycle(kCellPeriod);
  registerLifecycle(kTrpPeriod);
  EJitSmallTableHost::EntryRequest Req;
  Req.module = M.get();
  Req.entryName = "f_entry";
  Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
  Req.sourceVarName = "g_focus";
  LocalDims = focusDims();
  Req.dims = LocalDims;
  std::vector<std::string> P = {kCellPeriod, kTrpPeriod};
  Req.dimPeriodNames = P;
  Req.codeGeneration = 1;
  std::string Why;
  ASSERT_TRUE(static_cast<bool>(Host->planEntry(Req, Why))) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;
  ASSERT_EQ(Host->runtime().sampleBudget(), 64u)
      << "the default aggregate budget is 64 for ONE entry/code generation";

  // Ordinary traffic: round-robin over every admitted member until the shared
  // budget is reached, each execution entered and left for real.
  const uint32_t D[2] = {cellSlot(), trpSlot()};
  uint64_t Calls = 0;
  unsigned Member = 0;
  while (!Host->runtime().samplingExhausted()) {
    const unsigned Cell = Member % kCells;
    const unsigned Trp = (Member / kCells) % kTrps;
    ++Member;
    const uint32_t I[2] = {Cell, Trp};
    uint64_t Ticket = 0;
    void *Entry = Host->enterInstrumented(D, I, &Ticket, &Why);
    ASSERT_NE(Entry, nullptr) << Why;
    using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
    const int64_t Got = reinterpret_cast<EntryFn>(Entry)(Cell, Trp, 1);
    EXPECT_EQ(Got, aotEntry(Cell, Trp, 1))
        << "cell=" << Cell << " trp=" << Trp;
    ASSERT_NE(Ticket, 0u)
        << "a counted execution without a ticket cannot be completed";
    Host->leave(Ticket);
    ++Calls;
    ASSERT_LT(Calls, 512u) << "the aggregate budget never filled";
  }
  EXPECT_EQ(Calls, 64u) << "exactly the aggregate budget of real executions";
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 64u)
      << "the samples are real admitted executions, not lookups";
  EXPECT_EQ(Host->runtime().inFlight(), 0u)
      << "every real execution returned before any freeze";
  EXPECT_TRUE(Host->runtime().samplingProtected())
      << "the open window keeps its protected read until freeze or cancel";
  EXPECT_TRUE(Host->runtime().samplingExhausted())
      << "the shared budget is what the 64 real executions reached";

  // Freeze only after the real executions completed: the immutable bundle is
  // whole and carries the same table/generation identity the code bound.
  const EJitSmallTableProfileBundle *BundleWhileOpen = Host->bundle();
  EXPECT_EQ(BundleWhileOpen, nullptr) << "nothing is frozen before the freeze";
  EXPECT_EQ(Host->runtime().sampleCount() >= 64u, true);
  ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why))) << Why;
  const EJitSmallTableProfileBundle *Bundle = Host->bundle();
  ASSERT_NE(Bundle, nullptr);
  EXPECT_EQ(Bundle->sampleCount, 64u);
  EXPECT_EQ(Bundle->resourceGeneration, Host->runtime().resourceGeneration())
      << "the bundle names the resource generation T1 was compiled against";
  EXPECT_EQ(Bundle->resourceAddress,
            reinterpret_cast<uintptr_t>(Host->runtime().resource()->base()))
      << "symbol-name equality is not identity: the address must be the same";
  EXPECT_GT(Bundle->participatingMembers, 1u)
      << "the common window is shared by more than one member";
  EXPECT_FALSE(Bundle->readinessProvider.empty());
  EXPECT_FALSE(Bundle->profileData.empty())
      << "the bundle must carry a real synthesized profile, not a placeholder";
  EXPECT_FALSE(Bundle->counters.empty());
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 64u)
      << "the aggregate session count is the frozen sample count";
  EXPECT_EQ(Host->activeTier(), "final");
}

TEST_F(SmallTableHostTest, FreezeWaitsForTheLastInFlightExecution) {
  // Admission is not completion: with the freeze wait enabled, a freeze that
  // arrives while an admitted execution is still running must wait for its real
  // return rather than read a half-complete window.
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = 4;
  Opts.runtime.sampling.waitForInFlightOnFreeze = true;
  Opts.runtime.sampling.freezeWaitMillis = 3000;
  auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1);
  ASSERT_TRUE(M);
  Facts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D, kCells,
                    kTrps);
  ASSERT_TRUE(makeHost(Facts, Opts));
  installRetractionHook();
  registerLifecycle(kCellPeriod);
  registerLifecycle(kTrpPeriod);
  EJitSmallTableHost::EntryRequest Req;
  Req.module = M.get();
  Req.entryName = "f_entry";
  Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
  Req.sourceVarName = "g_focus";
  LocalDims = focusDims();
  Req.dims = LocalDims;
  std::vector<std::string> P = {kCellPeriod, kTrpPeriod};
  Req.dimPeriodNames = P;
  Req.codeGeneration = 1;
  std::string Why;
  ASSERT_TRUE(static_cast<bool>(Host->planEntry(Req, Why))) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;

  // Three completed samples and one execution held open: the window is at its
  // budget while the fourth real execution has not returned.
  const uint32_t D[2] = {cellSlot(), trpSlot()};
  for (unsigned I = 0; I < 3; ++I) {
    const uint32_t Inst[2] = {I, 0};
    uint64_t Ticket = 0;
    void *Entry = Host->enterInstrumented(D, Inst, &Ticket, &Why);
    ASSERT_NE(Entry, nullptr) << Why;
    Host->leave(Ticket);
  }
  const uint32_t Inst[2] = {0, 1};
  uint64_t Ticket = 0;
  void *Entry = Host->enterInstrumented(D, Inst, &Ticket, &Why);
  ASSERT_NE(Entry, nullptr) << Why;
  ASSERT_NE(Ticket, 0u);
  ASSERT_EQ(Host->runtime().currentSessionSamples(), 4u);
  ASSERT_EQ(Host->runtime().sessionInFlight(), 1u);

  // The freeze is refused (not silently truncated) while that execution runs.
  std::string FreezeWhy;
  auto Frozen = Host->runtime().freeze(FreezeWhy);
  EXPECT_FALSE(static_cast<bool>(Frozen)) << "a granted dispatch is not a sample";
  if (!Frozen)
    consumeError(Frozen.takeError());
  EXPECT_FALSE(FreezeWhy.empty());
  EXPECT_EQ(Host->runtime().inFlight(), 1u)
      << "the refused freeze must not have dropped the execution";

  // The real return completes the window; the freeze then succeeds on the SAME
  // session and the bundle reports all four real executions.
  Host->leave(Ticket);
  EXPECT_EQ(Host->runtime().inFlight(), 0u);
  std::string Why2;
  ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why2))) << Why2;
  ASSERT_NE(Host->bundle(), nullptr);
  EXPECT_EQ(Host->bundle()->sampleCount, 4u);
  EXPECT_EQ(Host->runtime().inFlight(), 0u);
}

TEST_F(SmallTableHostTest, FrozenBundleIsConsumedWholeByTheCommonT2) {
  // The common T2 is compiled FROM the frozen bundle and must bind the SAME
  // table resource as T1 (resource address AND generation, checked by the engine
  // lookup, not by symbol name). The published slots then run the T2 entry.
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, kCells, kTrps, {},
                                /*BranchProfile=*/true));
  const uint64_t Budget = Host->runtime().sampleBudget();
  for (uint64_t I = 0; I < Budget; ++I) {
    const unsigned Cell = I % kCells;
    const unsigned Trp = (I / kCells) % kTrps;
    const int32_t X = I % 2 ? -3 : 4;
    EXPECT_EQ(runWrapperCall(Cell, Trp, X),
              aotEntry(Cell, Trp, X) + (X < 0 ? -23 : 17));
  }
  EXPECT_EQ(Host->runtime().currentSessionSamples(), Budget);
  EXPECT_GT(g_profilePositiveCalls, 0u);
  EXPECT_GT(g_profileNegativeCalls, 0u);
  std::string Why;
  auto Frozen = Host->runtime().freeze(Why);
  ASSERT_TRUE(static_cast<bool>(Frozen)) << Why;
  const EJitSmallTableProfileBundle *Bundle = Host->bundle();
  ASSERT_NE(Bundle, nullptr);
  ASSERT_EQ(Bundle->resourceGeneration, Host->runtime().resourceGeneration());
  ASSERT_EQ(Bundle->resourceAddress,
            reinterpret_cast<uintptr_t>(Host->runtime().resource()->base()));
  ASSERT_GT(Bundle->counters.size(), 0u);

  // Every counter the bundle carries resolves in the SAME engine to a real
  // address: that is the profile data T2 consumes.
  for (const PgoCounterRef &C : Bundle->counters) {
    ASSERT_NE(C.pgoName, nullptr);
    std::string SymbolSuffix;
    for (const std::string &Suffix :
         Host->runtime().engine().getLastCounterNames())
      if (Host->runtime().engine().getCounterProfileName(Suffix) == C.pgoName) {
        ASSERT_TRUE(SymbolSuffix.empty()) << "duplicate canonical name mapping";
        SymbolSuffix = Suffix;
      }
    ASSERT_FALSE(SymbolSuffix.empty()) << C.pgoName;
    const std::string CounterName = "__profc_" + SymbolSuffix;
    auto Addr = Host->runtime().engine().lookup(Bundle->codeGeneration,
                                              CounterName);
    ASSERT_TRUE(static_cast<bool>(Addr))
        << CounterName << ": " << toString(Addr.takeError());
    EXPECT_NE(C.profcAddr, 0u) << CounterName;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(*Addr), C.profcAddr) << CounterName;
  }

  const std::string CompleteProfile = Bundle->profileData;
  ASSERT_FALSE(CompleteProfile.empty());
  auto ReaderOrErr = InstrProfReader::create(
      MemoryBuffer::getMemBufferCopy(CompleteProfile));
  ASSERT_TRUE(static_cast<bool>(ReaderOrErr))
      << toString(ReaderOrErr.takeError());
  std::set<std::string> CapturedNames;
  for (const PgoCounterRef &Counter : Bundle->counters) {
    ASSERT_NE(Counter.pgoName, nullptr);
    ASSERT_TRUE(CapturedNames.insert(Counter.pgoName).second)
        << "duplicate captured profile function";
  }
  std::set<std::string> ExpectedNames;
  for (const std::string &Name : Host->runtime().engine().getLastCounterNames()) {
    StringRef ProfileName = Host->runtime().engine().getCounterProfileName(Name);
    ASSERT_FALSE(ProfileName.empty());
    ASSERT_TRUE(ExpectedNames.insert(ProfileName.str()).second);
  }
  EXPECT_EQ(CapturedNames, ExpectedNames)
      << "capture may not silently omit an emitted function's counters";
  EXPECT_GE(CapturedNames.size(), 3u)
      << "the root and both noinline helpers must be present in the bundle";
  std::set<std::string> ProfileNames;
  bool SawMultipleCounters = false;
  for (const NamedInstrProfRecord &Record : **ReaderOrErr) {
    ASSERT_TRUE(ProfileNames.insert(Record.Name.str()).second)
        << "duplicate decoded profile function";
    const PgoCounterRef *Captured = nullptr;
    for (const PgoCounterRef &Counter : Bundle->counters)
      if (Record.Name == Counter.pgoName)
        Captured = &Counter;
    ASSERT_NE(Captured, nullptr) << Record.Name.str();
    ASSERT_NE(Captured->profdAddr, 0u);
    const auto *Data =
        reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(
            Captured->profdAddr);
    EXPECT_EQ(IndexedInstrProf::ComputeHash(Record.Name), Data->NameRef)
        << "canonical PGO name hash must match the real __profd NameRef: "
        << Record.Name.str();
    ASSERT_EQ(Record.Hash, Data->FuncHash) << Record.Name.str();
    ASSERT_EQ(Record.Counts.size(), Data->NumCounters) << Record.Name.str();
    ASSERT_FALSE(Record.Counts.empty());
    if (Record.Name == "f_entry") {
      EXPECT_GT(Data->NumCounters, 1u)
          << "the dynamic two-path fixture must cover tail edge counters";
      SawMultipleCounters = Data->NumCounters > 1;
    }
    const auto *RawCounts =
        reinterpret_cast<const uint64_t *>(Captured->profcAddr);
    for (size_t I = 0; I < Record.Counts.size(); ++I)
      EXPECT_EQ(Record.Counts[I], RawCounts[I])
          << Record.Name.str() << " counter " << I;
  }
  EXPECT_FALSE((*ReaderOrErr)->hasError());
  EXPECT_EQ(ProfileNames, CapturedNames)
      << "the frozen profile contains every captured function's full counters";
  EXPECT_TRUE(SawMultipleCounters);
  ASSERT_FALSE(static_cast<bool>(Host->publishGeneration(Why))) << Why;
  EXPECT_EQ(Host->bundle(), Bundle) << "T2 consumes the same frozen bundle";
  EXPECT_EQ(Host->bundle()->profileData, CompleteProfile);

  // The published tier is the T2 compiled from this bundle, and it is a
  // different code generation from the instrumented T1.
  EXPECT_EQ(Host->activeTier(), "final");
  ASSERT_NE(Host->instrumentedEntry(), nullptr);
  ASSERT_NE(Host->activeEntry(), nullptr);
  EXPECT_NE(Host->activeEntry(), Host->instrumentedEntry())
      << "T2 is its own compiled entry, not the instrumented one";

  // Published calls run that T2 under the same provider contract. Mutating an
  // authorized configuration field here without moving the provider revision
  // would violate that contract and must be tested as admission refusal, not
  // as an unchanged published dispatch (see changed-member tests).
  const EJitSmallTablePlan *Plan = Host->plan();
  ASSERT_NE(Plan, nullptr);
  ASSERT_GE(Plan->fields.size(), 2u);
  ASSERT_EQ(Plan->fields[1].strategy, EJitSmallTableStrategy::Table)
      << "this fixture's per-cell field must be a table for this check";
  const uint64_t PositiveBeforeT2 = g_profilePositiveCalls;
  const uint64_t NegativeBeforeT2 = g_profileNegativeCalls;
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T)
      for (int32_t X : {-2, 3}) {
        const auto R = call(C, T, X);
        ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched) << R.why;
        EXPECT_EQ(R.value, aotEntry(C, T, X) + (X < 0 ? -23 : 17))
            << "the common T2 consumes the full profile and executes both paths";
      }
  EXPECT_EQ(g_profilePositiveCalls - PositiveBeforeT2, kCells * kTrps);
  EXPECT_EQ(g_profileNegativeCalls - NegativeBeforeT2, kCells * kTrps);
}

//===----------------------------------------------------------------------===//
// 6. Failure paths: queue/compile failure, cancel, stale callbacks, retraction
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, CompileFailureLeavesEverySlotUnpublished) {
  // The request succeeds but T1 cannot materialize (the module's entry does not
  // exist), so no slot may ever be published and every call stays AOT.
  auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1);
  ASSERT_TRUE(M);
  Facts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D, kCells,
                    kTrps);
  ASSERT_TRUE(makeHost(Facts));
  registerLifecycle(kCellPeriod);
  registerLifecycle(kTrpPeriod);
  EJitSmallTableHost::EntryRequest Req;
  Req.module = M.get();
  Req.entryName = "missing_entry"; // declared nowhere in the module
  Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
  Req.sourceVarName = "g_focus";
  LocalDims = focusDims();
  Req.dims = LocalDims;
  std::vector<std::string> P = {kCellPeriod, kTrpPeriod};
  Req.dimPeriodNames = P;
  std::string Why;
  auto R = Host->planEntry(Req, Why);
  if (R) {
    auto T1 = Host->compileT1(Why);
    EXPECT_FALSE(static_cast<bool>(T1)) << "T1 must fail for an unknown entry";
    if (T1)
      consumeError(T1.takeError());
  } else {
    consumeError(R.takeError());
  }
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_FALSE(Host->codeReady());
  // The entry never completed its request path, so the host is not bound to the
  // function index at all: the ordinary dispatch API leaves it entirely alone
  // and the call site keeps its baseline body.
  const auto C = call(0, 0, 1);
  EXPECT_EQ(C.status, EJitSmallTableDispatch::NotBound) << C.why;
  EXPECT_FALSE(Host->bound());
}

TEST_F(SmallTableHostTest, CancelRejectsStaleCallbacksAndLaterDispatch) {
  EJitSmallTableHost::Options Opts;
  // A small aggregate budget keeps the sampling window short so the test can
  // reach its sample limit and still hold one execution in flight.
  Opts.runtime.sampling.aggregateLimit = 4;
  Opts.runtime.sampling.freezeWaitMillis = 100;
  auto M = parseHost("f_entry", "g_focus", "g_focus_out", kCells, kTrps + 1);
  ASSERT_TRUE(M);
  Facts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D, kCells,
                    kTrps);
  ASSERT_TRUE(makeHost(Facts, Opts));
  installRetractionHook();
  registerLifecycle(kCellPeriod);
  registerLifecycle(kTrpPeriod);
  EJitSmallTableHost::EntryRequest Req;
  Req.module = M.get();
  Req.entryName = "f_entry";
  Req.funcIndex = EJitFuncRegistry::instance().resolveAssign("f_entry");
  Req.sourceVarName = "g_focus";
  LocalDims = focusDims();
  Req.dims = LocalDims;
  std::vector<std::string> P = {kCellPeriod, kTrpPeriod};
  Req.dimPeriodNames = P;
  std::string Why;
  ASSERT_TRUE(static_cast<bool>(Host->planEntry(Req, Why))) << Why;
  ASSERT_TRUE(static_cast<bool>(Host->compileT1(Why))) << Why;

  // Three real samples, then the fourth (the budget's last) admitted execution
  // is left IN FLIGHT when the session is cancelled.
  const uint32_t D[2] = {cellSlot(), trpSlot()};
  for (unsigned I = 0; I < 3; ++I) {
    const uint32_t Inst[2] = {I, 0};
    uint64_t Ticket = 0;
    void *Entry = Host->enterInstrumented(D, Inst, &Ticket, &Why);
    ASSERT_NE(Entry, nullptr) << Why;
    ASSERT_NE(Ticket, 0u);
    Host->leave(Ticket);
  }
  const uint32_t Inst[2] = {0, 1};
  uint64_t Ticket = 0;
  void *Entry = Host->enterInstrumented(D, Inst, &Ticket, &Why);
  ASSERT_NE(Entry, nullptr) << Why;
  ASSERT_NE(Ticket, 0u);
  EXPECT_EQ(Host->runtime().inFlight(), 1u);
  EXPECT_EQ(Host->activeExecutions(), 1u);

  Host->cancel("timeout");
  EXPECT_EQ(Host->publishedSlots(), 0u);
  // PHYSICAL LIFETIME (coordinator P1 repair, 2026-09-16). The cancel settles
  // the execution LOGICALLY - no new call is admitted, the slot is drained, the
  // session stops counting - but the granted execution is a real call that has
  // not returned. Its protected read and its resource generation stay held until
  // its own `leave`, because the generated code already entered and still reads
  // the table through the column addresses it bound.
  EXPECT_EQ(Host->activeExecutions(), 1u)
      << "logical cancellation is not physical completion";
  EXPECT_EQ(Host->physicalExecutions(), 1u);
  EXPECT_EQ(Host->logicallyClosedExecutions(), 1u)
      << "the in-flight execution is settled for sampling, not released";
  EXPECT_EQ(Facts->outstandingBorrows(), 1u)
      << "the protected read must survive until the real call returns";
  EXPECT_EQ(Host->runtime().inFlight(), 1u)
      << "the runtime's in-flight count is physical and survives cancel";
  EXPECT_TRUE(Host->runtime().samplingProtected())
      << "the sampling window's borrow guards the running execution";
  EXPECT_EQ(Host->runtime().physicalReaders(Host->runtime().resourceGeneration()),
            1u);
  EXPECT_FALSE(Host->codeReady());

  // Completing the granted execution now is a stale completion for the session:
  // it is counted, never merged into a later generation, and it releases its OWN
  // physical lease.
  const uint64_t Before = Host->staleLeaveCount();
  Host->leave(Ticket);
  EXPECT_EQ(Host->staleLeaveCount(), Before + 1)
      << "a ticket from a cancelled session must be recognized as stale";
  EXPECT_GT(Host->runtime().stats().staleCallbacks, 0u)
      << "the runtime must record the stale sample callback too";
  EXPECT_EQ(Host->activeExecutions(), 0u)
      << "the real return releases the physical execution";
  EXPECT_EQ(Facts->outstandingBorrows(), 0u)
      << "the real return releases the protected read";
  EXPECT_EQ(Host->runtime().inFlight(), 0u);
  EXPECT_FALSE(Host->runtime().samplingProtected())
      << "the cancelled window's borrow is released once its last reader left";

  const auto R = call(0, 0, 1);
  EXPECT_EQ(R.status, EJitSmallTableDispatch::Aot) << R.why;
}

TEST_F(SmallTableHostTest, UnprovableCoordinateStaysAot) {
  ASSERT_TRUE(requestFocusEntry());
  ASSERT_TRUE(driveAndPublish());
  const uint64_t Before = Host->unprovableCoordinateCount();

  // A call that omits the declared TRP dimension cannot be mapped to a row:
  // the host must not guess which member it belongs to.
  const uint32_t JustCell[1] = {cellSlot()};
  const uint32_t OneInstance[1] = {0};
  const auto Short = Host->dispatch(JustCell, OneInstance, 1);
  EXPECT_EQ(Short.status, EJitSmallTableDispatch::CoordinateUnprovable)
      << Short.why;

  // A dim list that duplicates the declared dimension is ambiguous too.
  const uint32_t Dup[2] = {cellSlot(), cellSlot()};
  const uint32_t DupInst[2] = {0, 1};
  const auto Duplicated = Host->dispatch(Dup, DupInst, 1);
  EXPECT_EQ(Duplicated.status, EJitSmallTableDispatch::CoordinateUnprovable)
      << Duplicated.why;

  // An out-of-extent instance is not a member of the declared schema at all.
  const uint32_t D[2] = {cellSlot(), trpSlot()};
  const uint32_t Oor[2] = {kCells + 5, 0};
  const auto Outside = Host->dispatch(D, Oor, 1);
  EXPECT_EQ(Outside.status, EJitSmallTableDispatch::CoordinateUnprovable)
      << Outside.why;

  EXPECT_EQ(Host->unprovableCoordinateCount(), Before + 3);
  EXPECT_EQ(Host->publishedSlots(), kCells * kTrps)
      << "an unprovable call must not disturb the published slots";
}

TEST_F(SmallTableHostTest, LifecycleBoundaryPublishesAndDrainsOneMember) {
  // Cells 0..1 are ready, so every (cell, trp) coordinate shares its TRP
  // instance with exactly one other coordinate.
  ASSERT_TRUE(requestFocusEntry(kCells, kTrps, /*ReadyCells=*/2, kTrps));
  ASSERT_TRUE(driveAndPublish());
  const uint64_t Published = Host->publishedSlots();
  const std::vector<std::vector<uint64_t>> TrpOne = coordinatesWith(1, 1);
  ASSERT_FALSE(TrpOne.empty());

  // Deactivating one lifecycle instance drains exactly that member set; the rows
  // and every other member stay as they are.
  std::string Why;
  std::string SlotDump;
  raw_string_ostream OS(SlotDump);
  for (const EJitSmallTableLogicalSlot &S : Host->slots()) {
    OS << "(";
    for (uint64_t V : S.coordinate)
      OS << V << ",";
    OS << " pub=" << (S.published() ? 1 : 0) << ")";
  }
  OS.flush();
  EXPECT_TRUE(Host->onProductDeactivated(kTrpPeriod, 1))
      << SlotDump << " | trpOne=" << TrpOne.size()
      << " | deactivate-report: " << Host->lastRefusal();
  EXPECT_EQ(Host->publishedSlots(), Published - TrpOne.size())
      << "only the deactivated instance's coordinates stop being published";
  for (const std::vector<uint64_t> &C : TrpOne) {
    const auto Drained =
        Host->dispatchMember(C, 1);
    EXPECT_EQ(Drained.status, EJitSmallTableDispatch::Aot) << Drained.why;
  }
  const auto Other = call(0, 0, 1);
  EXPECT_EQ(Other.status, EJitSmallTableDispatch::Dispatched) << Other.why;
  EXPECT_EQ(Other.value, aotEntry(0, 0, 1));

  // Re-activating republishes the member set from the SAME resource generation.
  EXPECT_TRUE(Host->onProductActivated(kTrpPeriod, 1, &Why)) << Why;
  EXPECT_EQ(Host->publishedSlots(), Published);
  const auto Again = call(0, 1, 1);
  EXPECT_EQ(Again.status, EJitSmallTableDispatch::Dispatched) << Again.why;
  EXPECT_EQ(Again.value, aotEntry(0, 1, 1));
}

//===----------------------------------------------------------------------===//
// 7. The entry matrix and separate byte accounting
//===----------------------------------------------------------------------===//

TEST_F(SmallTableHostTest, SixEntriesBySixCellsByTwentyTrpsShareTheTableContract) {
  // The workload shape the plan asks for, stated explicitly: 6 entries x 6 cells
  // x 20 TRPs, driven through the normal request/dispatch API by several entries
  // that share one configured table. The declared shape stays 16x32; the window
  // is a WINDOW of it, never a hardcoded capacity.
  //
  // The first 4 entries each own a DISTINCT lifecycle pair, which is what fills
  // the 8 process-global dimType slots (`kEJitMaxDimTypes`); the last 2 entries
  // SHARE pairs 0 and 1, so they consume no new dimType slot. The supported
  // entry count is therefore not 4 and not a dimType limit: the binding limit is
  // the set of distinct lifecycle names, and the entry count itself is bounded
  // by the entry registry (`EJitFuncRegistry`), which the shared entries are
  // registered in below. Bounds are reported, not worked around.
  constexpr unsigned kEntries = 4;      // entries that own a distinct pair
  constexpr unsigned kSharedEntries = 2; // extra entries over existing pairs
  constexpr unsigned kMatrixWindowCells = 6;

  std::vector<std::unique_ptr<EJitSmallTableHost>> Hosts;
  std::vector<std::unique_ptr<Module>> Ms;
  std::vector<std::shared_ptr<EJitSmallTableHostFactSource>> Fs;
  std::vector<std::vector<std::string>> Ps;

  // Each entry gets its OWN lifecycle dimension pair, exactly as a product with
  // per-entry configuration sources does (distinct dimType slots).
  std::vector<std::string> CellPeriods;
  std::vector<std::string> TrpPeriods;
  for (unsigned E = 0; E < kEntries; ++E) {
    CellPeriods.push_back("m_cell_" + std::to_string(E));
    TrpPeriods.push_back("m_trp_" + std::to_string(E));
    registerLifecycle(CellPeriods.back().c_str());
    registerLifecycle(TrpPeriods.back().c_str());
  }

  for (unsigned E = 0; E < kEntries; ++E) {
    const std::string Entry = "m_entry_" + std::to_string(E);
    auto M = parseHost(Entry, "g_matrix", "g_matrix_out", kCeilingCells,
                       kCeilingTrps);
    ASSERT_TRUE(M) << Entry;
    auto F = std::make_shared<EJitSmallTableHostFactSource>(
        "g_matrix", &g_matrix[0][0], sizeof(g_matrix), 0x6000 + E);
    for (unsigned C = 0; C < kMatrixWindowCells; ++C)
      for (unsigned T = 0; T < kMatrixTrps; ++T)
        F->addReadyMember({C, T}, 0x6000 + E * 256 + C * 32 + T);

    Config Cfg;
    auto H = EJitSmallTableHost::create(Cfg, State.getRegistry(), State, F,
                                        EJitSmallTableHost::Options{});
    ASSERT_TRUE(static_cast<bool>(H)) << toString(H.takeError());
    auto HostPtr = std::move(*H);

    EJitSmallTableHost::EntryRequest Req;
    Req.module = M.get();
    Req.entryName = Entry;
    Req.funcIndex = EJitFuncRegistry::instance().resolveAssign(Entry);
    Req.sourceVarName = "g_matrix";
    SmallVector<EJitSmallTableDim, 2> D =
        hostDims(kCeilingCells, kCeilingTrps);
    Req.dims = D;
    std::vector<std::string> P = {CellPeriods[E], TrpPeriods[E]};
    Req.dimPeriodNames = P;
    Req.codeGeneration = 10 + E;
    std::string Why;
    Error ReqErr = HostPtr->requestEntry(
        Req, reinterpret_cast<void *>(&aotFocusEntry), Why);
    const bool ReqOk = !static_cast<bool>(ReqErr);
    if (!ReqOk) {
      ADD_FAILURE() << Entry << ": requestEntry failed: "
                    << toString(std::move(ReqErr)) << " (" << Why << ")";
    }
    ASSERT_TRUE(ReqOk) << Entry << ": " << Why;

    // Real dispatched sampling calls for THIS entry, then the published
    // generation.
    ASSERT_TRUE(HostPtr->driveSampling(4096, 1) > 0) << Entry;
    ASSERT_FALSE(static_cast<bool>(HostPtr->publishGeneration(Why))) << Entry << ": " << Why;
    EXPECT_EQ(HostPtr->publishedSlots(), kMatrixWindowCells * kMatrixTrps)
        << Entry;

    Ms.push_back(std::move(M));
    Fs.push_back(std::move(F));
    Ps.push_back(std::move(P));
    Hosts.push_back(std::move(HostPtr));
  }

  // Every entry dispatches through the same ordinary API, with its own dims, and
  // matches the AOT baseline on the whole window.
  for (unsigned E = 0; E < kEntries; ++E) {
    const uint32_t Cell = EJitLifecycleRegistry::instance().lookup(
        CellPeriods[E]);
    const uint32_t Trp =
        EJitLifecycleRegistry::instance().lookup(TrpPeriods[E]);
    ASSERT_NE(Cell, kEJitInvalidDimType);
    ASSERT_NE(Trp, kEJitInvalidDimType);
    for (unsigned C = 0; C < kMatrixWindowCells; ++C)
      for (unsigned T = 0; T < kMatrixTrps; ++T) {
        const uint32_t D[2] = {Cell, Trp};
        const uint32_t I[2] = {C, T};
        const auto R = Hosts[E]->dispatch(D, I, 2);
        ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched)
            << "entry " << E << ": " << R.why;
        EXPECT_EQ(R.value, aotResult(g_matrix[C][T], 2))
            << "entry=" << E << " cell=" << C << " trp=" << T;
      }
  }

  // Separate accounting: code, table and retained bytes are recorded
  // independently, and the table bytes are the declared shape's payload.
  std::string Report;
  raw_string_ostream OS(Report);
  for (unsigned E = 0; E < kEntries; ++E) {
    const auto A = Hosts[E]->accounting();
    OS << "entry " << E << ": code=" << A.publishedCodeBytes
       << " table_payload=" << A.tablePayloadBytes
       << " table_allocated=" << A.tableAllocatedBytes
       << " table_published=" << A.tablePublishedBytes
       << " retained=" << A.retainedBytes
       << " retained_generations=" << A.retainedGenerations << "\n";
    EXPECT_GT(A.tablePayloadBytes, 0u) << "entry " << E;
    EXPECT_GT(A.tablePublishedBytes, 0u) << "entry " << E;
    EXPECT_GE(A.tableAllocatedBytes, A.tablePayloadBytes) << "entry " << E;
    EXPECT_EQ(A.retainedBytes, 0u)
        << "no generation was retired, so nothing is retained yet";
    EXPECT_EQ(A.retainedGenerations, 0u);
  }
  OS.flush();
  RecordProperty("matrix_accounting", Report);
  std::cout << "[ PR231 matrix accounting ]\n" << Report;

  // The entry-registration mechanism, investigated rather than assumed: the
  // first 4 entries above each own two lifecycle names, which is what fills the 8
  // process-global dimType slots. Entries that SHARE a lifecycle pair consume no
  // new dimType slot, so the supported entry count is not 4: two more entries
  // over pairs 0 and 1 are registered, planned, published and dispatched over the
  // SAME 6 x 20 window here, for 6 entries x 6 cells x 20 TRPs in total.
  EXPECT_EQ(EJitLifecycleRegistry::instance().count(), kEntries * 2u)
      << "four distinct pairs fill the 8 process-global dimType slots";
  for (unsigned E = 0; E < kSharedEntries; ++E) {
    const unsigned Pair = E % kEntries;
    const std::string Entry = "m_entry_shared_" + std::to_string(E);
    auto M = parseHost(Entry, "g_matrix", "g_matrix_out", kCeilingCells,
                       kCeilingTrps);
    ASSERT_TRUE(M) << Entry;
    auto F = std::make_shared<EJitSmallTableHostFactSource>(
        "g_matrix", &g_matrix[0][0], sizeof(g_matrix), 0x6F00 + E);
    for (unsigned C = 0; C < kMatrixWindowCells; ++C)
      for (unsigned T = 0; T < kMatrixTrps; ++T)
        F->addReadyMember({C, T}, 0x6F00 + E * 256 + C * 32 + T);
    Config Cfg;
    auto H = EJitSmallTableHost::create(Cfg, State.getRegistry(), State, F,
                                        EJitSmallTableHost::Options{});
    ASSERT_TRUE(static_cast<bool>(H)) << toString(H.takeError());
    auto HostPtr = std::move(*H);
    EJitSmallTableHost::EntryRequest Req;
    Req.module = M.get();
    Req.entryName = Entry;
    Req.funcIndex = EJitFuncRegistry::instance().resolveAssign(Entry);
    Req.sourceVarName = "g_matrix";
    SmallVector<EJitSmallTableDim, 2> D =
        hostDims(kCeilingCells, kCeilingTrps);
    Req.dims = D;
    // The SAME lifecycle names as an earlier entry: no new dimType slot.
    std::vector<std::string> P = {CellPeriods[Pair], TrpPeriods[Pair]};
    Req.dimPeriodNames = P;
    Req.codeGeneration = 99 + E;
    const uint32_t DimTypeSlotsBefore =
        EJitLifecycleRegistry::instance().count();
    std::string Why;
    Error ReqErr =
        HostPtr->requestEntry(Req, reinterpret_cast<void *>(&aotFocusEntry), Why);
    ASSERT_FALSE(static_cast<bool>(ReqErr))
        << Entry << ": " << toString(std::move(ReqErr)) << " (" << Why << ")";
    EXPECT_NE(Req.funcIndex, kEJitInvalidFuncIndex)
        << "the ENTRY registry, not the dimType registry, bounds entry count";
    EXPECT_EQ(EJitLifecycleRegistry::instance().count(), DimTypeSlotsBefore)
        << "an entry sharing another entry's lifecycle pair must consume no new "
           "dimType slot";
    ASSERT_TRUE(HostPtr->driveSampling(4096, 1) > 0) << Entry;
    ASSERT_FALSE(static_cast<bool>(HostPtr->publishGeneration(Why)))
        << Entry << ": " << Why;
    EXPECT_EQ(HostPtr->dimTypes()[0],
              EJitLifecycleRegistry::instance().lookup(CellPeriods[Pair]));
    EXPECT_EQ(HostPtr->dimTypes()[1],
              EJitLifecycleRegistry::instance().lookup(TrpPeriods[Pair]));
    EXPECT_EQ(HostPtr->publishedSlots(), kMatrixWindowCells * kMatrixTrps);
    // The same 6 x 20 window through the shared entry's own dispatch.
    for (unsigned C = 0; C < kMatrixWindowCells; ++C)
      for (unsigned T = 0; T < kMatrixTrps; ++T) {
        const uint32_t DS[2] = {HostPtr->dimTypes()[0], HostPtr->dimTypes()[1]};
        const uint32_t IS[2] = {C, T};
        const auto R = HostPtr->dispatch(DS, IS, 2);
        ASSERT_EQ(R.status, EJitSmallTableDispatch::Dispatched)
            << Entry << ": " << R.why;
        EXPECT_EQ(R.value, aotResult(g_matrix[C][T], 2))
            << Entry << " cell=" << C << " trp=" << T;
      }
  }
}

} // namespace
