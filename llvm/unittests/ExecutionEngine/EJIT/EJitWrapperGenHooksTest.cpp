//===-- EJitWrapperGenHooksTest.cpp - PR231 wrapper-hook evidence ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The AOT wrapper's small-table hooks (`ejit_stab_enter` / `ejit_stab_leave`)
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
//   Cross-cutting: `verifyModule` must accept every shape, and the refusal edge
//   must settle the taskpool read token that the successful compile_or_get
//   handed the wrapper before it enters the AOT body.
//
//   Part B - a real generated wrapper is executed. The wrapped module is
//   JIT-compiled with LLJIT and its entry is called with real arguments; a real
//   EJitSmallTableHost is registered through the ordinary request path, driven
//   through common-T1 completion/freeze, T2 publication, a configuration move
//   and generation retirement. The wrapper's own call decides between the
//   published specialized entry and its AOT body, so the assertion values are
//   what distinguish the paths.
//
// Evidence boundary (recorded, not hidden): the local harness cannot bring up a
// live taskpool compile, so Part B asserts the hook contract on the ICACHE HIT
// path (the probe resolves the callable itself; a hit needs no taskpool compile)
// and the compile_or_get-success path is covered by Part A's generated IR plus
// the fail-closed execution assertions. The runtime C ABI symbols the wrapper
// calls are the process's REAL implementations, wired into the JIT like the
// product image's own symbol table; nothing here imitates wrapper code, and no
// logical slot is published by the test.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableHost.h"
#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "gtest/gtest.h"
#include <cstring>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

// The pass options the tests pin directly. All are non-static for exactly this
// reason: no opt/lit is available in this checkout, so the shapes must be
// selected in-process.
extern cl::opt<bool> EnableEJitSmallTableHooks;
extern cl::opt<bool> EJitInlineCache;
extern cl::opt<bool> EnableEJitGlobalCtors;

namespace {

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

// --- Path 1: the compile_or_get-success dispatch (-ejit-inline-cache OFF) ---

TEST_F(WrapperGenIRTest, PlainDispatchHooksOffIsTheBaselineShape) {
  const std::string IR = wrap(/*Hooks=*/false, /*Icache=*/false);
  ASSERT_FALSE(IR.empty());
  EXPECT_FALSE(contains(IR, "ejit_stab_enter"));
  EXPECT_FALSE(contains(IR, "ejit_stab_leave"));
  EXPECT_FALSE(contains(IR, "@__ejit_icache_fn_f_entry"));
  // The baseline dispatch: resolve, call the resolved pointer, release the read.
  EXPECT_EQ(countOccurrences(IR, "call i32 @ejit_taskpool_compile_or_get_1d"),
            1u);
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_taskpool_release_read"), 1u);
}

TEST_F(WrapperGenIRTest, PlainDispatchHooksOnGatesAfterASuccessfulResolve) {
  const std::string IR = wrap(/*Hooks=*/true, /*Icache=*/false);
  ASSERT_FALSE(IR.empty());
  ASSERT_TRUE(contains(IR, "declare ptr @ejit_stab_enter")) << IR;
  ASSERT_TRUE(contains(IR, "declare void @ejit_stab_leave")) << IR;

  // The gate is reached only through a successful compile_or_get, and its three
  // answers are the three blocks: admitted (jit_stab_go), refused
  // (jit_stab_refuse), no policy (jit_stab_call).
  EXPECT_TRUE(contains(IR, "jit_stab_decide"));
  EXPECT_TRUE(contains(IR, "jit_stab_go"));
  EXPECT_TRUE(contains(IR, "jit_stab_refuse"));
  EXPECT_TRUE(contains(IR, "jit_stab_call"));
  EXPECT_EQ(countOccurrences(IR, "call ptr @ejit_stab_enter"), 1u);
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_stab_leave"), 1u);
  // The reason load decides refused vs no-policy.
  EXPECT_TRUE(contains(IR, "ejit_stab_why_v"));
  // The callable is loaded from the per-call slot, not taken from the resolve
  // directly: an admitted execution runs the PUBLISHED entry.
  EXPECT_TRUE(contains(IR, "ejit_callee"));

  // The resolve still happens exactly once, and the read token is released
  // exactly once on EACH path that holds one: the specialized path (after the
  // call) and the refusal path (before the AOT body). Path 1 therefore has two
  // release sites; the AOT body itself holds no token.
  EXPECT_EQ(countOccurrences(IR, "call i32 @ejit_taskpool_compile_or_get_1d"),
            1u);
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_taskpool_release_read"), 2u);
}

// --- Path 2: the inline-cache hit path ------------------------------------

TEST_F(WrapperGenIRTest, IcacheHitIsFramelessOnlyWithoutHooks) {
  const std::string IR = wrap(/*Hooks=*/false, /*Icache=*/true);
  ASSERT_FALSE(IR.empty());
  EXPECT_FALSE(contains(IR, "ejit_stab_enter"));
  // Sentinel-formed 1D table: the wrapper is the probe plus one musttail BLR on
  // the loaded cell and has no guarded dispatch block at all.
  EXPECT_TRUE(contains(IR, "musttail call i32 %ejit_ic_fn")) << IR;
  EXPECT_FALSE(contains(IR, "jit_icache_dispatch"));
  EXPECT_FALSE(contains(IR, "jit_miss"));
}

TEST_F(WrapperGenIRTest, IcacheHitWithHooksIsFramedAndPaired) {
  const std::string IR = wrap(/*Hooks=*/true, /*Icache=*/true);
  ASSERT_FALSE(IR.empty());
  // The frame-less hit form is gone: it cannot close the execution after the
  // call. The miss tail call into MissFn is a different call site and stays.
  EXPECT_FALSE(contains(IR, "musttail call i32 %ejit_ic_fn")) << IR;
  EXPECT_TRUE(contains(IR, "call i32 %ejit_hit_callee")) << IR;
  EXPECT_TRUE(contains(IR, "musttail call i32 @f_entry_miss")) << IR;
  EXPECT_TRUE(contains(IR, "jit_icache_dispatch"));
  EXPECT_TRUE(contains(IR, "jit_miss"));
  EXPECT_TRUE(contains(IR, "jit_stab_hit_call"));
  EXPECT_TRUE(contains(IR, "jit_stab_hit_go"));
  // Exactly one enter and one leave per gated dispatch: the hit path and the
  // miss function's own dispatch. The refusal edge of the hit path is the miss
  // function, which re-resolves through its own gated dispatch.
  EXPECT_EQ(countOccurrences(IR, "call ptr @ejit_stab_enter"), 2u);
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_stab_leave"), 2u);
  EXPECT_TRUE(contains(IR, "ejit_hit_callee"));
  EXPECT_EQ(countOccurrences(IR, "call i32 @ejit_taskpool_compile_or_get_1d"),
            1u);
  EXPECT_EQ(countOccurrences(IR, "call void @ejit_taskpool_release_read"), 2u);
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

/// The executed wrapper: the generated module is JIT-compiled and its entry is
/// the callable the application would call. `g_wrap`/`g_wrap_out` are the
/// module's globals and the runtime C ABI symbols are the process's REAL
/// implementations, wired in like the product image's symbol table.
class GeneratedWrapper {
public:
  static Expected<std::unique_ptr<GeneratedWrapper>>
  create(uint32_t FuncIdx, uint32_t CellSlot, bool Hooks) {
    auto W = std::unique_ptr<GeneratedWrapper>(new GeneratedWrapper());

    // The AOT image: the generated wrapper PLUS the per-function globals the AOT
    // registration fills, and the inline-cache cell table.
    std::string Text = wrapperModuleText(FuncIdx, CellSlot);
    auto AotModule = parseModule(W->ctx_, Text, "GeneratedWrapper");
    if (!AotModule)
      return make_error<StringError>("the AOT fixture module did not parse",
                                     inconvertibleErrorCode());
    // The registered bitcode: the SAME entry WITHOUT the wrapper, which is what
    // the runtime compiles specializations from (a product registers the
    // pre-wrapper body). Kept alive for the host's request.
    W->compileModule_ = parseModule(
        W->ctx_, entryBodyText("g_wrap", "g_wrap_out", kWrapCells),
        "GeneratedWrapper.body");
    if (!W->compileModule_)
      return make_error<StringError>("the body module did not parse",
                                     inconvertibleErrorCode());

    bool SavedHooks = EnableEJitSmallTableHooks;
    bool SavedIcache = EJitInlineCache;
    bool SavedCtors = EnableEJitGlobalCtors;
    EnableEJitSmallTableHooks = Hooks;
    EJitInlineCache = true;
    // No JIT'd constructors on this host: the per-function globals are already
    // seeded with the values the REAL ejit_register_lifecycle /
    // ejit_register_funcindex assign by name (see wrapperModuleText), so the
    // registration a ctor would perform is not needed to run the wrapper.
    EnableEJitGlobalCtors = false;
    Analyses A;
    W->wrappedIR_ = runWrapperGen(*AotModule, A);
    EnableEJitGlobalCtors = SavedCtors;
    EnableEJitSmallTableHooks = SavedHooks;
    EJitInlineCache = SavedIcache;

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
    if (Error E = defineRuntimeSymbols(*W->jit_))
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
    return std::move(W);
  }

  /// The registered-bitcode module (the entry body the runtime compiles
  /// specializations from), kept alive for the host's request.
  Module *compileModule() const { return compileModule_.get(); }

  int32_t call(int32_t Cell, int32_t X) const { return entry_(Cell, X); }

  StringRef wrappedIR() const { return wrappedIR_; }

private:
  static std::string wrapperModuleText(uint32_t FuncIdx, uint32_t CellSlot) {
    std::string T = hostTargetHeader();
    T += "\n    %A = type { i32, i32 }\n";
    T += "    @g_wrap = external global [" + Twine(kWrapCells).str() +
         " x %A]\n";
    T += "    @g_wrap_out = external global [" + Twine(kWrapCells).str() +
         " x i32]\n";
    // The AOT registration values, pinned here because the JIT'd module's
    // constructors are not guaranteed to run on this host: the dense funcIndex
    // and the lifecycle dimType are the SAME values the real
    // ejit_register_funcindex / ejit_register_lifecycle assign by name.
    T += "    @__ejit_funcidx_f_entry = internal global i32 " +
         Twine(FuncIdx).str() + "\n";
    T += "    @__ejit_dimtype_" + std::string(kWrapPeriod) +
         " = internal global i32 " + Twine(CellSlot).str() + "\n";
    // The AOT image's inline-cache table. Every cell holds a real callable so a
    // probe for any coordinate of the declared window takes the HIT path (the
    // path under test).
    T += "    @__ejit_icache_fn_f_entry = internal global [16 x ptr] [";
    for (unsigned I = 0; I < 16; ++I) {
      if (I)
        T += ", ";
      T += "ptr @wrap_cell_callable";
    }
    T += "]\n";
    T += "    declare i32 @wrap_cell_callable(i32, i32)\n";
    // The entry body itself: the same text the runtime compiles from.
    const std::string Full = entryBodyText("g_wrap", "g_wrap_out", kWrapCells);
    const size_t Body = Full.find("    define i32 @f_entry");
    T += Full.substr(Body);
    return T;
  }

  static Error defineRuntimeSymbols(orc::LLJIT &J) {
    orc::SymbolMap Syms;
    auto &ES = J.getExecutionSession();
    auto Add = [&](StringRef Name, const void *Addr) {
      Syms[ES.intern(Name)] = JITEvaluatedSymbol(
          pointerToJITTargetAddress(Addr),
          JITSymbolFlags::Exported | JITSymbolFlags::Callable);
    };
    // The module's own globals.
    Add("g_wrap", static_cast<const void *>(&g_wrap[0].mode));
    Add("g_wrap_out", static_cast<const void *>(&g_wrap_out[0]));
    Add("wrap_cell_callable",
        reinterpret_cast<const void *>(&wrapCellCallable));
    // The REAL runtime C ABI the generated wrapper calls: the process
    // implementations, not stand-ins, exactly as a product image exports them
    // to its AOT wrappers.
    Add("ejit_taskpool_compile_or_get_1d",
        reinterpret_cast<const void *>(&ejit_taskpool_compile_or_get_1d));
    Add("ejit_taskpool_release_read",
        reinterpret_cast<const void *>(&ejit_taskpool_release_read));
    Add("ejit_stab_enter", reinterpret_cast<const void *>(&ejit_stab_enter));
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
  std::string wrappedIR_;
};

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

  void SetUp() override {
    // The wrapper's funcIndex/dimType assignment is process-global and
    // single-assignment: start from empty registries like a fresh image.
    EJitLifecycleRegistry::instance().reset();
    EJitFuncRegistry::instance().reset();
    prevGlobal_ = EJitSmallTableHost::installGlobal(nullptr);
    fillWrapConfig();
    g_cellCalls = 0;
    State.getRegistry().registerStaticVar("g_wrap", &g_wrap[0].mode);
    State.getRegistry().registerStaticVar("g_wrap_out", &g_wrap_out[0]);
    FuncIdx = EJitFuncRegistry::instance().resolveAssign("f_entry");
    CellSlot = EJitLifecycleRegistry::instance().resolveAssign(kWrapPeriod);
    ASSERT_NE(FuncIdx, kEJitInvalidFuncIndex);
    ASSERT_NE(CellSlot, kEJitInvalidDimType);
  }

  void TearDown() override {
    if (Host) {
      if (EJitSmallTableHost::global() == Host.get())
        EJitSmallTableHost::installGlobal(nullptr);
      Host.reset();
    }
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

  bool makeHost() {
    auto H = EJitSmallTableHost::create(Config(), State.getRegistry(), State,
                                        Facts, EJitSmallTableHost::Options{});
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

  /// Common-T1 completion + freeze + T2 + publication, through the runtime's
  /// own sampling protocol (the instrumented tier is the pre-publication tier by
  /// design: the ordinary `enter` refuses until code is published, so the window
  /// cannot be filled by the wrapper hook).
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
};

TEST_F(GeneratedWrapperTest, GeneratedWrapperCallsTheAdmittedPublishedEntry) {
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/true));
  // The generated IR really is the hooked shape (not a hand-written imitation).
  ASSERT_TRUE(Wrapper->wrappedIR().contains("call ptr @ejit_stab_enter"))
      << Wrapper->wrappedIR();
  ASSERT_TRUE(makeHost());
  Host->setInvalidationHook([]() {});
  ASSERT_TRUE(requestEntry());

  // --- 1. Bound, but no published code yet: fail closed. The wrapper's own call
  // must return the live-memory AOT value and must not run the cache cell's
  // callable: a refusal is never a silent fall-through to a resolved pointer.
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_FALSE(Host->wouldDispatch({0}));
  const int32_t X = 2;
  EXPECT_EQ(Wrapper->call(0, X), wrapAotResult(g_wrap[0], X));
  EXPECT_EQ(g_cellCalls, 0u)
      << "a refused call ran the cache cell instead of the AOT body";
  EXPECT_EQ(Host->activeExecutions(), 0u)
      << "the refused path left an execution in flight";

  // --- 2. Common T1 completes, the session freezes, T2 compiles from the
  // immutable bundle and the admitted slots are published.
  ASSERT_TRUE(driveAndPublish());
  ASSERT_EQ(Host->publishedSlots(), kWrapReadyCells);
  EXPECT_TRUE(Host->codeReady());
  ASSERT_NE(Host->bundle(), nullptr);
  const uint64_t PublishedResourceGen = Host->publishedResourceGeneration();
  ASSERT_NE(Host->publishedCodeGeneration(), 0u);
  ASSERT_NE(PublishedResourceGen, 0u);

  // --- 3. The discriminator. The configuration moves AFTER publication: live
  // memory now differs from the frozen bundle, so the AOT body and the published
  // specialization return different values. Only the published entry can return
  // the frozen one.
  for (unsigned C = 0; C < kWrapCells; ++C)
    g_wrap[C].bycell += 500;
  const int32_t Frozen = wrapAotResult(WrapElement{1, 10}, X); // cell 0, frozen
  const int32_t Live = wrapAotResult(g_wrap[0], X);
  ASSERT_NE(Frozen, Live) << "the fixture does not discriminate the paths";

  EXPECT_TRUE(Host->wouldDispatch({0}));
  EXPECT_EQ(Wrapper->call(0, X), Frozen)
      << "the generated wrapper did not run the admitted published entry";
  EXPECT_EQ(g_cellCalls, 0u)
      << "the wrapper ran its cache cell instead of the published entry";
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
  EXPECT_FALSE(Host->wouldDispatch({kWrapReadyCells}));
  EXPECT_EQ(Wrapper->call(kWrapReadyCells, X),
            wrapAotResult(g_wrap[kWrapReadyCells], X))
      << "an unconfirmed row must stay AOT";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->activeExecutions(), 0u);

  // --- 4. The configuration moves: published slots are retracted, so the
  // wrapper refuses again and the AOT body (live memory) is what runs. The old
  // generation can then be retired.
  Host->noteConfigurationChange("test: configuration moved");
  EXPECT_EQ(Host->publishedSlots(), 0u);
  EXPECT_EQ(Wrapper->call(0, X), Live)
      << "a drained generation must not be reachable from the wrapper";
  EXPECT_EQ(g_cellCalls, 0u);
  EXPECT_EQ(Host->activeExecutions(), 0u);
  std::string Why;
  EXPECT_TRUE(Host->retireGenerationsUpTo(PublishedResourceGen, Why)) << Why;
}

TEST_F(GeneratedWrapperTest, HooksOnWithNoHostKeepsTheUnchangedDispatch) {
  // The default-off product state: the wrapper is built WITH the hooks but no
  // small-table host is installed. `ejit_stab_enter` must answer "no policy", so
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
  void *Entry = ejit_stab_enter(FuncIdx, &Dim, 1, &Ticket, &Reason);
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

TEST_F(GeneratedWrapperTest, HooksOffWrapperIsTheUnchangedWrapper) {
  // Same fixture, hooks OFF: the generated wrapper has no hook call at all, and
  // its result is the live-memory AOT value (the sentinel-formed table sends the
  // probe into MissFn, whose resolve cannot succeed without a live taskpool).
  // This is the default-off regression guard.
  ASSERT_TRUE(makeWrapperAndFacts(/*Hooks=*/false));
  EXPECT_FALSE(Wrapper->wrappedIR().contains("ejit_stab_enter"));
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

} // namespace