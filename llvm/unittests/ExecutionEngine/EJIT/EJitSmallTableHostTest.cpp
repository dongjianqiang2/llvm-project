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
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h" // the wrapper C ABI hooks
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableHost.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

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
                           unsigned Cells, unsigned Trps) {
  const std::string CA = Twine(Cells).str();
  const std::string TA = Twine(Trps).str();
  std::string Text = hostTargetHeader();
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
  Text += "      ret i32 %res\n";
  Text += "    }\n\n    !0 = !{!2}\n    !1 = !{}\n";
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
                                    unsigned Trps) {
    SMDiagnostic Err;
    auto M = parseAssemblyString(hostModuleText(Entry, Global, Out, Cells, Trps),
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
                         unsigned ReadyTrps = kTrps) {
    auto M = parseHost("f_entry", "g_focus", "g_focus_out", Cells, kTrps + 1);
    if (!M)
      return false;
    Modules.push_back(std::move(M));
    Facts = makeFacts("g_focus", &g_focus[0][0], sizeof(g_focus), 0xF00D,
                      Cells, Trps, ReadyCells, ReadyTrps);
    if (!makeHost(Facts))
      return false;
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
      // Not counted (budget reached): no execution was opened.
      return true;
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

TEST_F(SmallTableHostTest, BoundEntryStaysAotUntilCodeIsReady) {
  ASSERT_TRUE(requestFocusEntry());
  // The plan exists and the members are admitted, but no code generation is
  // published yet: every dispatch must take the AOT path (fail closed), and the
  // AOT path is exactly what the real baseline computes.
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_EQ(Host->activeTier(), "instrumented");
  EXPECT_FALSE(Host->codeReady());
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T) {
      std::string Why;
      EXPECT_FALSE(Host->wouldDispatch({C, T}, &Why)) << Why;
      const auto R = call(C, T, 2);
      EXPECT_EQ(R.status, EJitSmallTableDispatch::Aot) << R.why;
      EXPECT_FALSE(R.counted);
    }
  EXPECT_EQ(Host->runtime().currentSessionSamples(), 0u)
      << "a refused call never consumes the aggregate budget";
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

  // An execution above the aggregate budget still runs the correct code but is
  // not counted.
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
  EXPECT_EQ(Host->activeExecutions(), 0u)
      << "cancelling drains the logical slots and closes the executions";
  EXPECT_EQ(Facts->outstandingBorrows(), 0u)
      << "cancelling must release the cancelled execution's protected read";
  EXPECT_FALSE(Host->codeReady());

  // Completing the granted execution now is a stale completion: it is counted,
  // never merged into a later generation.
  const uint64_t Before = Host->staleLeaveCount();
  Host->leave(Ticket);
  EXPECT_EQ(Host->staleLeaveCount(), Before + 1)
      << "a ticket from a cancelled session must be recognized as stale";
  EXPECT_GT(Host->runtime().stats().staleCallbacks, 0u)
      << "the runtime must record the stale sample callback too";

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
