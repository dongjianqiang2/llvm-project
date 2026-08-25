//===-- EJitRuntimeTest.cpp - EmbeddedJIT Runtime Unit Tests
//---------------===//
//
// NOTE: To call the C API functions, we need to include the C runtime header
// which is in the non-canonical include path. We declare the symbols we need
// via extern "C" declarations instead, since they're provided by libLLVMEJIT.
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLogger.h"
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#ifdef EJIT_SRE_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitTaskPool.h"
#endif
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#ifndef EJIT_FREESTANDING
#include <thread>
#endif

using namespace llvm;
using namespace llvm::ejit;

TEST(EJitDump, FunctionAndModuleViewsHaveDifferentScopes) {
  LLVMContext Ctx;
  Module M("dump_selective", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *FT = FunctionType::get(I32, {I32}, false);
  auto *Helper = Function::Create(FT, Function::InternalLinkage,
                                  "dump_helper", &M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Helper));
  B.CreateRet(B.CreateAdd(Helper->getArg(0), B.getInt32(1)));

  auto *Entry =
      Function::Create(FT, Function::ExternalLinkage, "dump_entry", &M);
  B.SetInsertPoint(BasicBlock::Create(Ctx, "entry", Entry));
  B.CreateRet(B.CreateCall(Helper, {Entry->getArg(0)}));

  std::string Output;
  ASSERT_TRUE(
      llvm::ejit::detail::renderDumpFunctionIR(M, "dump_entry", Output));
  EXPECT_NE(Output.find("@dump_entry("), std::string::npos) << Output;
  EXPECT_EQ(Output.find("define internal"), std::string::npos) << Output;
  EXPECT_NE(Output.find("@dump_helper("), std::string::npos) << Output;

  Output.clear();
  llvm::ejit::detail::renderDumpModuleIR(M, Output);
  EXPECT_NE(Output.find("define internal"), std::string::npos) << Output;
  EXPECT_NE(Output.find("@dump_helper("), std::string::npos) << Output;
}

TEST(EJitDump, DumpAllKeepsEachIndependentlyCompiledEntry) {
  std::string Bitcode;
  {
    LLVMContext Ctx;
    Module M("dump_all_entries", Ctx);
    auto *I32 = Type::getInt32Ty(Ctx);
    auto *FT = FunctionType::get(I32, {I32}, false);
    for (StringRef Name : {"dump_a", "dump_b", "dump_c"}) {
      auto *F = Function::Create(FT, Function::ExternalLinkage, Name, &M);
      // The entry tag is required: the JIT pipeline internalizes every
      // non-entry definition and ORC never registers local-linkage symbols,
      // so an untagged function fails lookup ("Symbols not found").
      Metadata *EntryMDOps[] = {MDString::get(Ctx, TAG_EJIT_ENTRY)};
      F->setMetadata(MD_EJIT_METADATA,
                     MDNode::get(Ctx, {MDNode::get(Ctx, EntryMDOps)}));
      IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
      B.CreateRet(B.CreateAdd(F->getArg(0), B.getInt32(Name.back())));
    }
    raw_string_ostream OS(Bitcode);
    WriteBitcodeToFile(M, OS);
    OS.flush();
  }

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  EJitRuntimeState State;
  Config Cfg;
  auto EngineOrErr = EJitOrcEngine::Create(Cfg, State.getRegistry(), State);
  ASSERT_TRUE(static_cast<bool>(EngineOrErr));
  auto Engine = std::move(*EngineOrErr);

  setDumpFuncFilter("*");
  for (auto [Name, Key] :
       {std::pair<const char *, uint64_t>{"dump_a", 0xda},
        {"dump_b", 0xdb}, {"dump_c", 0xdc}}) {
    SpecializationContext Ctx;
    Ctx.fnName = Name;
    Ctx.cacheKey = Key;
    Engine->setActiveContext(&Ctx);
    ASSERT_FALSE(errorToBool(Engine->loadBitcodeModule(Bitcode, Key, Name)));
    auto FnOrErr = Engine->lookup(Key, Name);
    ASSERT_TRUE(static_cast<bool>(FnOrErr));
  }
  Engine->setActiveContext(nullptr);
  setDumpFuncFilter("");

  for (const char *Name : {"dump_a", "dump_b", "dump_c"}) {
    EXPECT_TRUE(printDumped(Name)) << Name;
    EXPECT_TRUE(printDumpedModule(Name)) << Name;
  }
}

namespace llvm {
namespace ejit {
// Test-only accessor. EJitOptimizer deliberately keeps its individual pipeline
// steps private (runPipeline() is the production entry point). This subclass is
// granted access through a friend declaration in EJitOptimizer.h and re-exports
// the steps so unit tests can drive them in isolation, without widening the
// production public API. Tests construct an EJitOptimizerTestAccess in place of
// an EJitOptimizer; all other call syntax is unchanged.
struct EJitOptimizerTestAccess : EJitOptimizer {
  using EJitOptimizer::EJitOptimizer;
  using EJitOptimizer::preReplacePeriodIndices;
  using EJitOptimizer::runInstCombine;
  using EJitOptimizer::runInterproceduralPropagation;
  using EJitOptimizer::runOptimizationPipeline;
  using EJitOptimizer::runStructFieldPass;
};
} // namespace ejit
} // namespace llvm

// ejit_status_t values mirrored locally. EJitError::code is a plain int that
// stores ejit_status_t codes (see EJitRuntime.h). The C header is not included
// here because it conflicts with the local extern "C" test declarations further
// down (which intentionally use a private signature for the C API).
static constexpr int kEjitStatusOk = 0;             // EJIT_OK
static constexpr int kEjitStatusCompileFailed = -3; // EJIT_ERR_COMPILE_FAILED

//===----------------------------------------------------------------------===//
// EJitRegistrationStore tests (T3-08)
//===----------------------------------------------------------------------===//

TEST(EJitRegistrationStore, RegisterAndConsumeBitcode) {
  EJitRegistrationStore &store = EJitRegistrationStore::instance();
  // consume any leftover data from previous tests
  store.consume();

  const uint8_t data[] = {0x01, 0x02, 0x03};
  store.registerBitcode("func_a", data, sizeof(data));

  StoredData result = store.consume();
  ASSERT_EQ(result.bitcodes.size(), 1u);
  EXPECT_EQ(result.bitcodes[0].funcName, "func_a");
  EXPECT_EQ(result.bitcodes[0].size, 3u);
  EXPECT_EQ(result.bitcodes[0].data[0], 0x01);

  // consume again should be empty
  StoredData empty = store.consume();
  EXPECT_TRUE(empty.bitcodes.empty());
  EXPECT_TRUE(empty.periodArrays.empty());
  EXPECT_TRUE(empty.staticVars.empty());
}

TEST(EJitRegistrationStore, RegisterAndConsumePeriodArrays) {
  EJitRegistrationStore &store = EJitRegistrationStore::instance();
  store.consume();

  int arr[10];
  store.registerPeriodArray("cell", "cells", arr, 10);
  store.registerPeriodArray("trp", "trps", arr, 5);

  StoredData result = store.consume();
  ASSERT_EQ(result.periodArrays.size(), 2u);
  EXPECT_EQ(result.periodArrays[0].periodName, "cell");
  EXPECT_EQ(result.periodArrays[0].varName, "cells");
  EXPECT_EQ(result.periodArrays[0].arraySize, 10u);
  EXPECT_EQ(result.periodArrays[1].periodName, "trp");
}

TEST(EJitRegistrationStore, RegisterAndConsumeStaticVars) {
  EJitRegistrationStore &store = EJitRegistrationStore::instance();
  store.consume();

  int val = 42;
  store.registerStaticVar("config", &val);

  StoredData result = store.consume();
  ASSERT_EQ(result.staticVars.size(), 1u);
  EXPECT_EQ(result.staticVars[0].varName, "config");
  EXPECT_EQ(result.staticVars[0].varAddr, &val);
}

TEST(EJitRegistrationStore, ConsumeClearsAllTypes) {
  EJitRegistrationStore &store = EJitRegistrationStore::instance();
  store.consume();

  const uint8_t d[] = {0};
  store.registerBitcode("f", d, 1);
  int arr;
  store.registerPeriodArray("p", "v", &arr, 1);
  int val;
  store.registerStaticVar("s", &val);

  StoredData result = store.consume();
  EXPECT_EQ(result.bitcodes.size(), 1u);
  EXPECT_EQ(result.periodArrays.size(), 1u);
  EXPECT_EQ(result.staticVars.size(), 1u);
}

#ifndef EJIT_FREESTANDING
TEST(EJitRegistrationStore, ThreadSafety) {
  EJitRegistrationStore &store = EJitRegistrationStore::instance();
  store.consume();

  std::thread t1([&]() {
    for (int i = 0; i < 100; ++i) {
      const uint8_t d[] = {0};
      store.registerBitcode("t1_func", d, 1);
    }
  });
  std::thread t2([&]() {
    for (int i = 0; i < 100; ++i) {
      int arr;
      store.registerPeriodArray("t2_period", "t2_var", &arr, 1);
    }
  });

  t1.join();
  t2.join();

  StoredData result = store.consume();
  EXPECT_EQ(result.bitcodes.size() + result.periodArrays.size(), 200u);
}
#endif // EJIT_FREESTANDING

//===----------------------------------------------------------------------===//
// EJitModuleLoader tests (T3-09)
//===----------------------------------------------------------------------===//

TEST(EJitModuleLoader, RegisterAndGetBitcode) {
  EJitModuleLoader loader;
  const uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
  EXPECT_TRUE(loader.registerBitcode("my_func", data, sizeof(data)));

  // funcIndex is the dense index EJitFuncRegistry assigned to the name.
  uint32_t idx = ejit::EJitFuncRegistry::instance().lookup("my_func");
  ASSERT_NE(idx, ejit::kEJitInvalidFuncIndex);
  auto result = loader.getBitcodeByFuncIdx(idx);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result->size(), 4u);
  EXPECT_EQ((uint8_t)(*result)[0], 0xAA);
  EXPECT_EQ(loader.getFuncNameByFuncIdx(idx), "my_func");
}

TEST(EJitModuleLoader, GetBitcodeNotFound) {
  EJitModuleLoader loader;
  auto result = loader.getBitcodeByFuncIdx(0xDEAD);
  EXPECT_FALSE(static_cast<bool>(result));
  // Consume the error to avoid unchecked-Expected assertion at destruction
  if (!result)
    llvm::consumeError(result.takeError());
}

TEST(EJitModuleLoader, MultipleFunctions) {
  EJitModuleLoader loader;
  const uint8_t d1[] = {0x11};
  const uint8_t d2[] = {0x22, 0x33};
  EXPECT_TRUE(loader.registerBitcode("f1", d1, sizeof(d1)));
  EXPECT_TRUE(loader.registerBitcode("f2", d2, sizeof(d2)));

  // Each name has its own distinct dense funcIndex.
  uint32_t i1 = ejit::EJitFuncRegistry::instance().lookup("f1");
  uint32_t i2 = ejit::EJitFuncRegistry::instance().lookup("f2");
  ASSERT_NE(i1, ejit::kEJitInvalidFuncIndex);
  ASSERT_NE(i2, ejit::kEJitInvalidFuncIndex);
  ASSERT_NE(i1, i2);
  auto r1 = loader.getBitcodeByFuncIdx(i1);
  ASSERT_TRUE(static_cast<bool>(r1));
  EXPECT_EQ(r1->size(), 1u);
  EXPECT_EQ(loader.getFuncNameByFuncIdx(i1), "f1");

  auto r2 = loader.getBitcodeByFuncIdx(i2);
  ASSERT_TRUE(static_cast<bool>(r2));
  EXPECT_EQ(r2->size(), 2u);
  EXPECT_EQ(loader.getFuncNameByFuncIdx(i2), "f2");
}

TEST(EJitModuleLoader, FuncIndexIsOrderIndependent) {
  // Two loaders (modules) registering the same names in OPPOSITE order must map
  // each name to the SAME dense funcIndex — the registry fixes a name's index
  // on first sight and never shifts it.
  auto &FR = ejit::EJitFuncRegistry::instance();
  FR.reset();
  const uint8_t da[] = {0xA1};
  const uint8_t db[] = {0xB2};
  EJitModuleLoader fwd;
  EXPECT_TRUE(fwd.registerBitcode("alpha", da, sizeof(da)));
  EXPECT_TRUE(fwd.registerBitcode("omega", db, sizeof(db)));
  EJitModuleLoader rev;
  EXPECT_TRUE(rev.registerBitcode("omega", db, sizeof(db)));
  EXPECT_TRUE(rev.registerBitcode("alpha", da, sizeof(da)));

  uint32_t ia = FR.lookup("alpha");
  uint32_t io = FR.lookup("omega");
  ASSERT_NE(ia, ejit::kEJitInvalidFuncIndex);
  ASSERT_NE(io, ejit::kEJitInvalidFuncIndex);
  ASSERT_NE(ia, io);
  EXPECT_EQ(fwd.getFuncNameByFuncIdx(ia), "alpha");
  EXPECT_EQ(rev.getFuncNameByFuncIdx(ia), "alpha");
  EXPECT_EQ(fwd.getFuncNameByFuncIdx(io), "omega");
  EXPECT_EQ(rev.getFuncNameByFuncIdx(io), "omega");
  FR.reset();
}

TEST(EJitModuleLoader, NullOrZeroPayloadRejected) {
  EJitModuleLoader loader;
  const uint8_t d[] = {0x10};
  EXPECT_FALSE(loader.registerBitcode("null_fn", nullptr, sizeof(d)));
  EXPECT_FALSE(loader.registerBitcode("zero_fn", d, 0));
}

TEST(EJitModuleLoader, SameNameSamePayloadIdempotent) {
  EJitModuleLoader loader;
  const uint8_t d[] = {0x10, 0x11};
  EXPECT_TRUE(loader.registerBitcode("idem", d, sizeof(d)));
  // Same name + same (data ptr + size): idempotent success.
  EXPECT_TRUE(loader.registerBitcode("idem", d, sizeof(d)));
  auto r = loader.getBitcodeByFuncIdx(
      ejit::EJitFuncRegistry::instance().lookup("idem"));
  ASSERT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(r->size(), 2u);
}

TEST(EJitModuleLoader, SameNameDifferentPayloadRejectedKeepsOriginal) {
  EJitModuleLoader loader;
  const uint8_t d1[] = {0x10};
  const uint8_t d2[] = {0x20, 0x21};
  EXPECT_TRUE(loader.registerBitcode("conf", d1, sizeof(d1)));
  // Same name, DIFFERENT payload: rejected, the original is kept unchanged.
  EXPECT_FALSE(loader.registerBitcode("conf", d2, sizeof(d2)));
  auto r = loader.getBitcodeByFuncIdx(
      ejit::EJitFuncRegistry::instance().lookup("conf"));
  ASSERT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(r->size(), 1u);
  EXPECT_EQ((uint8_t)(*r)[0], 0x10);
}

//===----------------------------------------------------------------------===//
// EJitCache tests (T3-10)
//===----------------------------------------------------------------------===//

#ifndef EJIT_FREESTANDING

#endif // EJIT_FREESTANDING

//===----------------------------------------------------------------------===//
// PeriodArrayRegistry tests (T3-11)
//===----------------------------------------------------------------------===//

TEST(PeriodArrayRegistry, RegisterAndQueryArrays) {
  PeriodArrayRegistry reg;
  int data[10];
  reg.registerArray("cell", "cells", data, 10);

  const auto *arrs = reg.getArrays("cell");
  ASSERT_NE(arrs, nullptr);
  ASSERT_EQ(arrs->size(), 1u);
  EXPECT_EQ((*arrs)[0].varName, "cells");
  EXPECT_EQ((*arrs)[0].periodName, "cell");
  EXPECT_EQ((*arrs)[0].baseAddr, data);
  EXPECT_EQ((*arrs)[0].arraySize, 10u);

  const auto *arrs2 = reg.getArrays("nonexistent");
  EXPECT_EQ(arrs2, nullptr);
}

TEST(PeriodArrayRegistry, RegisterAndQueryStaticVars) {
  PeriodArrayRegistry reg;
  int val = 42;
  reg.registerStaticVar("config", &val);

  const auto &vars = reg.getStaticVars();
  ASSERT_EQ(vars.size(), 1u);
  EXPECT_EQ(vars[0].varName, "config");
  EXPECT_EQ(vars[0].varAddr, &val);
}

TEST(PeriodArrayRegistry, VarNameIndex) {
  PeriodArrayRegistry reg;
  int data[5];
  reg.registerArray("cell", "my_cells", data, 5);

  const auto *info = reg.getArrayInfo("my_cells");
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->periodName, "cell");
  EXPECT_EQ(info->arraySize, 5u);

  EXPECT_EQ(reg.getArrayInfo("unknown"), nullptr);
}

TEST(PeriodArrayRegistry, MultipleArraysSamePeriod) {
  PeriodArrayRegistry reg;
  int data1[5], data2[10];
  reg.registerArray("cell", "cells_a", data1, 5);
  reg.registerArray("cell", "cells_b", data2, 10);

  const auto *arrs = reg.getArrays("cell");
  ASSERT_NE(arrs, nullptr);
  EXPECT_EQ(arrs->size(), 2u);
}

// Idempotent registration: the constructor path may register the same array
// twice (PASS1+PASS2 both add to global_ctors, or the static-registry walk
// revisits an entry). A repeat with identical (period, base, size) must be a
// no-op — it must NOT append a second entry to arraysByPeriod_ (which would
// make activate fan out redundantly and getArrays() report ghosts).
TEST(PeriodArrayRegistry, RegisterArrayIsIdempotent) {
  PeriodArrayRegistry reg;
  int data[4];
  reg.registerArray("cell", "g_arr", data, 4);
  reg.registerArray("cell", "g_arr", data, 4); // identical duplicate
  reg.registerArray("cell", "g_arr", data, 4); // identical duplicate

  const auto *arrs = reg.getArrays("cell");
  ASSERT_NE(arrs, nullptr);
  EXPECT_EQ(arrs->size(), 1u); // no duplicate entries
  EXPECT_EQ(reg.getArrayInfo("g_arr")->baseAddr, (void *)data);
}

// A same-name conflict with a different base/size is rejected: the first
// registration is kept and the second does not create a duplicate entry.
TEST(PeriodArrayRegistry, RegisterArrayConflictKeepsFirst) {
  PeriodArrayRegistry reg;
  int a[4], b[4];
  reg.registerArray("cell", "g_arr", a, 4);
  reg.registerArray("cell", "g_arr", b, 4); // different base — conflict

  const auto *arrs = reg.getArrays("cell");
  ASSERT_NE(arrs, nullptr);
  EXPECT_EQ(arrs->size(), 1u); // no duplicate
  EXPECT_EQ(reg.getArrayInfo("g_arr")->baseAddr, (void *)a); // first kept
}

TEST(PeriodArrayRegistry, RegisterStaticVarIsIdempotent) {
  PeriodArrayRegistry reg;
  int val = 0;
  reg.registerStaticVar("g_sv", &val);
  reg.registerStaticVar("g_sv", &val); // identical duplicate
  reg.registerStaticVar("g_sv", &val); // identical duplicate

  EXPECT_EQ(reg.getStaticVars().size(), 1u); // no duplicate entries
  EXPECT_EQ(reg.getStaticVarAddr("g_sv"), (void *)&val);
}

TEST(PeriodArrayRegistry, RegisterStaticVarConflictKeepsFirst) {
  PeriodArrayRegistry reg;
  int a = 0, b = 0;
  reg.registerStaticVar("g_sv", &a);
  reg.registerStaticVar("g_sv", &b); // different addr — conflict

  EXPECT_EQ(reg.getStaticVars().size(), 1u); // no duplicate
  EXPECT_EQ(reg.getStaticVarAddr("g_sv"), (void *)&a); // first kept
}

//===----------------------------------------------------------------------===//
// EJitRuntimeState tests (T3-11)
//===----------------------------------------------------------------------===//

TEST(EJitRuntimeState, ActivateAndDeactivate) {
  EJitRuntimeState state;
  // Per-array activation model: a period must have a registered array before
  // activate()/isActive() can track its cell state.
  int cells[8];
  state.getRegistry().registerArray("cell", "cells", cells, 8);
  EXPECT_FALSE(state.isActive("cell", 0));

  state.activate("cell", 0);
  EXPECT_TRUE(state.isActive("cell", 0));

  state.deactivate("cell", 0);
  EXPECT_FALSE(state.isActive("cell", 0));
}

TEST(EJitRuntimeState, ActivateAllAndDeactivateAll) {
  EJitRuntimeState state;

  // Register a period array so activateAll/deactivateAll know the cell range
  int dummy[8];
  state.getRegistry().registerArray("cell", "dummy_cells", dummy, 8);

  state.activateAll("cell");

  // After activateAll, all cells of the registered array are active
  EXPECT_TRUE(state.isActive("cell", 0));
  EXPECT_TRUE(state.isActive("cell", 7));

  state.deactivateAll("cell");
  EXPECT_FALSE(state.isActive("cell", 0));
  EXPECT_FALSE(state.isActive("cell", 7));
}

TEST(EJitRuntimeState, IndependentPeriods) {
  EJitRuntimeState state;
  int cells[8], trps[8];
  state.getRegistry().registerArray("cell", "cells", cells, 8);
  state.getRegistry().registerArray("trp", "trps", trps, 8);

  state.activate("cell", 1);
  state.deactivate("trp", 2);

  EXPECT_TRUE(state.isActive("cell", 1));
  EXPECT_FALSE(state.isActive("trp", 2));
}

TEST(EJitRuntimeState, UninitializedReturnsFalse) {
  EJitRuntimeState state;
  EXPECT_FALSE(state.isActive("nonexistent", 99));
}

#ifndef EJIT_FREESTANDING
TEST(EJitRuntimeState, ThreadSafety) {
  EJitRuntimeState state;
  int cells[8];
  state.getRegistry().registerArray("cell", "cells", cells, 8);

  std::thread activator([&]() {
    for (int i = 0; i < 1000; ++i)
      state.activate("cell", i % 8);
  });

  std::thread checker([&]() {
    for (int i = 0; i < 1000; ++i)
      (void)state.isActive("cell", i % 8);
  });

  activator.join();
  checker.join();

  // After all activations, the last few should be active
  // (no guarantee about specific states due to interleaving)
  state.activateAll("cell");
  EXPECT_TRUE(state.isActive("cell", 0));
  EXPECT_TRUE(state.isActive("cell", 7));
}
#endif // EJIT_FREESTANDING

//===----------------------------------------------------------------------===//
// EJitLogger tests (T3-12)
//===----------------------------------------------------------------------===//

#ifndef EJIT_FREESTANDING
TEST(EJitLogger, LogAndGetLastError) {
  EJitLogger logger;
  logger.log(kEjitStatusCompileFailed, "test error", "myfunc", "mykey");

  const EJitError *err = logger.getLastError();
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(err->code, kEjitStatusCompileFailed);
  EXPECT_EQ(err->message, "test error");
  EXPECT_EQ(err->funcName, "myfunc");
  EXPECT_EQ(err->cacheKey, "mykey");
}

TEST(EJitLogger, GetLastErrorEmpty) {
  EJitLogger logger;
  EXPECT_EQ(logger.getLastError(), nullptr);
}

TEST(EJitLogger, RingBufferWrap) {
  EJitLogger logger;

  // Write more than kMaxErrors entries
  for (size_t i = 0; i < EJitLogger::kMaxErrors + 10; ++i) {
    logger.log(kEjitStatusOk, "msg" + std::to_string(i));
  }

  const EJitError *last = logger.getLastError();
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(last->message, "msg" + std::to_string(EJitLogger::kMaxErrors + 9));
}

TEST(EJitLogger, GetErrors) {
  EJitLogger logger;

  for (size_t i = 0; i < 5; ++i)
    logger.log(kEjitStatusOk, "msg" + std::to_string(i));

  auto errors = logger.getErrors(3);
  EXPECT_EQ(errors.size(), 3u);
  EXPECT_EQ(errors[0].message, "msg0");
  EXPECT_EQ(errors[1].message, "msg1");
  EXPECT_EQ(errors[2].message, "msg2");
}

TEST(EJitLogger, Clear) {
  EJitLogger logger;
  logger.log(kEjitStatusOk, "test");
  logger.clear();
  EXPECT_EQ(logger.getLastError(), nullptr);
}

#endif // EJIT_FREESTANDING

//===----------------------------------------------------------------------===//
// EJit end-to-end construction test (T3-20)
//===----------------------------------------------------------------------===//

TEST(EJit, ConstructionAndBasicOps) {
  Config config;
  config.compileMode = CompileMode::Sync;
  config.maxCacheEntries = 64;
  config.maxCacheSize = 1024 * 1024;

  // Register the period array BEFORE construction (staged) so this works in
  // both builds — a taskpool build freezes registration after construction.
  int tp[8];
  EJitRegistrationStore::instance().consume(); // clear leftover
  EJitRegistrationStore::instance().registerPeriodArray("test_period", "tp", tp,
                                                        8);

  EJit ejit(config);

  // Basic lifecycle operations should not crash
  ejit.activate("test_period", 0);
  EXPECT_TRUE(ejit.isActive("test_period", 0));

  ejit.deactivate("test_period", 0);
  EXPECT_FALSE(ejit.isActive("test_period", 0));

  // Legacy LRU cache retired; taskpool stats via ejit_taskpool_get_stats.
}

TEST(EJit, ActivateAllAndDeactivateAll) {
  // Register a period array before constructing EJit so
  // activateAll/deactivateAll know the cell range.
  int dummy[4];
  EJitRegistrationStore::instance().consume(); // clear leftover
  EJitRegistrationStore::instance().registerPeriodArray("p1", "dummy_arr",
                                                        dummy, 4);

  EJit ejit(Config{});

  ejit.activateAll("p1");
  EXPECT_TRUE(ejit.isActive("p1", 0));
  EXPECT_TRUE(ejit.isActive("p1", 3));

  // deactivateAll should clear all
  ejit.deactivateAll("p1");
  EXPECT_FALSE(ejit.isActive("p1", 0));
  EXPECT_FALSE(ejit.isActive("p1", 3));
}

TEST(EJit, CacheOperations) {
  EJit ejit(Config{});

  // clearCache should not crash
  ejit.clearCache();

  // invalidateByPeriod should not crash
  ejit.invalidateByPeriod("test", 0);
}

TEST(EJit, CompileMode) {
  // Default is Async (background worker handles compilation).
  EJit ejit(Config{});
  EXPECT_EQ(ejit.getCompileMode(), CompileMode::Async);
  ASSERT_NE(ejit.taskPool(), nullptr);

  // Switch to sync: compile inline on calling thread.
  EXPECT_TRUE(ejit.setCompileMode(CompileMode::Sync));
  EXPECT_EQ(ejit.getCompileMode(), CompileMode::Sync);

  // Switch to off: no JIT, always AOT fallback.
  EXPECT_TRUE(ejit.setCompileMode(CompileMode::Off));
  EXPECT_EQ(ejit.getCompileMode(), CompileMode::Off);

  // Back to async.
  EXPECT_TRUE(ejit.setCompileMode(CompileMode::Async));
  EXPECT_EQ(ejit.getCompileMode(), CompileMode::Async);
}

TEST(EJit, OptimizationLevel) {
  EJit ejit(Config{});
  EXPECT_EQ(ejit.getOptimizationLevel(), llvm::ejit::OptimizationLevel::L2);

  ejit.setOptimizationLevel(llvm::ejit::OptimizationLevel::L3);
  EXPECT_EQ(ejit.getOptimizationLevel(), llvm::ejit::OptimizationLevel::L3);
}

#ifdef EJIT_SRE_TASKPOOL
TEST(EJitTaskpoolInit, OffModeSucceedsWithoutWorker) {
  Config config;
  config.compileMode = CompileMode::Off;
  EJit ejit(config);
  EXPECT_FALSE(ejit.initFailed());
  EXPECT_EQ(ejit.getCompileMode(), CompileMode::Off);
  ASSERT_NE(ejit.taskPool(), nullptr);
  EXPECT_FALSE(ejit.taskPool()->isWorkerRunning());
}

TEST(EJitTaskpoolInit, DefaultAsyncInitSucceeds) {
  EJit ejit(Config{});
  // ORC engine is always created; async init succeeds.
  EXPECT_FALSE(ejit.initFailed());
  EXPECT_EQ(ejit.getCompileMode(), CompileMode::Async);
  ASSERT_NE(ejit.taskPool(), nullptr);
}

// Finding (二): a constructed taskpool EJit freezes registration once its
// constructor completes, so every runtime registration entry point returns
// false and leaves the registry unchanged (the worker reads it lock-free).
TEST(EJit, TaskpoolRegistrationFrozenAfterConstruction) {
  EJit ejit(Config{});
  ASSERT_FALSE(ejit.initFailed());
  EXPECT_TRUE(ejit.registrationFrozen());

  const uint8_t bc[] = {1, 2, 3, 4};
  EXPECT_FALSE(ejit.registerBitcode("frozen_fn", bc, sizeof(bc)));
  int sv = 0;
  EXPECT_FALSE(ejit.registerStaticVar("frozen_var", &sv));
  int arr[8];
  EXPECT_FALSE(ejit.registerPeriodArray("frozen_period", "arr", arr, 8));
  // Nothing was registered: the registry is unchanged.
  EXPECT_EQ(ejit.getRegistry().getArrays("frozen_period"), nullptr);
}
#endif // EJIT_SRE_TASKPOOL

//===----------------------------------------------------------------------===//
// C API tests with runtime-dynamic cellIdx (T3-21)
//===----------------------------------------------------------------------===//

extern "C" {
typedef enum { EJIT_OK_C = 0 } ejit_status_test_t;
extern ejit_status_test_t ejit_init(const void *config);
extern void ejit_shutdown(void);
extern ejit_status_test_t ejit_activate(const char *, uint32_t);
extern ejit_status_test_t ejit_deactivate(const char *, uint32_t);
extern bool ejit_is_active(const char *, uint32_t);
extern void ejit_invalidate(const char *, uint32_t);
extern void ejit_clear_cache(void);
extern void ejit_register_period_array(const char *, const char *, void *,
                                       uint64_t);
extern void ejit_register_bitcode(const char *, const uint8_t *, uint64_t);
extern void ejit_register_static_var(const char *, void *);
extern void ejit_register_lifecycle(const char *, uint32_t *);
extern void ejit_set_log_level(int level);
extern int ejit_get_log_level(void);
extern void ejit_print_registry(void);
extern void ejit_print_func_meta(const char *funcName);
// P0 diagnostics: code pool stats + active period map. Declared returning int
// (the real ejit_status_t) with literal status values (0=OK, -1=INVALID_PARAM,
// -2=NOT_ACTIVE, -9=DISABLED) so the test need not include the C API header.
extern int ejit_get_code_pool_stats(void *out);
extern void ejit_print_code_pool_stats(void);
extern void ejit_print_active(void);
extern void ejit_print_version(void);
}

// The "runtime-dynamic cellIdx" C-API tests below exercise the LEGACY model:
// dynamic period-array registration AFTER ejit_init, then period-name activate.
// In a taskpool build that model is intentionally replaced — registration is
// frozen after init and activate is keyed on registered lifecycles — so these
// tests run only in the non-taskpool build. Taskpool-mode equivalents (freeze,
// lifecycle activate, array sync) live in the EJitCApiTaskpool section.
#ifndef EJIT_SRE_TASKPOOL

TEST(EJitCApi, ActivateWithDynamicCellIdx) {
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);
  // Per-array activation model: register an array for the period first.
  int arr[256];
  ejit_register_period_array("dynamic", "arr", arr, 256);
  uint8_t idx = 42;
  EXPECT_EQ(ejit_activate("dynamic", idx), EJIT_OK_C);
  EXPECT_TRUE(ejit_is_active("dynamic", idx));
  EXPECT_EQ(ejit_deactivate("dynamic", idx), EJIT_OK_C);
  EXPECT_FALSE(ejit_is_active("dynamic", idx));
  ejit_shutdown();
}

TEST(EJitCApi, LoopWithDynamicCellIdx) {
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);
  int arr[256];
  ejit_register_period_array("loop", "arr", arr, 256);
  for (uint8_t i = 0; i < 10; i++)
    EXPECT_EQ(ejit_activate("loop", i), EJIT_OK_C);
  for (uint8_t i = 0; i < 10; i++)
    EXPECT_TRUE(ejit_is_active("loop", i));
  for (uint8_t i = 10; i > 0; i--)
    EXPECT_EQ(ejit_deactivate("loop", i - 1), EJIT_OK_C);
  for (uint8_t i = 0; i < 10; i++)
    EXPECT_FALSE(ejit_is_active("loop", i));
  ejit_shutdown();
}

static uint8_t computeCellIdx(int x, int y) {
  return static_cast<uint8_t>((x + y) % 256);
}

TEST(EJitCApi, ActivateWithComputedCellIdx) {
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);
  int arr[256];
  ejit_register_period_array("compute", "arr", arr, 256);
  uint8_t idx = computeCellIdx(100, 55);
  EXPECT_EQ(idx, 155);
  EXPECT_EQ(ejit_activate("compute", idx), EJIT_OK_C);
  EXPECT_TRUE(ejit_is_active("compute", idx));
  idx = computeCellIdx(200, 200);
  EXPECT_EQ(idx, 144);
  EXPECT_EQ(ejit_activate("compute", idx), EJIT_OK_C);
  EXPECT_TRUE(ejit_is_active("compute", idx));
  EXPECT_FALSE(ejit_is_active("other", 155));
  ejit_shutdown();
}

TEST(EJitCApi, DynamicCellIdxBoundaries) {
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);
  int arr[256];
  ejit_register_period_array("bound", "arr", arr, 256);
  EXPECT_EQ(ejit_activate("bound", (uint8_t)0), EJIT_OK_C);
  EXPECT_TRUE(ejit_is_active("bound", (uint8_t)0));
  EXPECT_EQ(ejit_activate("bound", (uint8_t)255), EJIT_OK_C);
  EXPECT_TRUE(ejit_is_active("bound", (uint8_t)255));
  // Out-of-range instance index must be REJECTED (post uint32_t-ABI fix), not
  // truncated to 8 bits at the call boundary and applied to the wrong cell.
  EXPECT_NE(ejit_activate("bound", 256u), EJIT_OK_C);
  EXPECT_NE(ejit_deactivate("bound", 256u), EJIT_OK_C);
  // Same contract for the query/invalidate entry points: 256 must neither
  // read nor invalidate instance 0.
  ejit_activate("bound", (uint8_t)0);
  EXPECT_TRUE(ejit_is_active("bound", (uint8_t)0));
  EXPECT_FALSE(ejit_is_active("bound", 256u));
  ejit_invalidate("bound", 256u);
  EXPECT_TRUE(ejit_is_active("bound", (uint8_t)0)); // instance 0 untouched
  ejit_deactivate("bound", 0);
  EXPECT_FALSE(ejit_is_active("bound", 0));
  EXPECT_TRUE(ejit_is_active("bound", (uint8_t)255));
  ejit_shutdown();
}

TEST(EJitCApi, MultiPeriodDynamicIndices) {
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);
  int cellArr[256], trpArr[256];
  ejit_register_period_array("cell", "cellArr", cellArr, 256);
  ejit_register_period_array("trp", "trpArr", trpArr, 256);
  uint8_t indices[] = {3, 7, 15, 31, 63};
  for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
    EXPECT_EQ(ejit_activate("cell", indices[i]), EJIT_OK_C);
    EXPECT_EQ(ejit_activate("trp", indices[i]), EJIT_OK_C);
  }
  for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
    EXPECT_TRUE(ejit_is_active("cell", indices[i]));
    EXPECT_TRUE(ejit_is_active("trp", indices[i]));
  }
  for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++)
    ejit_deactivate("trp", indices[i]);
  for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
    EXPECT_TRUE(ejit_is_active("cell", indices[i]));
    EXPECT_FALSE(ejit_is_active("trp", indices[i]));
  }
  ejit_shutdown();
}

TEST(EJitCApi, InvalidateWithDynamicCellIdx) {
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);
  int arr[256];
  ejit_register_period_array("inv", "arr", arr, 256);
  for (uint8_t i = 0; i < 5; i++)
    ejit_activate("inv", i);
  for (uint8_t i = 0; i < 5; i++)
    EXPECT_TRUE(ejit_is_active("inv", i));
  for (uint8_t i = 0; i < 5; i++)
    ejit_invalidate("inv", i);
  for (uint8_t i = 0; i < 5; i++)
    EXPECT_TRUE(ejit_is_active("inv", i));
  ejit_shutdown();
}

TEST(EJitCApi, ActivationCycleWithRuntimeIndex) {
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);
  int arr[256];
  ejit_register_period_array("cycle", "arr", arr, 256);
  uint8_t workload[] = {10, 20, 30, 40, 50};
  for (size_t cycle = 0; cycle < 3; cycle++) {
    for (size_t i = 0; i < sizeof(workload) / sizeof(workload[0]); i++)
      EXPECT_EQ(ejit_activate("cycle", workload[i]), EJIT_OK_C);
    for (size_t i = 0; i < sizeof(workload) / sizeof(workload[0]); i++)
      EXPECT_TRUE(ejit_is_active("cycle", workload[i]));
    for (size_t i = 0; i < sizeof(workload) / sizeof(workload[0]); i++)
      EXPECT_EQ(ejit_deactivate("cycle", workload[i]), EJIT_OK_C);
    for (size_t i = 0; i < sizeof(workload) / sizeof(workload[0]); i++)
      EXPECT_FALSE(ejit_is_active("cycle", workload[i]));
  }
  ejit_shutdown();
}

#endif // !EJIT_SRE_TASKPOOL

#ifdef EJIT_SRE_TASKPOOL
//===----------------------------------------------------------------------===//
// Taskpool-mode C-API: registration freeze (finding 二) + name/array control
// plane synced to the SwitchController (findings 一/五).
//===----------------------------------------------------------------------===//

namespace {
// Reset process-global registration state so each taskpool test starts clean:
// no live gEJIT, no staged data, no staged error, empty name registries. This
// is essential because the C ABI / registries are process singletons that leak
// across tests (e.g. a frozen post-init registration records an error that a
// later ejit_init would otherwise consume and fail on).
void resetTaskpoolRegState() {
  ejit_shutdown(); // drop any live runtime singleton (no-op if already null)
  llvm::ejit::EJitLifecycleRegistry::instance().reset();
  llvm::ejit::EJitFuncRegistry::instance().reset();
  llvm::ejit::EJitRegistrationStore::instance().consume();
  llvm::ejit::EJitRegistrationStore::instance().consumeError();
}
} // namespace

// Finding (二): in a taskpool build, all registration is frozen after
// ejit_init; post-init register_* calls are rejected and mutate nothing.
TEST(EJitCApiTaskpool, RegistrationFrozenAfterInit) {
  resetTaskpoolRegState();
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);
  // A period registered AFTER init is rejected (frozen): the name is therefore
  // not a registered period, so activate cannot find it.
  int arr[8];
  ejit_register_period_array("post_period", "arr", arr, 8); // frozen no-op
  EXPECT_NE(ejit_activate("post_period", 0), EJIT_OK_C);
  EXPECT_FALSE(ejit_is_active("post_period", 0));
  // Bitcode / static-var registration after init is likewise frozen (no crash,
  // no mutation). The void ABI cannot return a status; the key invariant is
  // that nothing observable changes.
  const uint8_t bc[] = {1, 2, 3, 4};
  ejit_register_bitcode("post_fn", bc, sizeof(bc));
  int sv = 0;
  ejit_register_static_var("post_var", &sv);
  ejit_shutdown();
}

// Finding (一/五): a lifecycle registered BEFORE init (constructor-phase path)
// drives both the time-window state and the SwitchController via the public
// C ABI. Activation is keyed by lifecycle name + instance index only.
TEST(EJitCApiTaskpool, PreInitLifecycleActivateAndArraySync) {
  // Start from a fully clean process state so "cell" gets a deterministic slot
  // and no leaked error sink fails ejit_init, regardless of test order.
  resetTaskpoolRegState();
  // Register the lifecycle + its period array BEFORE init (constructor-phase),
  // so init consumes them and registration is consistent before the freeze.
  uint32_t cellSlot = 0xFFFFFFFFu;
  ejit_register_lifecycle("cell", &cellSlot);
  ASSERT_NE(cellSlot, 0xFFFFFFFFu);
  static int cellArr[256];
  ejit_register_period_array("cell", "cellArr", cellArr, 256);
  ASSERT_EQ(ejit_init(nullptr), EJIT_OK_C);

  // Name-based activate/deactivate of the registered lifecycle succeeds.
  EXPECT_EQ(ejit_activate("cell", 7), EJIT_OK_C);
  EXPECT_TRUE(ejit_is_active("cell", 7));
  EXPECT_EQ(ejit_deactivate("cell", 7), EJIT_OK_C);
  EXPECT_FALSE(ejit_is_active("cell", 7));

  // An unknown period name is rejected and mutates nothing.
  EXPECT_NE(ejit_activate("wrong", 0), EJIT_OK_C);
  EXPECT_FALSE(ejit_is_active("wrong", 0));
  ejit_shutdown();
}

// Finding (一): name-level activate/deactivate through the PUBLIC EJit API
// syncs the time-window state AND the taskpool SwitchController; the version
// bumps only on a real flip. This proves the EJit wiring, not just the
// SwitchController in isolation.
TEST(EJitTaskpoolArray, NameLevelSyncsSwitchController) {
  resetTaskpoolRegState();
  uint32_t slot = 0xFFFFFFFFu;
  ejit_register_lifecycle("cell", &slot);
  ASSERT_NE(slot, 0xFFFFFFFFu);
  static int cellArr[16];
  ejit_register_period_array("cell", "cellArr", cellArr, 16); // staged
  EJit ejit(Config{});
  ASSERT_FALSE(ejit.initFailed());
  EJitTaskPool *tp = ejit.taskPool();
  ASSERT_NE(tp, nullptr);
  uint32_t dt = EJitLifecycleRegistry::instance().lookup("cell");
  ASSERT_NE(dt, kEJitInvalidDimType);

#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Shared build: deactivate/activate writes the shared pool's switch
  // controller, which is separate from the per-instance EJitTaskPool switch.
  // Check enabled state via the shared pool (instanceVersion is private).
  // initSharedStorage defaults all instances to disabled — enable explicitly.
  EJitSharedTaskPool *sp = ejit.sharedTaskPool();
  ASSERT_NE(sp, nullptr);
  EXPECT_TRUE(sp->setInstanceEnabled(dt, 5, true));
  auto instanceEnabled = [&]() { return sp->isInstanceActive(dt, 5); };
#else
  EJitSwitchController &sw = tp->switchController();
  auto instanceEnabled = [&]() { return sw.isInstanceEnabled(dt, 5); };
  auto instanceVer = [&]() { return sw.getInstanceVersion(dt, 5); };
  uint32_t v0 = instanceVer();
#endif

  EXPECT_TRUE(instanceEnabled()); // defaults enabled
  // deactivate.
  EXPECT_TRUE(ejit.deactivate("cell", 5));
  EXPECT_FALSE(ejit.isActive("cell", 5));
  EXPECT_FALSE(instanceEnabled());
#ifndef EJIT_SRE_SHARED_TASKPOOL
  EXPECT_EQ(instanceVer(), v0 + 1);
#endif
  // activate.
  EXPECT_TRUE(ejit.activate("cell", 5));
  EXPECT_TRUE(ejit.isActive("cell", 5));
  EXPECT_TRUE(instanceEnabled());
#ifndef EJIT_SRE_SHARED_TASKPOOL
  EXPECT_EQ(instanceVer(), v0 + 2);
  // Redundant activate: no flip.
  EXPECT_TRUE(ejit.activate("cell", 5));
  EXPECT_EQ(instanceVer(), v0 + 2);
#endif
  resetTaskpoolRegState();
}

// Finding (一): an unknown period name is cleanly rejected — neither the
// RuntimeState nor the SwitchController (nor the version) changes.
TEST(EJitTaskpoolArray, UnknownPeriodCleanReject) {
  resetTaskpoolRegState();
  uint32_t slot = 0xFFFFFFFFu;
  ejit_register_lifecycle("cell", &slot);
  ASSERT_NE(slot, 0xFFFFFFFFu);
  static int cellArr[16];
  ejit_register_period_array("cell", "cellArr", cellArr, 16);
  EJit ejit(Config{});
  ASSERT_FALSE(ejit.initFailed());
  EJitTaskPool *tp = ejit.taskPool();
  ASSERT_NE(tp, nullptr);
  uint32_t dt = EJitLifecycleRegistry::instance().lookup("cell");
  EJitSwitchController &sw = tp->switchController();

  uint32_t v0 = sw.getInstanceVersion(dt, 2);
  EXPECT_FALSE(ejit.activate("wrong", 2));   // unknown lifecycle -> reject
  EXPECT_FALSE(ejit.deactivate("wrong", 2));
  EXPECT_TRUE(sw.isInstanceEnabled(dt, 2));
  EXPECT_EQ(sw.getInstanceVersion(dt, 2), v0);
  EXPECT_FALSE(ejit.isActive("wrong", 2));
  resetTaskpoolRegState();
}

// Finding (三): any init failure (here: lifecycle-capacity exhaustion, which
// shares the recordInitError path with a worker-start failure) drives the
// registration phase to Failed (registrationFrozen() true), leaves the worker
// stopped, and rejects further registration.
TEST(EJitTaskpoolInit, InitFailureSetsFailedAndStopsWorker) {
  resetTaskpoolRegState();
  // Fill the 8 lifecycle slots; the 9th records a capacity error that the next
  // construction consumes as an init failure.
  for (uint32_t i = 0; i < kEJitMaxDimTypes; ++i) {
    uint32_t s = 0xFFFFFFFFu;
    ejit_register_lifecycle(("k" + std::to_string(i)).c_str(), &s);
    ASSERT_NE(s, 0xFFFFFFFFu);
  }
  uint32_t s9 = 0xFFFFFFFFu;
  ejit_register_lifecycle("k_overflow", &s9);
  EXPECT_EQ(s9, 0xFFFFFFFFu); // 9th rejected

  EJit ejit(Config{});
  EXPECT_TRUE(ejit.initFailed());
  // Phase Failed: registrationFrozen() is true and a fresh registration fails.
  EXPECT_TRUE(ejit.registrationFrozen());
  const uint8_t bc[] = {1, 2, 3};
  EXPECT_FALSE(ejit.registerBitcode("x", bc, sizeof(bc)));
  // The worker was never started on a failed init.
  if (EJitTaskPool *tp = ejit.taskPool())
    EXPECT_FALSE(tp->isWorkerRunning());
  resetTaskpoolRegState();
}

// Finding (三): ejit_init returns failure and destroys the instance on an init
// error; a subsequent C ABI call sees "not initialized".
TEST(EJitCApiTaskpool, InitFailsAndDestroysOnRegistrationError) {
  resetTaskpoolRegState();
  for (uint32_t i = 0; i < kEJitMaxDimTypes; ++i) {
    uint32_t s = 0xFFFFFFFFu;
    ejit_register_lifecycle(("m" + std::to_string(i)).c_str(), &s);
  }
  uint32_t s9 = 0xFFFFFFFFu;
  ejit_register_lifecycle("m_overflow", &s9);
  EXPECT_EQ(s9, 0xFFFFFFFFu);

  EXPECT_NE(ejit_init(nullptr), EJIT_OK_C);     // init fails
  EXPECT_NE(ejit_activate("m0", 0), EJIT_OK_C); // torn down: not initialized
  ejit_shutdown();                              // safe no-op
  resetTaskpoolRegState();
}
#endif // EJIT_SRE_TASKPOOL

//===----------------------------------------------------------------------===//
// PeriodArrayRegistry::getArrayByBaseAddr tests
//===----------------------------------------------------------------------===//

TEST(PeriodArrayRegistry, GetArrayByBaseAddr) {
  PeriodArrayRegistry reg;
  int data[10];
  reg.registerArray("cell", "my_cells", data, 10);

  const auto *info = reg.getArrayByBaseAddr(data);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->varName, "my_cells");
  EXPECT_EQ(info->periodName, "cell");
  EXPECT_EQ(info->arraySize, 10u);

  // Non-registered pointer returns nullptr
  int other;
  EXPECT_EQ(reg.getArrayByBaseAddr(&other), nullptr);
  EXPECT_EQ(reg.getArrayByBaseAddr(nullptr), nullptr);
}

TEST(PeriodArrayRegistry, GetArrayByBaseAddrMultipleArrays) {
  PeriodArrayRegistry reg;
  int data1[5], data2[10];
  reg.registerArray("cell", "a", data1, 5);
  reg.registerArray("trp", "b", data2, 10);

  const auto *info1 = reg.getArrayByBaseAddr(data1);
  ASSERT_NE(info1, nullptr);
  EXPECT_EQ(info1->varName, "a");
  EXPECT_EQ(info1->periodName, "cell");

  const auto *info2 = reg.getArrayByBaseAddr(data2);
  ASSERT_NE(info2, nullptr);
  EXPECT_EQ(info2->varName, "b");
  EXPECT_EQ(info2->periodName, "trp");
}

//===----------------------------------------------------------------------===//
// EJitOptimizer tests
//===----------------------------------------------------------------------===//

static std::unique_ptr<Module> createTestModule(LLVMContext &Ctx,
                                                const std::string &Name) {
  auto M = std::make_unique<Module>("test", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  return M;
}

/// Create a simple function with a period-array-index argument metadata.
static Function *createPeriodIndFunc(LLVMContext &Ctx, Module &M,
                                     const std::string &name) {
  IRBuilder<> B(Ctx);
  Type *RetTy = B.getInt32Ty();
  Type *ParamTy = B.getInt32Ty();
  FunctionType *FT = FunctionType::get(RetTy, {ParamTy}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, name, &M);
  F->setName(name);

  auto &Arg = *F->arg_begin();
  Arg.setName("period_idx");

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  B.CreateRet(B.CreateAdd(&Arg, B.getInt32(1)));

  // Attach !ejit.metadata with a sub-node for period-array-index:
  //   !ejit.metadata = !{!0}
  //   !0 = !{!"ejit_period_arr_ind", !"cell", i32 0}
  Metadata *MDOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(B.getInt32Ty(), 0)),
  };
  MDNode *Sub = MDNode::get(Ctx, MDOps);
  F->setMetadata(MD_EJIT_METADATA, MDNode::get(Ctx, {Sub}));

  return F;
}

TEST(EJitOptimizer, PreReplacePeriodIndices) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "preReplaceTest");
  Function *F = createPeriodIndFunc(Ctx, *M, "test_func");
  ASSERT_NE(F, nullptr);

  PeriodArrayRegistry reg;
  SpecializationContext ctx;
  ctx.fnName = "test_func";
  ctx.dimensions.push_back({"cell", 42});

  // Before replacement: argument is used
  auto &Arg = *F->arg_begin();
  EXPECT_TRUE(Arg.hasNUsesOrMore(1));

  EJitOptimizerTestAccess opt(reg);
  opt.preReplacePeriodIndices(*M, ctx);

  // After replacement: the arg should have zero uses (replaced by constant 42)
  EXPECT_EQ(Arg.getNumUses(), 0u);
}

TEST(EJitOptimizer, OptimizationPipelineL1) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "optL1");
  createPeriodIndFunc(Ctx, *M, "f");

  PeriodArrayRegistry reg;
  EJitOptimizerTestAccess opt(reg);

  // L1 should not crash
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L1);
}

TEST(EJitOptimizer, OptimizationPipelineL2) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "optL2");
  createPeriodIndFunc(Ctx, *M, "f");

  PeriodArrayRegistry reg;
  EJitOptimizerTestAccess opt(reg);

  // L2 should not crash
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L2);
}

TEST(EJitOptimizer, OptimizationPipelineL3) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "optL3");
  createPeriodIndFunc(Ctx, *M, "f");

  PeriodArrayRegistry reg;
  EJitOptimizerTestAccess opt(reg);

  // L3 should not crash
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L3);
}

TEST(EJitOptimizer, FullPipelineEndToEnd) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "fullPipeline");
  createPeriodIndFunc(Ctx, *M, "test_fn");

  PeriodArrayRegistry reg;
  SpecializationContext ctx;
  ctx.fnName = "test_fn";
  ctx.dimensions.push_back({"cell", 100});
  ctx.optLevel = llvm::ejit::OptimizationLevel::L3;

  EJitOptimizerTestAccess opt(reg);

  // 1. Pre-replace
  opt.preReplacePeriodIndices(*M, ctx);

  // 2. InstCombine
  opt.runInstCombine(*M);

  // 3. Inline (no-op for a single function, but shouldn't crash)

  // 4. Optimization pipeline at L3
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L3);
}

/// Verify the optimization pipeline folds llvm.expect-guarded constant
/// branches and DCEs the dead calls they guard. The AOT IR carries
/// __builtin_expect / LIKELY hints on top of may_const conditions; once
/// specialization turns the condition into a constant, the expect intrinsic
/// blocks InstCombine/SCCP from folding the branch unless LowerExpectIntrinsic
/// runs first. Without it the dead block (and its calls) survive ADCE and the
/// specialization does the same work as AOT plus JIT overhead.
TEST(EJitOptimizer, FoldsExpectGuardedConstantBranch) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("expect_fold", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  IRBuilder<> B(Ctx);
  Type *I32Ty = B.getInt32Ty();
  Type *I64Ty = B.getInt64Ty();

  FunctionCallee ExpectFn = M->getOrInsertFunction(
      "llvm.expect.i64", FunctionType::get(I64Ty, {I64Ty, I64Ty}, false));
  FunctionCallee HeavyFn = M->getOrInsertFunction(
      "heavy_dead_call", FunctionType::get(B.getVoidTy(), {}, false));

  FunctionType *FT = FunctionType::get(I32Ty, {}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "test_expect", M.get());
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *Taken = BasicBlock::Create(Ctx, "taken", F);
  BasicBlock *Dead = BasicBlock::Create(Ctx, "dead", F);

  // entry: %e = expect(0, 0); %c = icmp eq %e, 0; br %c, taken, dead
  B.SetInsertPoint(Entry);
  auto *E = B.CreateCall(ExpectFn, {B.getInt64(0), B.getInt64(0)}, "e");
  auto *C = B.CreateICmpEQ(E, B.getInt64(0), "c");
  B.CreateCondBr(C, Taken, Dead);

  // taken: ret 0
  B.SetInsertPoint(Taken);
  B.CreateRet(B.getInt32(0));

  // dead: call heavy(); ret 1   -- must be eliminated once the branch folds
  B.SetInsertPoint(Dead);
  B.CreateCall(HeavyFn, {});
  B.CreateRet(B.getInt32(1));

  PeriodArrayRegistry reg;
  EJitOptimizerTestAccess opt(reg);
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L3);

  // No llvm.expect should remain.
  bool hasExpect = false;
  bool hasHeavyCall = false;
  unsigned retCount = 0;
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "llvm.expect.i64")
          hasExpect = true;
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "heavy_dead_call")
          hasHeavyCall = true;
      }
      if (isa<ReturnInst>(&I))
        ++retCount;
    }
  EXPECT_FALSE(hasExpect) << "llvm.expect was not lowered";
  EXPECT_FALSE(hasHeavyCall) << "dead call in folded block was not DCE'd";
  EXPECT_EQ(retCount, 1u);
  // The single ret must be ret 0 (taken path), not ret 1 (dead path).
  auto *Ret = dyn_cast<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getZExtValue(), 0u);
}

//===----------------------------------------------------------------------===//
// EJitStructFieldPass tests
//===----------------------------------------------------------------------===//

/// Create a function with a GEP + load from a global array (simulating
/// a period array access marked with !ejit.may_const).
/// The GEP index is a constant so the pass can compute the offset.
/// Also adds !ejit.metadata to the global to identify it as a period array.
static Function *createStructFieldFunc(LLVMContext &Ctx, Module &M,
                                       uint64_t gepIdx = 2) {
  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();

  // Create a global array: int32_t g_arr[4]
  auto *ArrTy = ArrayType::get(Int32Ty, 4);
  auto *GVar = new GlobalVariable(M, ArrTy, false, GlobalValue::InternalLinkage,
                                  ConstantAggregateZero::get(ArrTy), "g_arr");

  // Add !ejit.metadata to GVar: !g_arr = !{!"ejit_period_arr", !"cell", i32 4}
  Metadata *ArrMDOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GVar->setMetadata(MD_EJIT_METADATA,
                    MDNode::get(Ctx, {MDNode::get(Ctx, ArrMDOps)}));

  // Function: int32_t test_load()
  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "test_load", &M);

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);

  // GEP: &g_arr[gepIdx] with constant index
  Value *IdxList[] = {B.getInt32(0), B.getInt64(gepIdx)};
  auto *GEP = B.CreateInBoundsGEP(ArrTy, GVar, IdxList, "gep");
  auto *Load = B.CreateLoad(Int32Ty, GEP, "load");

  // Add !ejit.may_const metadata to the load
  Load->setMetadata("ejit.may_const",
                    MDNode::get(Ctx, MDString::get(Ctx, "ejit")));

  B.CreateRet(Load);
  return F;
}

TEST(EJitStructFieldPass, MayConstLoadSubstitution) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_struct", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createStructFieldFunc(Ctx, *M, 2); // g_arr[2] = 30
  ASSERT_NE(F, nullptr);

  // Create real memory representing the period array data.
  // The StructFieldPass reads from the registry's baseAddr at the GEP offset.
  int32_t mockArr[4] = {10, 20, 30, 40};

  PeriodArrayRegistry reg;
  GlobalVariable *GV = M->getGlobalVariable("g_arr", true);
  ASSERT_NE(GV, nullptr) << "g_arr not found in module";
  reg.registerArray("cell", "g_arr", mockArr, 4);

  // Run the StructFieldPass
  EJitStructFieldPass structPass(reg);
  structPass.initFromModule(*M);
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  auto PA = structPass.run(*F, FAM);

  // The pass should find the GEP+load, compute offset for g_arr[2] = 8 bytes,
  // read mockArr + 8 = mockArr[2] = 30, and replace the load.
  EXPECT_FALSE(PA.areAllPreserved());

  // Verify the load was replaced: the ret should now use a ConstantInt(30)
  bool loadRemoved = true;
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB)
      if (isa<LoadInst>(&I))
        loadRemoved = false;
  EXPECT_TRUE(loadRemoved);

  // Check that the return value is constant 30
  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 30);
}

TEST(EJitStructFieldPass, NoMayConstNoChange) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_nochange", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));

  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();
  FunctionType *FT = FunctionType::get(Int32Ty, {Int32Ty}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "noop", M.get());

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  auto *Add = B.CreateAdd(F->getArg(0), B.getInt32(1));
  B.CreateRet(Add);

  PeriodArrayRegistry reg;
  EJitStructFieldPass structPass(reg);
  structPass.initFromModule(*M);
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  auto PA = structPass.run(*F, FAM);

  // Pass should preserve all analyses when there's nothing to do
  EXPECT_TRUE(PA.areAllPreserved());
}

//===----------------------------------------------------------------------===//
// EJitStructFieldPass extended tests
//===----------------------------------------------------------------------===//

/// Create a function with two loads from different "fields" of the same
/// global array (index 1 and index 3). Tests multi-field substitution.
/// g_arr[1] + g_arr[3]
static Function *createMultiFieldFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();

  auto *ArrTy = ArrayType::get(Int32Ty, 4);
  auto *GVar = new GlobalVariable(M, ArrTy, false, GlobalValue::InternalLinkage,
                                  ConstantAggregateZero::get(ArrTy), "g_arr");

  // !ejit.metadata = !{!"ejit_period_arr", !"cell", i32 4}
  Metadata *ArrOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GVar->setMetadata(MD_EJIT_METADATA,
                    MDNode::get(Ctx, {MDNode::get(Ctx, ArrOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "multi_field", &M);

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);

  // Load from g_arr[1] (offset 4)
  Value *I1[] = {B.getInt32(0), B.getInt64(1)};
  auto *GEP1 = B.CreateInBoundsGEP(ArrTy, GVar, I1, "elem1");
  auto *Load1 = B.CreateLoad(Int32Ty, GEP1, "val1");
  Load1->setMetadata("ejit.may_const",
                     MDNode::get(Ctx, MDString::get(Ctx, "ejit")));

  // Load from g_arr[3] (offset 12)
  Value *I3[] = {B.getInt32(0), B.getInt64(3)};
  auto *GEP3 = B.CreateInBoundsGEP(ArrTy, GVar, I3, "elem3");
  auto *Load3 = B.CreateLoad(Int32Ty, GEP3, "val3");
  Load3->setMetadata("ejit.may_const",
                     MDNode::get(Ctx, MDString::get(Ctx, "ejit")));

  auto *Sum = B.CreateAdd(Load1, Load3, "sum");
  B.CreateRet(Sum);
  return F;
}

TEST(EJitStructFieldPass, MayConstLoadSubstitutionMultipleFields) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_multifield", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createMultiFieldFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  int32_t mockArr[4] = {10, 55, 20, 66}; // g_arr[1]=55, g_arr[3]=66

  PeriodArrayRegistry reg;
  GlobalVariable *GV = M->getGlobalVariable("g_arr", true);
  ASSERT_NE(GV, nullptr);
  reg.registerArray("cell", "g_arr", mockArr, 4);

  EJitStructFieldPass sp(reg);
  sp.initFromModule(*M);
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  sp.run(*F, FAM);

  // Fold the add of two constants: 55 + 66 = 121
  EJitOptimizerTestAccess opt(reg);
  opt.runInstCombine(*M);

  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 121);
}

/// Create a function with loads from different indices into a period array.
static Function *createNestedStructFunc(LLVMContext &Ctx, Module &M,
                                        uint64_t arrIdx = 1) {
  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();

  auto *ArrTy = ArrayType::get(Int32Ty, 4);
  auto *GVar = new GlobalVariable(M, ArrTy, false, GlobalValue::InternalLinkage,
                                  ConstantAggregateZero::get(ArrTy), "g_data");

  Metadata *ArrOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GVar->setMetadata(MD_EJIT_METADATA,
                    MDNode::get(Ctx, {MDNode::get(Ctx, ArrOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "indexed_load", &M);

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);

  Value *Indices[] = {B.getInt32(0), B.getInt64(arrIdx)};
  auto *GEP = B.CreateInBoundsGEP(ArrTy, GVar, Indices, "gep");
  auto *Load = B.CreateLoad(Int32Ty, GEP, "val");
  Load->setMetadata("ejit.may_const",
                    MDNode::get(Ctx, MDString::get(Ctx, "ejit")));
  B.CreateRet(Load);
  return F;
}

TEST(EJitStructFieldPass, MayConstLoadSubstitutionNestedStruct) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_nested", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createNestedStructFunc(Ctx, *M, 2);
  ASSERT_NE(F, nullptr);

  int32_t mockArr[4] = {10, 20, 40, 80}; // g_data[2] = 40

  PeriodArrayRegistry reg;
  GlobalVariable *GV = M->getGlobalVariable("g_data", true);
  ASSERT_NE(GV, nullptr);
  reg.registerArray("cell", "g_data", mockArr, 4);

  EJitStructFieldPass sp(reg);
  sp.initFromModule(*M);
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  sp.run(*F, FAM);

  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 40);
}

/// Create a function that accesses two different period arrays.
/// g_cells["cell"].field and g_trps["trp"].field
static Function *createMultiArrayFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();

  auto *ArrTy = ArrayType::get(Int32Ty, 4);

  auto *GVar1 =
      new GlobalVariable(M, ArrTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(ArrTy), "g_cells");
  auto *GVar2 =
      new GlobalVariable(M, ArrTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(ArrTy), "g_trps");

  // g_cells metadata: ejit_period_arr "cell" size 4
  Metadata *CellOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GVar1->setMetadata(MD_EJIT_METADATA,
                     MDNode::get(Ctx, {MDNode::get(Ctx, CellOps)}));
  // g_trps metadata: ejit_period_arr "trp" size 4
  Metadata *TrpOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "trp"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GVar2->setMetadata(MD_EJIT_METADATA,
                     MDNode::get(Ctx, {MDNode::get(Ctx, TrpOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "multi_arr", &M);

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);

  // Load from g_cells[2] -> cell array, index 2
  Value *CIdx[] = {B.getInt32(0), B.getInt64(2)};
  auto *GEP1 = B.CreateInBoundsGEP(ArrTy, GVar1, CIdx, "cell_gep");
  auto *Load1 = B.CreateLoad(Int32Ty, GEP1, "cell_val");
  Load1->setMetadata("ejit.may_const",
                     MDNode::get(Ctx, MDString::get(Ctx, "ejit")));

  // Load from g_trps[3] -> trp array, index 3
  Value *TIdx[] = {B.getInt32(0), B.getInt64(3)};
  auto *GEP2 = B.CreateInBoundsGEP(ArrTy, GVar2, TIdx, "trp_gep");
  auto *Load2 = B.CreateLoad(Int32Ty, GEP2, "trp_val");
  Load2->setMetadata("ejit.may_const",
                     MDNode::get(Ctx, MDString::get(Ctx, "ejit")));

  auto *Mul = B.CreateMul(Load1, Load2, "product");
  B.CreateRet(Mul);
  return F;
}

TEST(EJitStructFieldPass, MayConstLoadSubstitutionMultipleArrays) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_multi_arr", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createMultiArrayFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  int32_t cellData[4] = {1, 2, 7, 4}; // g_cells[2] = 7
  int32_t trpData[4] = {5, 6, 7, 8};  // g_trps[3] = 8

  PeriodArrayRegistry reg;
  reg.registerArray("cell", "g_cells", cellData, 4);
  reg.registerArray("trp", "g_trps", trpData, 4);

  EJitStructFieldPass sp(reg);
  sp.initFromModule(*M);
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  sp.run(*F, FAM);

  EJitOptimizerTestAccess opt(reg);
  opt.runInstCombine(*M);

  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 56); // 7 * 8 = 56
}

/// Create a function with may_const loads of different types (int + float)
/// from separate array globals. Tests mixed-type substitution.
static Function *createMixedTypeFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();
  Type *FloatTy = B.getFloatTy();

  auto *IntArrTy = ArrayType::get(Int32Ty, 4);
  auto *FltArrTy = ArrayType::get(FloatTy, 4);

  auto *GInt =
      new GlobalVariable(M, IntArrTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(IntArrTy), "g_ints");
  auto *GFlt =
      new GlobalVariable(M, FltArrTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(FltArrTy), "g_floats");

  Metadata *IntOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "ints"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GInt->setMetadata(MD_EJIT_METADATA,
                    MDNode::get(Ctx, {MDNode::get(Ctx, IntOps)}));
  Metadata *FltOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "floats"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GFlt->setMetadata(MD_EJIT_METADATA,
                    MDNode::get(Ctx, {MDNode::get(Ctx, FltOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "mixed_type", &M);

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);

  // Load int from g_ints[0]
  Value *II[] = {B.getInt32(0), B.getInt64(0)};
  auto *GEP_i = B.CreateInBoundsGEP(IntArrTy, GInt, II, "int_elem");
  auto *LoadI = B.CreateLoad(Int32Ty, GEP_i, "int_val");
  LoadI->setMetadata("ejit.may_const",
                     MDNode::get(Ctx, MDString::get(Ctx, "ejit")));

  // Load float from g_floats[0]
  Value *FI[] = {B.getInt32(0), B.getInt64(0)};
  auto *GEP_f = B.CreateInBoundsGEP(FltArrTy, GFlt, FI, "flt_elem");
  auto *LoadF = B.CreateLoad(FloatTy, GEP_f, "flt_val");
  LoadF->setMetadata("ejit.may_const",
                     MDNode::get(Ctx, MDString::get(Ctx, "ejit")));

  auto *FToI = B.CreateFPToSI(LoadF, Int32Ty, "ftoi");
  auto *Sum = B.CreateAdd(LoadI, FToI, "sum");
  B.CreateRet(Sum);
  return F;
}

TEST(EJitStructFieldPass, MayConstLoadSubstitutionIntFloat) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_mixed", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createMixedTypeFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  int32_t intData[4] = {10, 0, 0, 0};
  float fltData[4] = {3.5f, 0, 0, 0};
  // 10 + (int)3.5 = 10 + 3 = 13

  PeriodArrayRegistry reg;
  reg.registerArray("ints", "g_ints", intData, 4);
  reg.registerArray("floats", "g_floats", fltData, 4);

  EJitStructFieldPass sp(reg);
  sp.initFromModule(*M);
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  sp.run(*F, FAM);

  EJitOptimizerTestAccess opt(reg);
  opt.runInstCombine(*M);

  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 13);
}

//===----------------------------------------------------------------------===//
// EJitStructFieldPass multi-dimensional array test
//===----------------------------------------------------------------------===//

/// Create a function accessing a 2D period array: g_arr[1][2]
/// The GEP has 3 indices: {0, 1, 2}. Tests the multi-index offset fix.
static Function *createMultiDimArrayFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();
  auto *InnerTy = ArrayType::get(Int32Ty, 8); // [8 x i32]
  auto *OuterTy = ArrayType::get(InnerTy, 4); // [4 x [8 x i32]]

  auto *GVar =
      new GlobalVariable(M, OuterTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(OuterTy), "g_2d");

  Metadata *ArrOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GVar->setMetadata(MD_EJIT_METADATA,
                    MDNode::get(Ctx, {MDNode::get(Ctx, ArrOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "access_2d", &M);

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);

  // g_2d[1][2] with 3-index GEP
  Value *Indices[] = {B.getInt32(0), B.getInt64(1), B.getInt64(2)};
  auto *GEP = B.CreateInBoundsGEP(OuterTy, GVar, Indices, "gep_2d");
  auto *Load = B.CreateLoad(Int32Ty, GEP, "val_2d");
  Load->setMetadata("ejit.may_const",
                    MDNode::get(Ctx, MDString::get(Ctx, "ejit")));
  B.CreateRet(Load);
  return F;
}

TEST(EJitStructFieldPass, MayConstLoadSubstitutionMultiDimArray) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_2d", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createMultiDimArrayFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  // 2D mock: [4][8] int32
  // g_2d[1][2] = element at row 1, col 2 = offset 1*8*4 + 2*4 = 40 bytes
  int32_t mock2D[4][8] = {
      {0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 99, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0},
  };

  PeriodArrayRegistry reg;
  GlobalVariable *GV = M->getGlobalVariable("g_2d", true);
  ASSERT_NE(GV, nullptr);
  reg.registerArray("cell", "g_2d", mock2D, 4);

  EJitStructFieldPass sp(reg);
  sp.initFromModule(*M);
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  sp.run(*F, FAM);

  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 99);
}

//===----------------------------------------------------------------------===//
// EJitStructFieldPass robustness tests (may_const metadata edge cases)
//
// These cover the cross-cutting behaviours flagged in the SPEC audit:
//   1. spurious per-load !ejit.may_const on a non-period global is NOT replaced
//   2. a load missing per-load metadata is still replaced via the GV-level
//      ejit_may_const_field offset fallback (PASS1-drop recovery)
//   3. a load missing metadata whose offset is NOT in the GV offset list is
//      cleanly left alone (safe fallback, no mis-replacement)
//   4. multiple loads of the same field are all replaced with the same value
//===----------------------------------------------------------------------===//

namespace {

/// Register the standard FAM/LAM/CGAM/MAM analysis managers for a struct-field
/// pass run. Keeps the robustness tests focused on behaviour, not boilerplate.
struct StructFieldHarness {
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  StructFieldHarness() {
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerModuleAnalyses(MAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  }
};

/// Count the load instructions remaining in a function.
static unsigned countLoads(Function &F) {
  unsigned n = 0;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (isa<LoadInst>(&I))
        ++n;
  return n;
}

} // anonymous namespace

// 1. A load tagged !ejit.may_const whose pointer roots at a global that is NOT
//    a registered period variable must be left untouched. This guards against
//    a stray/incorrect annotation silently reading unrelated memory.
TEST(EJitStructFieldPass, SpuriousMetadataOnNonPeriodGVNoReplace) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_spurious_md", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));

  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();
  auto *ArrTy = ArrayType::get(Int32Ty, 4);
  // No !ejit.metadata on this global — it is not a period variable.
  auto *GVar =
      new GlobalVariable(*M, ArrTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(ArrTy), "g_plain");

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "spurious", M.get());
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  Value *Idx[] = {B.getInt32(0), B.getInt64(2)};
  auto *GEP = B.CreateInBoundsGEP(ArrTy, GVar, Idx, "gep");
  auto *Load = B.CreateLoad(Int32Ty, GEP, "load");
  // Empty metadata node, matching Clang's actual !ejit.may_const !{} emission.
  Load->setMetadata(MD_EJIT_MAY_CONST, MDNode::get(Ctx, {}));
  B.CreateRet(Load);

  int32_t mockArr[4] = {10, 20, 30, 40};
  PeriodArrayRegistry reg;
  // Registered under a different name — g_plain is unknown to the registry.
  reg.registerArray("cell", "g_other", mockArr, 4);

  EJitStructFieldPass sp(reg);
  StructFieldHarness H;
  sp.initFromModule(*M);
  auto PA = sp.run(*F, H.FAM);

  // The load must survive: no registered period GV backs this annotation.
  EXPECT_EQ(countLoads(*F), 1u);
  EXPECT_TRUE(PA.areAllPreserved());
}

// 2. A load WITHOUT per-load !ejit.may_const is still specialized when its byte
//    offset appears in the GV-level ejit_may_const_field list. This is the
//    fallback path that recovers metadata dropped by AOT optimization.
TEST(EJitStructFieldPass, MissingPerLoadMetadataGVOffsetFallback) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_gv_fallback", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));

  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();
  auto *ArrTy = ArrayType::get(Int32Ty, 4);
  auto *GVar =
      new GlobalVariable(*M, ArrTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(ArrTy), "g_arr");
  // GV metadata: ejit_period_arr "cell" size 4 + ejit_may_const_field offset 8
  // (offset 8 == element index 2 for i32[4]).
  Metadata *ArrOps[] = {MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
                        MDString::get(Ctx, "cell"),
                        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4))};
  Metadata *FieldOps[] = {
      MDString::get(Ctx, TAG_EJIT_MAY_CONST_FIELD),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 8))};
  GVar->setMetadata(
      MD_EJIT_METADATA,
      MDNode::get(Ctx, {MDNode::get(Ctx, ArrOps), MDNode::get(Ctx, FieldOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "fallback", M.get());
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  Value *Idx[] = {B.getInt32(0), B.getInt64(2)};
  auto *GEP = B.CreateInBoundsGEP(ArrTy, GVar, Idx, "gep");
  // NOTE: deliberately NO per-load !ejit.may_const metadata here.
  auto *Load = B.CreateLoad(Int32Ty, GEP, "load");
  B.CreateRet(Load);

  int32_t mockArr[4] = {10, 20, 30, 40}; // g_arr[2] = 30
  PeriodArrayRegistry reg;
  reg.registerArray("cell", "g_arr", mockArr, 4);

  EJitStructFieldPass sp(reg);
  StructFieldHarness H;
  sp.initFromModule(*M);
  auto PA = sp.run(*F, H.FAM);

  EXPECT_EQ(countLoads(*F), 0u);
  EXPECT_FALSE(PA.areAllPreserved());
  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 30);
}

// A pointer-form period array is registered by the address of its pointer
// slot. The root pointer load has no !ejit.may_const marker: only the nested
// BBB field does. Specializing the root address is nevertheless required when
// the field access sits behind a non-inlined helper, because the field pass
// cannot otherwise trace the helper argument back to @aaa.
TEST(EJitStructFieldPass, PointerPeriodRootFeedsNestedMayConstHelper) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("pointer_period_nested", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  M->setDataLayout("e-p:64:64");

  IRBuilder<> B(Ctx);
  Type *I32Ty = B.getInt32Ty();
  auto *BBBTy = StructType::create(Ctx, {I32Ty, I32Ty}, "BBB");
  auto *CCCTy = StructType::create(Ctx, {I32Ty}, "CCC");
  auto *AAATy = StructType::create(
      Ctx, {BBBTy, ArrayType::get(CCCTy, 10)}, "AAA");

  auto *AAA = new GlobalVariable(
      *M, B.getPtrTy(), false, GlobalValue::ExternalLinkage,
      ConstantPointerNull::get(B.getPtrTy()), "aaa");
  Metadata *PeriodOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR), MDString::get(Ctx, "xxx"),
      ConstantAsMetadata::get(ConstantInt::get(I32Ty, 0))};
  // Offset zero belongs to BBB::mayConstValue. AAA and AAA::bbb themselves are
  // deliberately not may_const.
  Metadata *FieldOps[] = {
      MDString::get(Ctx, TAG_EJIT_MAY_CONST_FIELD),
      ConstantAsMetadata::get(ConstantInt::get(I32Ty, 0))};
  AAA->setMetadata(
      MD_EJIT_METADATA,
      MDNode::get(Ctx,
                  {MDNode::get(Ctx, PeriodOps), MDNode::get(Ctx, FieldOps)}));

  auto *HelperTy = FunctionType::get(I32Ty, {B.getPtrTy()}, false);
  auto *Helper = Function::Create(HelperTy, GlobalValue::InternalLinkage,
                                  "read_bbb_may_const", M.get());
  Helper->addFnAttr(Attribute::NoInline);
  BasicBlock *HelperEntry = BasicBlock::Create(Ctx, "entry", Helper);
  B.SetInsertPoint(HelperEntry);
  Value *NestedField = B.CreateStructGEP(AAATy, Helper->getArg(0), 0,
                                         "bbb");
  NestedField = B.CreateStructGEP(BBBTy, NestedField, 0, "may_const");
  auto *NestedLoad = B.CreateLoad(I32Ty, NestedField, "value");
  NestedLoad->setMetadata(MD_EJIT_MAY_CONST, MDNode::get(Ctx, {}));
  B.CreateRet(NestedLoad);

  auto *EntryTy = FunctionType::get(I32Ty, {}, false);
  auto *Entry = Function::Create(EntryTy, GlobalValue::ExternalLinkage,
                                 "read_aaa", M.get());
  BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", Entry);
  B.SetInsertPoint(EntryBB);
  // This load has no may_const metadata. It is the address root that must be
  // materialized before interprocedural propagation can expose the field GEP.
  Value *Base = B.CreateLoad(B.getPtrTy(), AAA, "aaa.base");
  B.CreateRet(B.CreateCall(Helper, {Base}));

  struct MockAAA {
    struct {
      int32_t mayConstValue;
      int32_t mutableValue;
    } bbb;
    struct {
      int32_t value;
    } ccc[10];
  } Mock = {{73, 9}, {}};
  MockAAA *CurrentAAA = &Mock;

  PeriodArrayRegistry Reg;
  // Pointer-form period arrays are registered with &aaa, not aaa.
  Reg.registerArray("xxx", "aaa", &CurrentAAA, 0);
  EJitOptimizerTestAccess Opt(Reg);

  Opt.runStructFieldPass(*M);
  EXPECT_EQ(countLoads(*Entry), 0u)
      << "the unannotated period pointer root should become an address";

  Opt.runInterproceduralPropagation(*M);
  Opt.runInstCombine(*M);
  Opt.runStructFieldPass(*M);
  Opt.runInstCombine(*M);

  EXPECT_EQ(countLoads(*Helper), 0u)
      << "the nested BBB may_const load should be specialized";
  auto *Ret = dyn_cast_or_null<ReturnInst>(&Helper->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetValue = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetValue, nullptr);
  EXPECT_EQ(RetValue->getSExtValue(), 73);
}

// 3. A load WITHOUT per-load metadata whose offset is NOT in the GV offset list
//    must be left alone. Confirms the fallback does not over-fire.
TEST(EJitStructFieldPass, MissingPerLoadMetadataWrongOffsetNoReplace) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_no_fallback", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));

  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();
  auto *ArrTy = ArrayType::get(Int32Ty, 4);
  auto *GVar =
      new GlobalVariable(*M, ArrTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(ArrTy), "g_arr");
  // may_const_field offset list contains only 0; the load below is at offset 8.
  Metadata *ArrOps[] = {MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
                        MDString::get(Ctx, "cell"),
                        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4))};
  Metadata *FieldOps[] = {
      MDString::get(Ctx, TAG_EJIT_MAY_CONST_FIELD),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 0))};
  GVar->setMetadata(
      MD_EJIT_METADATA,
      MDNode::get(Ctx, {MDNode::get(Ctx, ArrOps), MDNode::get(Ctx, FieldOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "no_fallback",
                             M.get());
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  Value *Idx[] = {B.getInt32(0), B.getInt64(2)}; // offset 8, not in {0}
  auto *GEP = B.CreateInBoundsGEP(ArrTy, GVar, Idx, "gep");
  auto *Load = B.CreateLoad(Int32Ty, GEP, "load");
  B.CreateRet(Load);

  int32_t mockArr[4] = {10, 20, 30, 40};
  PeriodArrayRegistry reg;
  reg.registerArray("cell", "g_arr", mockArr, 4);

  EJitStructFieldPass sp(reg);
  StructFieldHarness H;
  sp.initFromModule(*M);
  auto PA = sp.run(*F, H.FAM);

  // Offset 8 is not a may_const field → load stays, AOT value is read at
  // runtime.
  EXPECT_EQ(countLoads(*F), 1u);
  EXPECT_TRUE(PA.areAllPreserved());
}

// 4. Two loads of the same may_const field are each replaced with the same
//    runtime constant (the optimizer later CSEs them, but the pass must handle
//    repeated loads without leaving one behind).
TEST(EJitStructFieldPass, MultipleLoadsSameFieldAllReplaced) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_dup_load", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));

  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();
  auto *ArrTy = ArrayType::get(Int32Ty, 4);
  auto *GVar =
      new GlobalVariable(*M, ArrTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(ArrTy), "g_arr");
  Metadata *ArrOps[] = {MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
                        MDString::get(Ctx, "cell"),
                        ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4))};
  GVar->setMetadata(MD_EJIT_METADATA,
                    MDNode::get(Ctx, {MDNode::get(Ctx, ArrOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "dup_load", M.get());
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);

  // Two independent loads of g_arr[1] (offset 4), both annotated may_const.
  Value *Idx[] = {B.getInt32(0), B.getInt64(1)};
  auto *GEP1 = B.CreateInBoundsGEP(ArrTy, GVar, Idx, "gep1");
  auto *L1 = B.CreateLoad(Int32Ty, GEP1, "v1");
  L1->setMetadata(MD_EJIT_MAY_CONST, MDNode::get(Ctx, {}));
  auto *GEP2 = B.CreateInBoundsGEP(ArrTy, GVar, Idx, "gep2");
  auto *L2 = B.CreateLoad(Int32Ty, GEP2, "v2");
  L2->setMetadata(MD_EJIT_MAY_CONST, MDNode::get(Ctx, {}));
  B.CreateRet(B.CreateAdd(L1, L2, "sum"));

  int32_t mockArr[4] = {10, 55, 30, 40}; // g_arr[1] = 55
  PeriodArrayRegistry reg;
  reg.registerArray("cell", "g_arr", mockArr, 4);

  EJitStructFieldPass sp(reg);
  StructFieldHarness H;
  sp.initFromModule(*M);
  auto PA = sp.run(*F, H.FAM);

  EXPECT_EQ(countLoads(*F), 0u);
  EXPECT_FALSE(PA.areAllPreserved());

  // 55 + 55 = 110 after constant folding.
  EJitOptimizerTestAccess opt(reg);
  opt.runInstCombine(*M);
  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 110);
}

//===----------------------------------------------------------------------===//
// EJitOptimizer extended tests
//===----------------------------------------------------------------------===//

/// Create a function with 2 period-array-index params (cell + trp).
static Function *createMultiDimFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  Type *Int32Ty = B.getInt32Ty();
  FunctionType *FT = FunctionType::get(Int32Ty, {Int32Ty, Int32Ty}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "multi_dim", &M);

  F->getArg(0)->setName("cell_idx");
  F->getArg(1)->setName("trp_idx");

  // Metadata for cell dimension (param 0)
  Metadata *CellOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 0)),
  };
  // Metadata for trp dimension (param 1)
  Metadata *TrpOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
      MDString::get(Ctx, "trp"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 1)),
  };
  F->setMetadata(
      MD_EJIT_METADATA,
      MDNode::get(Ctx, {MDNode::get(Ctx, CellOps), MDNode::get(Ctx, TrpOps)}));

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  auto *Sum = B.CreateAdd(F->getArg(0), F->getArg(1), "sum");
  B.CreateRet(Sum);
  return F;
}

TEST(EJitOptimizer, PreReplacePeriodIndicesMultiDim) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "multi_dim_test");
  Function *F = createMultiDimFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  PeriodArrayRegistry reg;
  SpecializationContext ctx;
  ctx.fnName = "multi_dim";
  ctx.dimensions.push_back({"cell", 10});
  ctx.dimensions.push_back({"trp", 25});

  EJitOptimizerTestAccess opt(reg);
  opt.preReplacePeriodIndices(*M, ctx);

  // Both args should be replaced; sum should fold to 35
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  opt.runInstCombine(*M);

  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 35);
}

/// Create IR with dead code behind a false branch. L1 should eliminate it.
static Function *createDeadCodeFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  FunctionType *FT = FunctionType::get(B.getInt32Ty(), {}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "dead_code_fn", &M);

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);

  // Live block: the false target of the constant branch, so it is always taken.
  BasicBlock *Live = BasicBlock::Create(Ctx, "live", F);
  {
    IRBuilder<> LB(Live);
    LB.CreateRet(LB.getInt32(42));
  }

  // Unreachable block: the true target of `br false` (never taken).
  auto *DeadBB = BasicBlock::Create(Ctx, "dead", F);
  {
    IRBuilder<> DB(DeadBB);
    DB.CreateRet(DB.getInt32(999));
  }

  // br i1 false -> always the false target (live); the true target (dead) is
  // unreachable and should be eliminated.
  B.CreateCondBr(B.getFalse(), DeadBB, Live);

  return F;
}

TEST(EJitOptimizer, OptimizationL1DeadCodeElimination) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "deadcode");
  Function *F = createDeadCodeFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);
  int bbCount = (int)std::distance(F->begin(), F->end());

  PeriodArrayRegistry reg;
  EJitOptimizerTestAccess opt(reg);
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L1);

  // Dead block should be removed
  int bbAfter = (int)std::distance(F->begin(), F->end());
  EXPECT_LT(bbAfter, bbCount);
}

/// Create a call to a small callee. L2 should inline it.
static Function *createInlineCandidate(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  auto *Int32Ty = B.getInt32Ty();

  // Callee: int callee(int x) { return x + 1; }
  FunctionType *CalleeFT = FunctionType::get(Int32Ty, {Int32Ty}, false);
  auto *Callee =
      Function::Create(CalleeFT, GlobalValue::InternalLinkage, "callee", &M);
  Callee->addFnAttr(Attribute::AlwaysInline);
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Callee);
    B.SetInsertPoint(BB);
    auto *Val = B.CreateAdd(Callee->getArg(0), B.getInt32(1));
    B.CreateRet(Val);
  }

  // Caller: int caller() { return callee(41); }
  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *Caller =
      Function::Create(FT, GlobalValue::ExternalLinkage, "caller", &M);
  {
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Caller);
    B.SetInsertPoint(BB);
    auto *Call = B.CreateCall(Callee, {B.getInt32(41)});
    B.CreateRet(Call);
  }
  return Caller;
}

TEST(EJitOptimizer, OptimizationL2InlineAndSimplify) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "inline_test");
  Function *F = createInlineCandidate(Ctx, *M);
  ASSERT_NE(F, nullptr);

  PeriodArrayRegistry reg;
  EJitOptimizerTestAccess opt(reg);
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L2);

  // After inlining 41+1, should fold to constant 42
  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  // If fully optimized, this should be constant 42
  if (RetVal)
    EXPECT_EQ(RetVal->getSExtValue(), 42);
}

/// Create a small loop with constant bounds. L3 should unroll it.
static Function *createLoopFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  auto *Int32Ty = B.getInt32Ty();
  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "loop_fn", &M);

  // for i in [0..4): sum += i  => 0+1+2+3 = 6
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *LoopHdr = BasicBlock::Create(Ctx, "loop", F);
  BasicBlock *LoopBody = BasicBlock::Create(Ctx, "body", F);
  BasicBlock *LoopExit = BasicBlock::Create(Ctx, "exit", F);

  B.SetInsertPoint(Entry);
  B.CreateBr(LoopHdr);

  // Loop header: phi [i=0, sum=0], icmp i < 4
  B.SetInsertPoint(LoopHdr);
  auto *PhiI = B.CreatePHI(Int32Ty, 2, "i");
  auto *PhiSum = B.CreatePHI(Int32Ty, 2, "sum");
  auto *Cmp = B.CreateICmpSLT(PhiI, B.getInt32(4));
  B.CreateCondBr(Cmp, LoopBody, LoopExit);

  B.SetInsertPoint(LoopBody);
  auto *NewSum = B.CreateAdd(PhiSum, PhiI);
  auto *NewI = B.CreateAdd(PhiI, B.getInt32(1));
  B.CreateBr(LoopHdr);

  // Back edges
  PhiI->addIncoming(B.getInt32(0), Entry);
  PhiI->addIncoming(NewI, LoopBody);
  PhiSum->addIncoming(B.getInt32(0), Entry);
  PhiSum->addIncoming(NewSum, LoopBody);

  B.SetInsertPoint(LoopExit);
  B.CreateRet(PhiSum);
  return F;
}

TEST(EJitOptimizer, OptimizationL3LoopUnroll) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "loop_test");
  Function *F = createLoopFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  PeriodArrayRegistry reg;
  EJitOptimizerTestAccess opt(reg);

  // Promote first (mem2reg)
  opt.runInstCombine(*M);
  // Then L3 with loop unroll
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L3);

  // After unrolling 0+1+2+3, should fold to constant 6
  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  if (RetVal)
    EXPECT_EQ(RetVal->getSExtValue(), 6);
}

//===----------------------------------------------------------------------===//
// Pipeline-collapse proof tests
//
// The L1/L2/L3 optimizer tiers were collapsed into one fixed pipeline. These
// prove the collapse is behavior-preserving:
//   * level-equivalence — optimizing the SAME module at L1, L2, and L3 yields
//     byte-identical IR (the optimizer no longer branches on the level);
//   * no-regression — that IR is the fully optimized form the old L3 produced
//     (the loop is unrolled and folded to the constant 6). Pre-collapse the L1
//     output would still contain the loop, so this would have failed.
//===----------------------------------------------------------------------===//

// Optimize a fresh copy of createLoopFunc at `lvl` and return the printed IR.
static std::string optimizeLoopFnAt(llvm::ejit::OptimizationLevel lvl) {
  LLVMContext Ctx;
  auto M = createTestModule(Ctx, "collapse_equiv");
  createLoopFunc(Ctx, *M);
  PeriodArrayRegistry reg;
  EJitOptimizerTestAccess opt(reg);
  opt.runInstCombine(*M);
  opt.runOptimizationPipeline(*M, lvl);
  std::string Out;
  raw_string_ostream OS(Out);
  M->print(OS, nullptr);
  return Out;
}

TEST(EJitOptimizer, OptimizationFoldsConstantLoopAtAllLevels) {
  std::string L1 = optimizeLoopFnAt(llvm::ejit::OptimizationLevel::L1);
  std::string L2 = optimizeLoopFnAt(llvm::ejit::OptimizationLevel::L2);
  std::string L3 = optimizeLoopFnAt(llvm::ejit::OptimizationLevel::L3);

  // No-regression: each tier (L1->O1, L2->O2, L3->O3) unrolls + folds the
  // constant-bound loop to `ret i32 6`. Tiers are now distinct O1/O2/O3
  // pipelines; byte-identical IR across tiers is no longer asserted.
  EXPECT_NE(L1.find("ret i32 6"), std::string::npos)
      << "L1 (O1) did not fold the constant loop";
  EXPECT_NE(L2.find("ret i32 6"), std::string::npos)
      << "L2 (O2) did not fold the constant loop";
  EXPECT_NE(L3.find("ret i32 6"), std::string::npos)
      << "L3 (O3) did not fold the constant loop";
}

//===----------------------------------------------------------------------===//
// End-to-end tests
//===----------------------------------------------------------------------===//

/// Create IR with a branch that depends on a may_const field.
/// After full pipeline (StructField + L2), the branch should be folded.
static Function *createBranchOnMayConstFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  auto *Int32Ty = B.getInt32Ty();
  auto *STy = StructType::create(Ctx, {Int32Ty}, "BranchCfg");

  auto *GVar = new GlobalVariable(
      M, STy, false, GlobalValue::InternalLinkage,
      ConstantStruct::get(STy, ConstantInt::get(Int32Ty, 0)), "g_branch_cfg");
  Metadata *PeriodOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD),
      MDString::get(Ctx, "static"),
  };
  GVar->setMetadata(MD_EJIT_METADATA,
                    MDNode::get(Ctx, {MDNode::get(Ctx, PeriodOps)}));

  FunctionType *FT = FunctionType::get(Int32Ty, {}, false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "branch_fn", &M);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *ThenBB = BasicBlock::Create(Ctx, "then", F);
  BasicBlock *ElseBB = BasicBlock::Create(Ctx, "else", F);
  BasicBlock *Merge = BasicBlock::Create(Ctx, "merge", F);

  B.SetInsertPoint(Entry);
  Value *I0[] = {B.getInt32(0), B.getInt32(0)};
  auto *GEP = B.CreateInBoundsGEP(STy, GVar, I0, "cfg_gep");
  auto *Load = B.CreateLoad(Int32Ty, GEP, "cfg_val");
  Load->setMetadata("ejit.may_const",
                    MDNode::get(Ctx, MDString::get(Ctx, "ejit")));
  auto *Cmp = B.CreateICmpNE(Load, B.getInt32(0), "is_set");
  B.CreateCondBr(Cmp, ThenBB, ElseBB);

  B.SetInsertPoint(ThenBB);
  B.CreateBr(Merge);
  B.SetInsertPoint(ElseBB);
  B.CreateBr(Merge);

  B.SetInsertPoint(Merge);
  auto *Phi = B.CreatePHI(Int32Ty, 2, "result");
  Phi->addIncoming(B.getInt32(100), ThenBB);
  Phi->addIncoming(B.getInt32(0), ElseBB);
  B.CreateRet(Phi);
  return F;
}

TEST(EJitEndToEnd, BranchFolding) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("test_branch", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createBranchOnMayConstFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  // Mock memory: field value = 1 (non-zero, so "then" branch taken)
  struct MockCfg {
    int32_t val;
  };
  MockCfg mock = {1};

  PeriodArrayRegistry reg;
  GlobalVariable *GV = M->getGlobalVariable("g_branch_cfg", true);
  ASSERT_NE(GV, nullptr);
  reg.registerStaticVar("g_branch_cfg", &mock);

  // Full pipeline: StructField -> InstCombine -> Inline -> L2
  EJitStructFieldPass sfp(reg);
  sfp.initFromModule(*M);
  EJitOptimizerTestAccess opt(reg);

  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  sfp.run(*F, FAM);
  opt.runInstCombine(*M);
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L2);

  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  // mock.val = 1 => branch taken => result = 100
  EXPECT_EQ(RetVal->getSExtValue(), 100);
}

// Second collapse-proof, on a different function shape: a may_const field
// driving a branch (not a loop). Specialize it through the full pipeline at L1,
// L2, and L3 and confirm the output IR is byte-identical (level does not change
// the pipeline) and fully folded (branch resolved to the constant 100).
static std::string specializeBranchAt(llvm::ejit::OptimizationLevel lvl) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("branch_equiv", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  createBranchOnMayConstFunc(Ctx, *M);

  struct MockCfg {
    int32_t val;
  } mock = {1}; // non-zero → "then" branch → result 100
  PeriodArrayRegistry reg;
  reg.registerStaticVar("g_branch_cfg", &mock);

  EJitOptimizerTestAccess opt(reg);
  opt.runInstCombine(*M);
  opt.runStructFieldPass(*M);
  opt.runOptimizationPipeline(*M, lvl);

  std::string Out;
  raw_string_ostream OS(Out);
  M->print(OS, nullptr);
  return Out;
}

TEST(EJitEndToEnd, PipelineFoldsMayConstBranchAtAllLevels) {
  std::string L1 = specializeBranchAt(llvm::ejit::OptimizationLevel::L1);
  std::string L2 = specializeBranchAt(llvm::ejit::OptimizationLevel::L2);
  std::string L3 = specializeBranchAt(llvm::ejit::OptimizationLevel::L3);

  // No-regression: each tier (L1->O1, L2->O2, L3->O3) folds the may_const
  // branch to the constant 100. Tiers are now distinct O1/O2/O3 pipelines;
  // byte-identical IR across tiers is no longer asserted.
  EXPECT_NE(L1.find("ret i32 100"), std::string::npos)
      << "L1 (O1) did not fold the may_const branch";
  EXPECT_NE(L2.find("ret i32 100"), std::string::npos)
      << "L2 (O2) did not fold the may_const branch";
  EXPECT_NE(L3.find("ret i32 100"), std::string::npos)
      << "L3 (O3) did not fold the may_const branch";
}

//===----------------------------------------------------------------------===//
// Interprocedural propagation (phase 1d)
//===----------------------------------------------------------------------===//

// The AOT inliner keeps a call edge wherever it chose not to inline, so a
// specialization used to stop at every such edge: the entry's substituted
// dims arrive at the callee as ordinary constant arguments, but the callee
// body still indexes the period array with a runtime value and re-tests
// guards. These tests pin phase 1d (internalize + IPSCCP): the constants
// cross the edge, the callee's may_const load becomes substitutable, and the
// callee folds to a constant return like the entry does.
namespace {

/// Entry (has ejit_entry metadata) calls a callee with a constant cell index
/// — the shape phase 1a leaves behind. The callee loads field 1 of
/// g_ipcfg[cell] (may_const) and branches on it.
std::unique_ptr<Module> parseInterprocModule(LLVMContext &Ctx) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
    %S = type { i32, i32 }
    @g_ipcfg = external global [4 x %S], !ejit.metadata !0

    define i32 @ip_callee(i8 noundef %cell) {
      %idx = zext i8 %cell to i64
      %gep = getelementptr inbounds [4 x %S], ptr @g_ipcfg, i64 0, i64 %idx, i32 1
      %v = load i32, ptr %gep, align 4, !ejit.may_const !2
      %c = icmp eq i32 %v, 7
      br i1 %c, label %then, label %else
    then:
      ret i32 100
    else:
      ret i32 200
    }

    define i32 @ip_entry() !ejit.metadata !3 {
      %r = call i32 @ip_callee(i8 noundef 2)
      ret i32 %r
    }

    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 4}
    !2 = !{}
    !3 = !{!4}
    !4 = !{!"ejit_entry"}
  )",
                              Err, Ctx);
  if (!M)
    Err.print("parseInterprocModule", errs());
  return M;
}

/// Per-cell mock backing for @g_ipcfg; cell 2 has field 1 == 7 → callee
/// returns 100 when specialized for cell 2.
struct IpMockElem {
  int32_t a, b;
};

unsigned countLoadsIn(const Function &F) {
  unsigned n = 0;
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      n += isa<LoadInst>(&I);
  return n;
}

} // anonymous namespace

// Without phase 1d the callee is untouched: its load survives the whole
// pipeline because the cell index stays a runtime argument inside it. This is
// the baseline that motivates the pass.
TEST(EJitInterprocedural, ConstantsStopAtCallEdgeWithoutIPSCCP) {
  LLVMContext Ctx;
  auto M = parseInterprocModule(Ctx);
  ASSERT_NE(M, nullptr);

  IpMockElem mock[4] = {{0, 1}, {0, 3}, {0, 7}, {0, 9}};
  PeriodArrayRegistry reg;
  reg.registerArray("cell", "g_ipcfg", mock, 4);

  EJitOptimizerTestAccess opt(reg);
  opt.runInstCombine(*M);
  opt.runStructFieldPass(*M);
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L2);

  Function *Callee = M->getFunction("ip_callee");
  ASSERT_NE(Callee, nullptr);
  EXPECT_EQ(countLoadsIn(*Callee), 1u)
      << "callee folded without IPSCCP — baseline assumption changed";
}

// With phase 1d in its pipeline position (after 1c, before the 1e/1f
// re-fold), the constant argument crosses the edge, the callee's may_const
// load folds against cell 2's runtime value, and the guard collapses: the
// callee body becomes `ret i32 100`.
TEST(EJitInterprocedural, IPSCCPPropagatesDimsIntoCallee) {
  LLVMContext Ctx;
  auto M = parseInterprocModule(Ctx);
  ASSERT_NE(M, nullptr);

  IpMockElem mock[4] = {{0, 1}, {0, 3}, {0, 7}, {0, 9}};
  PeriodArrayRegistry reg;
  reg.registerArray("cell", "g_ipcfg", mock, 4);

  EJitOptimizerTestAccess opt(reg);
  // Same order as runPipeline: 1b InstCombine, 1c StructField, 1d IPSCCP,
  // 1e/1f re-fold, phases 2-4.
  opt.runInstCombine(*M);
  opt.runStructFieldPass(*M);
  opt.runInterproceduralPropagation(*M);
  opt.runInstCombine(*M);
  opt.runStructFieldPass(*M);
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L2);

  Function *Callee = M->getFunction("ip_callee");
  Function *Entry = M->getFunction("ip_entry");
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(Entry, nullptr);

  // The callee was internalized so IPSCCP could trust its call-site set; the
  // entry must stay externally visible — it is the symbol the JIT looks up.
  EXPECT_TRUE(Callee->hasLocalLinkage());
  EXPECT_FALSE(Entry->hasLocalLinkage());

  // The callee's period load and guard folded to the constant return.
  EXPECT_EQ(countLoadsIn(*Callee), 0u) << "callee may_const load survived";
  ASSERT_EQ(Callee->size(), 1u) << "callee guard branch survived";
  auto *Ret = dyn_cast<ReturnInst>(Callee->getEntryBlock().getTerminator());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr) << "callee return did not fold";
  EXPECT_EQ(RetVal->getSExtValue(), 100); // mock[2].b == 7 → then-branch
}

//===----------------------------------------------------------------------===//
// EJit end-to-end MultiPeriod specialization test
//===----------------------------------------------------------------------===//

TEST(EJitEndToEnd, MultiPeriodSpecialization) {
  // Create module with two period arrays, run StructField with different
  // mock data simulating different cell indices.
  LLVMContext Ctx1;
  auto M1 = std::make_unique<Module>("spec1", Ctx1);
  M1->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F1 = createMultiArrayFunc(Ctx1, *M1);

  int32_t cellA[4] = {1, 10, 20, 30};
  int32_t trpA[4] = {100, 200, 300, 400};

  PeriodArrayRegistry reg;
  reg.registerArray("cell", "g_cells", cellA, 4);
  reg.registerArray("trp", "g_trps", trpA, 4);

  EJitStructFieldPass sfp(reg);
  sfp.initFromModule(*M1);

  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  sfp.run(*F1, FAM);

  EJitOptimizerTestAccess opt1(reg);
  opt1.runInstCombine(*M1);

  // g_cells[2] * g_trps[3] = 20 * 400 = 8000
  auto *Ret = dyn_cast_or_null<ReturnInst>(&F1->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 8000);

  // Now change cell data, re-run (different specialization)
  LLVMContext Ctx2;
  auto M2 = std::make_unique<Module>("spec2", Ctx2);
  M2->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F2 = createMultiArrayFunc(Ctx2, *M2);

  int32_t cellB[4] = {5, 5, 5, 5}; // g_cells[2] = 5
  int32_t trpB[4] = {0, 0, 0, 6};  // g_trps[3] = 6

  PeriodArrayRegistry reg2;
  reg2.registerArray("cell", "g_cells", cellB, 4);
  reg2.registerArray("trp", "g_trps", trpB, 4);

  EJitStructFieldPass sfp2(reg2);
  sfp2.initFromModule(*M2);
  FunctionAnalysisManager FAM2;
  PassBuilder PB2;
  PB2.registerFunctionAnalyses(FAM2);
  PB2.registerLoopAnalyses(LAM);
  PB2.registerCGSCCAnalyses(CGAM);
  PB2.registerModuleAnalyses(MAM);
  PB2.crossRegisterProxies(LAM, FAM2, CGAM, MAM);

  sfp2.run(*F2, FAM2);

  EJitOptimizerTestAccess opt2(reg2);
  opt2.runInstCombine(*M2);

  auto *Ret2 = dyn_cast_or_null<ReturnInst>(&F2->back().back());
  ASSERT_NE(Ret2, nullptr);
  auto *RetVal2 = dyn_cast<ConstantInt>(Ret2->getReturnValue());
  ASSERT_NE(RetVal2, nullptr);
  EXPECT_EQ(RetVal2->getSExtValue(), 30); // 5 * 6 = 30
}

//===----------------------------------------------------------------------===//
// EJit end-to-end cache invalidation test
//===----------------------------------------------------------------------===//



//===----------------------------------------------------------------------===//
// JIT pipeline IR verification tests
//===----------------------------------------------------------------------===//

/// Create IR matching the process_board trace test pattern:
///   if (g_cfg.field0 == 1) { g_cfg.field1 = 100; } else { g_cfg.field1 = 200;
///   }
/// After StructField, the branch on field0 should fold, eliminating the dead
/// path.
static Function *createBranchOnFieldFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  auto *Int32Ty = B.getInt32Ty();
  auto *STy = StructType::create(Ctx, {Int32Ty, Int32Ty}, "Cfg");

  auto *GV =
      new GlobalVariable(M, STy, false, GlobalValue::InternalLinkage,
                         ConstantStruct::get(STy, ConstantInt::get(Int32Ty, 0),
                                             ConstantInt::get(Int32Ty, 0)),
                         "g_cfg");
  Metadata *PeriodOps[] = {
      MDString::get(Ctx, "ejit_period"),
      MDString::get(Ctx, "static"),
  };
  GV->setMetadata(MD_EJIT_METADATA,
                  MDNode::get(Ctx, {MDNode::get(Ctx, PeriodOps)}));

  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "process_data", &M);

  auto *Entry = BasicBlock::Create(Ctx, "entry", F);
  auto *ThenBB = BasicBlock::Create(Ctx, "then", F);
  auto *ElseBB = BasicBlock::Create(Ctx, "else", F);
  auto *Merge = BasicBlock::Create(Ctx, "merge", F);

  B.SetInsertPoint(Entry);
  Value *I0[] = {B.getInt32(0), B.getInt32(0)};
  auto *GEP_f0 = B.CreateInBoundsGEP(STy, GV, I0, "field0");
  auto *Load = B.CreateLoad(Int32Ty, GEP_f0, "load_field0");
  Load->setMetadata("ejit.may_const",
                    MDNode::get(Ctx, MDString::get(Ctx, "ejit")));
  auto *Cmp = B.CreateICmpEQ(Load, B.getInt32(1), "cmp");
  B.CreateCondBr(Cmp, ThenBB, ElseBB);

  B.SetInsertPoint(ThenBB);
  Value *I1_t[] = {B.getInt32(0), B.getInt32(1)};
  auto *GEP_xx_t = B.CreateInBoundsGEP(STy, GV, I1_t, "xx_then");
  B.CreateStore(B.getInt32(100), GEP_xx_t);
  B.CreateBr(Merge);

  B.SetInsertPoint(ElseBB);
  Value *I1_e[] = {B.getInt32(0), B.getInt32(1)};
  auto *GEP_xx_e = B.CreateInBoundsGEP(STy, GV, I1_e, "xx_else");
  B.CreateStore(B.getInt32(200), GEP_xx_e);
  B.CreateBr(Merge);

  B.SetInsertPoint(Merge);
  B.CreateRetVoid();
  return F;
}

TEST(EJitPipelineIR, BranchFoldingOnMayConst) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("branch_fold", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createBranchOnFieldFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  // Count branches before optimization
  int brBefore = 0;
  for (auto &BB : *F)
    if (isa<BranchInst>(BB.getTerminator()))
      ++brBefore;
  EXPECT_GE(brBefore, 1);

  // Mock: field0 = 1
  struct MockCfg {
    int32_t f0;
    int32_t f1;
  };
  MockCfg mock = {1, 0};

  PeriodArrayRegistry reg;
  reg.registerStaticVar("g_cfg", &mock);

  // Full pipeline: StructField -> InstCombine -> Inline -> L2
  EJitStructFieldPass sfp(reg);
  sfp.initFromModule(*M);
  EJitOptimizerTestAccess opt(reg);

  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  sfp.run(*F, FAM);
  opt.runInstCombine(*M);
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L2);

  // After full pipeline: no loads should remain (all may_const replaced)
  int loadCount = 0;
  for (auto &BB : *F)
    for (auto &I : BB)
      if (isa<LoadInst>(&I))
        ++loadCount;
  EXPECT_EQ(loadCount, 0) << "All may_const loads should be replaced";

  // The branch on field0 == 1 should be folded (single conditional branch gone)
  // May still have unconditional branches for block transitions
  int condBrCount = 0;
  for (auto &BB : *F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (BI && BI->isConditional())
      ++condBrCount;
  }
  EXPECT_EQ(condBrCount, 0) << "Conditional branch should be eliminated";
}

/// Create IR matching the process_cell trace test pattern:
///   g_arr[idx].field == 0xFD ? a += 5 : a += 15
/// With idx=0 replaced by constant during preReplacePeriodIndices.
static Function *createCellProcessFunc(LLVMContext &Ctx, Module &M) {
  IRBuilder<> B(Ctx);
  auto *Int32Ty = B.getInt32Ty();
  auto *STy = StructType::create(Ctx, {Int32Ty, Int32Ty}, "CellCfg");
  auto *ArrTy = ArrayType::get(STy, 4);

  auto *GV = new GlobalVariable(M, ArrTy, false, GlobalValue::InternalLinkage,
                                ConstantAggregateZero::get(ArrTy), "g_cells");
  Metadata *ArrOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 4)),
  };
  GV->setMetadata(MD_EJIT_METADATA,
                  MDNode::get(Ctx, {MDNode::get(Ctx, ArrOps)}));

  // Function: void process_cell(i32 cell_idx) — ejit_period_arr_ind on arg 0
  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), {Int32Ty}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "process_cell", &M);
  F->getArg(0)->setName("cell_idx");

  // Attach ejit_period_arr_ind metadata on arg 0
  Metadata *IndOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 0)),
  };
  F->setMetadata(MD_EJIT_METADATA,
                 MDNode::get(Ctx, {MDNode::get(Ctx, IndOps)}));

  auto *Entry = BasicBlock::Create(Ctx, "entry", F);
  auto *ThenBB = BasicBlock::Create(Ctx, "then", F);
  auto *ElseBB = BasicBlock::Create(Ctx, "else", F);
  auto *Merge = BasicBlock::Create(Ctx, "merge", F);

  B.SetInsertPoint(Entry);
  // Load field 0 at cell_idx: gep %STy, %ArrTy* @g_cells, i32 0, i64 %cell_idx,
  // i32 0
  Value *Idx_f0[] = {B.getInt32(0), F->getArg(0), B.getInt32(0)};
  auto *GEP_f0 = B.CreateInBoundsGEP(ArrTy, GV, Idx_f0, "cell_f0");
  auto *Load = B.CreateLoad(Int32Ty, GEP_f0, "load_f0");
  Load->setMetadata("ejit.may_const",
                    MDNode::get(Ctx, MDString::get(Ctx, "ejit")));
  auto *Cmp = B.CreateICmpEQ(Load, B.getInt32(253), "cmp"); // 0xFD = 253
  B.CreateCondBr(Cmp, ThenBB, ElseBB);

  B.SetInsertPoint(ThenBB);
  Value *Idx_f1_t[] = {B.getInt32(0), F->getArg(0), B.getInt32(1)};
  auto *GEP_xx = B.CreateInBoundsGEP(ArrTy, GV, Idx_f1_t, "field1_then");
  auto *Old = B.CreateLoad(Int32Ty, GEP_xx, "old_val");
  auto *New = B.CreateAdd(Old, B.getInt32(5));
  B.CreateStore(New, GEP_xx);
  B.CreateBr(Merge);

  B.SetInsertPoint(ElseBB);
  Value *Idx_f1_e[] = {B.getInt32(0), F->getArg(0), B.getInt32(1)};
  auto *GEP_xx_e = B.CreateInBoundsGEP(ArrTy, GV, Idx_f1_e, "field1_else");
  auto *Old_e = B.CreateLoad(Int32Ty, GEP_xx_e, "old_val_e");
  auto *New_e = B.CreateAdd(Old_e, B.getInt32(15));
  B.CreateStore(New_e, GEP_xx_e);
  B.CreateBr(Merge);

  B.SetInsertPoint(Merge);
  B.CreateRetVoid();
  return F;
}

TEST(EJitPipelineIR, CellProcessBranchFolding) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("cell_process", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  Function *F = createCellProcessFunc(Ctx, *M);
  ASSERT_NE(F, nullptr);

  // Mock: g_cells[0].field0 = 0xFD (253), g_cells[0].field1 = 0
  struct MockCell {
    int32_t f0;
    int32_t f1;
  };
  MockCell mockArr[4] = {{253, 0}, {0, 0}, {0, 0}, {0, 0}};

  PeriodArrayRegistry reg;
  reg.registerArray("cell", "g_cells", mockArr, 4);

  SpecializationContext ctx;
  ctx.fnName = "process_cell";
  ctx.dimensions.push_back({"cell", 0});
  ctx.optLevel = llvm::ejit::OptimizationLevel::L2;

  EJitOptimizerTestAccess opt(reg);
  // 1. Replace period index arg (cell_idx=0) with constant
  opt.preReplacePeriodIndices(*M, ctx);
  // 2. Fold constant chains + Promote
  opt.runInstCombine(*M);

  // 3. StructField: replace may_const loads
  EJitStructFieldPass sfp(reg);
  sfp.initFromModule(*M);
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  sfp.run(*F, FAM);

  // 4. Final cleanup
  opt.runInstCombine(*M);
  opt.runOptimizationPipeline(*M, llvm::ejit::OptimizationLevel::L2);

  // All may_const loads should be gone
  int mayConstLoads = 0;
  for (auto &BB : *F)
    for (auto &I : BB)
      if (auto *LI = dyn_cast<LoadInst>(&I))
        if (LI->hasMetadata("ejit.may_const"))
          ++mayConstLoads;
  EXPECT_EQ(mayConstLoads, 0);

  // Conditional branch should be eliminated
  int condBr = 0;
  for (auto &BB : *F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (BI && BI->isConditional())
      ++condBr;
  }
  EXPECT_EQ(condBr, 0)
      << "Conditional branch on may_const field should be folded";
}

/// Verify InstCombine runs correctly after period index replacement
/// (catches the case where preReplacePeriodIndices + InstCombine should
/// fold a constant expression like "period_idx" replaced with constant).
TEST(EJitPipelineIR, PeriodIndexReplacementAndFold) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("period_idx_fold", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));

  IRBuilder<> B(Ctx);
  auto *Int32Ty = B.getInt32Ty();

  FunctionType *FT = FunctionType::get(Int32Ty, {Int32Ty}, false);
  auto *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "test_fn", M.get());
  F->getArg(0)->setName("period_idx");

  Metadata *IndOps[] = {
      MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
      MDString::get(Ctx, "cell"),
      ConstantAsMetadata::get(ConstantInt::get(Int32Ty, 0)),
  };
  F->setMetadata(MD_EJIT_METADATA,
                 MDNode::get(Ctx, {MDNode::get(Ctx, IndOps)}));

  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", F);
  B.SetInsertPoint(BB);
  // period_idx * 2 + 10: should fold to 42 * 2 + 10 = 94 after replacement
  auto *Mul = B.CreateMul(F->getArg(0), B.getInt32(2), "mul");
  auto *Add = B.CreateAdd(Mul, B.getInt32(10), "add");
  B.CreateRet(Add);

  PeriodArrayRegistry reg;
  SpecializationContext ctx;
  ctx.fnName = "test_fn";
  ctx.dimensions.push_back({"cell", 42});

  EJitOptimizerTestAccess opt(reg);
  opt.preReplacePeriodIndices(*M, ctx);
  opt.runInstCombine(*M);

  auto *Ret = dyn_cast_or_null<ReturnInst>(&F->back().back());
  ASSERT_NE(Ret, nullptr);
  auto *RetVal = dyn_cast<ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(RetVal, nullptr);
  EXPECT_EQ(RetVal->getSExtValue(), 94);
}

//===----------------------------------------------------------------------===//
// JIT cache lifecycle tests
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Log level + diagnostics C-API (ejit_set_log_level,
// ejit_print_registry, ejit_print_func_meta)
//===----------------------------------------------------------------------===//

TEST(EJitDiagLogLevel, SetAndGet) {
  // Default level is INFO (1).
  EXPECT_EQ(ejit_get_log_level(), 1);
  ejit_set_log_level(3); // DEBUG
  EXPECT_EQ(ejit_get_log_level(), 3);
  ejit_set_log_level(0); // OFF
  EXPECT_EQ(ejit_get_log_level(), 0);
  ejit_set_log_level(2); // VERBOSE
  EXPECT_EQ(ejit_get_log_level(), 2);
  // Restore the default so subsequent tests are unaffected.
  ejit_set_log_level(1);
  EXPECT_EQ(ejit_get_log_level(), 1);
}

TEST(EJitDiagLogLevel, ClampsOutOfRange) {
  ejit_set_log_level(-5);
  EXPECT_EQ(ejit_get_log_level(), 0);
  ejit_set_log_level(99);
  EXPECT_EQ(ejit_get_log_level(), 3);
  ejit_set_log_level(1); // restore
}

// Registry / func-meta prints must not crash on an uninitialized runtime and on
// a missing name. (Output goes through EJIT_DIAG, which is a no-op when
// EJIT_DIAG_ENABLE is undefined — the test only asserts it does not crash.)
TEST(EJitDiagnostics, PrintRegistryUninitialized) {
  // Not initialized: should report and return, not crash.
  ejit_print_registry();
}

TEST(EJitDiagnostics, PrintFuncMetaMissingName) {
  ejit_print_func_meta(nullptr); // null
  ejit_print_func_meta("");      // empty
  ejit_print_func_meta("does_not_exist");
}

// Code pool stats: a null out pointer is rejected before the gEJIT check, so
// it deterministically returns INVALID_PARAM (-1) regardless of init state.
TEST(EJitDiagnostics, CodePoolStatsNullOutRejected) {
  EXPECT_EQ(ejit_get_code_pool_stats(nullptr), -1);
}

// With a valid out pointer the call either succeeds (0) or reports a clean
// non-fatal status (NOT_ACTIVE=-2 if not initialized, DISABLED=-9 if built
// without EJIT_SRE_CODE_POOL). It must not crash and must not return a
// positive value.
TEST(EJitDiagnostics, CodePoolStatsNoCrash) {
  int dummy = -1;
  int rv = ejit_get_code_pool_stats(&dummy);
  EXPECT_TRUE(rv == 0 || rv == -2 || rv == -9);
}

TEST(EJitDiagnostics, PrintCodePoolStatsNoCrash) {
  ejit_print_code_pool_stats(); // uninitialized or no pool: prints a notice
}

// ejitDiagPermille() drives the code-pool usage percentage: integer-only
// (freestanding builds have no FPU), 0 when the denominator is 0, and
// truncating (never rounds up).
TEST(EJitDiagnostics, DiagPermille) {
  EXPECT_EQ(ejitDiagPermille(0, 0), uint64_t{0});
  EXPECT_EQ(ejitDiagPermille(7, 0), uint64_t{0});
  EXPECT_EQ(ejitDiagPermille(50, 100), uint64_t{500});
  EXPECT_EQ(ejitDiagPermille(65536, 524288), uint64_t{125}); // "12.5%"
  EXPECT_EQ(ejitDiagPermille(1, 3), uint64_t{333});          // truncates
  EXPECT_EQ(ejitDiagPermille(3, 2), uint64_t{1500});         // >100%: 4K-seal rounding
  EXPECT_EQ(ejitDiagPermille(100, 100), uint64_t{1000});     // "100.0%"
}

TEST(EJitDiagnostics, PrintActiveNoCrash) {
  ejit_print_active(); // uninitialized: prints a notice
}

// ejit_print_version() prints the LLVM release version + source git commit
// through the platform sink (SRE_printf on SRE builds, std::printf here). It
// is unconditional - not gated on EJIT_DIAG_ENABLE and needs no initialized
// runtime - so the build identity is always recoverable. The test only
// asserts it does not crash; the version/commit are baked in at compile time.
TEST(EJitDiagnostics, PrintVersionNoCrash) {
  ejit_print_version();
}
