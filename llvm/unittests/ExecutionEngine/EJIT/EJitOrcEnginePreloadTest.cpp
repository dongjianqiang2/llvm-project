//===-- EJitOrcEnginePreloadTest.cpp - bitcode preload cache -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests the parsed-bitcode cache. Parse count and retained footprint are
// observable through getBitcodeCacheStats(), so these assert on that directly
// rather than inferring caching from JIT output.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// A module with `NEntries` entry points, each returning \p Base plus its
/// index. Mirrors AOT layout: one blob holds every ejit_entry in a TU.
std::string makeBitcode(uint32_t Base, unsigned NEntries = 1) {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("ejit_preload_test", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  for (unsigned I = 0; I < NEntries; ++I) {
    auto *F = Function::Create(FunctionType::get(I32, /*isVarArg=*/false),
                               GlobalValue::ExternalLinkage,
                               "spec_entry" + std::to_string(I), M.get());
    IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
    B.CreateRet(B.getInt32(Base + I));
  }
  std::string Buf;
  raw_string_ostream OS(Buf);
  WriteBitcodeToFile(*M, OS);
  OS.flush();
  return Buf;
}

/// Calls an external function through a bitcast (so it only resolves via
/// stripPointerCasts) and reads an external global.
std::string makeBitcodeWithExternals() {
  LLVMContext Ctx;
  auto M = std::make_unique<Module>("ejit_preload_ext", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);

  auto *GV = new GlobalVariable(*M, I32, /*isConstant=*/false,
                                GlobalValue::ExternalLinkage,
                                /*Initializer=*/nullptr, "ext_global");
  // Declared with a different signature than it is called through, forcing
  // the call through a bitcast.
  auto *DeclFT = FunctionType::get(I32, {I32, I32}, false);
  auto *Helper = Function::Create(DeclFT, GlobalValue::ExternalLinkage,
                                  "ext_helper", M.get());

  auto *F = Function::Create(FunctionType::get(I32, false),
                             GlobalValue::ExternalLinkage, "spec_entry0",
                             M.get());
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
  auto *CallFT = FunctionType::get(I32, {I32}, false);
  auto *Cast = ConstantExpr::getBitCast(Helper, CallFT->getPointerTo());
  Value *G = B.CreateLoad(I32, GV);
  Value *R = B.CreateCall(CallFT, Cast, {G});
  B.CreateRet(R);

  std::string Buf;
  raw_string_ostream OS(Buf);
  WriteBitcodeToFile(*M, OS);
  OS.flush();
  return Buf;
}

using EntryFn = uint32_t (*)();

/// No active SpecializationContext is set, so the IR transform layer skips
/// the JIT optimization pipeline; codegen still runs.
class EJitPreloadTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
  }

  void SetUp() override { reset(Cfg); }

  void reset(const Config &C) {
    Eng.reset();
    auto EngOrErr = EJitOrcEngine::Create(C, Reg, State);
    ASSERT_TRUE(!!EngOrErr) << toString(EngOrErr.takeError());
    Eng = std::move(*EngOrErr);
  }

  uint32_t compileAndRun(StringRef Blob, uint64_t Key,
                         const char *Entry = "spec_entry0") {
    EXPECT_FALSE(errorToBool(Eng->loadBitcodeModule(Blob, Key, Entry)))
        << "loadBitcodeModule failed for key " << Key;
    auto AddrOrErr = Eng->lookup(Key, Entry);
    EXPECT_TRUE(!!AddrOrErr) << "lookup failed for key " << Key;
    if (!AddrOrErr) {
      consumeError(AddrOrErr.takeError());
      return 0;
    }
    return reinterpret_cast<EntryFn>(*AddrOrErr)();
  }

  uint64_t parses() const { return Eng->getBitcodeCacheStats().parses; }

  Config Cfg;
  PeriodArrayRegistry Reg;
  EJitRuntimeState State;
  std::unique_ptr<EJitOrcEngine> Eng;
};

// Two parses, not one: the first load deliberately retains nothing, so the
// template is built on the second. Everything after that is a clone.
TEST_F(EJitPreloadTest, ParseCountIsBoundedAcrossSpecializations) {
  const std::string BC = makeBitcode(111);
  const uint64_t N = 8;

  for (uint64_t Key = 1; Key <= N; ++Key)
    EXPECT_EQ(compileAndRun(BC, Key), 111u);

  EXPECT_EQ(parses(), 2u) << "blob was re-parsed per specialization";
  EXPECT_EQ(Eng->getBitcodeCacheStats().templateHits, N - 2);
}

// Parsed IR costs ~20x the bitcode; a one-shot blob must not pay that.
TEST_F(EJitPreloadTest, SingleSpecializationRetainsNothing) {
  const std::string BC = makeBitcode(111);

  EXPECT_EQ(compileAndRun(BC, 1), 111u);

  auto S = Eng->getBitcodeCacheStats();
  EXPECT_EQ(S.entries, 0u) << "a one-shot blob was retained";
  EXPECT_EQ(S.approxBytes, 0u);
  EXPECT_EQ(S.parses, 1u);
}

// The template is built on the second load, not the first.
TEST_F(EJitPreloadTest, TemplateIsRetainedOnSecondLoad) {
  const std::string BC = makeBitcode(111);

  EXPECT_EQ(compileAndRun(BC, 1), 111u);
  EXPECT_EQ(Eng->getBitcodeCacheStats().entries, 0u);

  EXPECT_EQ(compileAndRun(BC, 2), 111u);
  auto S = Eng->getBitcodeCacheStats();
  EXPECT_EQ(S.entries, 1u);
  EXPECT_GT(S.approxBytes, 0u);
  EXPECT_EQ(S.parses, 2u) << "second load should parse once more, then cache";

  // Third load is served from the template.
  EXPECT_EQ(compileAndRun(BC, 3), 111u);
  EXPECT_EQ(parses(), 2u);
}

// preLoadBitcodeUtil skips the second-load gate.
TEST_F(EJitPreloadTest, PreLoadBitcodeUtilCachesImmediately) {
  const std::string BC = makeBitcode(111);

  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(BC)));
  auto S = Eng->getBitcodeCacheStats();
  EXPECT_EQ(S.entries, 1u);
  EXPECT_EQ(S.parses, 1u);

  EXPECT_EQ(compileAndRun(BC, 1), 111u);
  EXPECT_EQ(parses(), 1u) << "preloaded blob was parsed again on first compile";
}

TEST_F(EJitPreloadTest, PreLoadBitcodeUtilIsIdempotent) {
  const std::string BC = makeBitcode(111);
  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(BC)));
  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(BC)));
  EXPECT_EQ(parses(), 1u);
  EXPECT_EQ(Eng->getBitcodeCacheStats().entries, 1u);
}

// A cached template is never handed out for different bitcode.
TEST_F(EJitPreloadTest, DistinctBlobsAreCachedIndependently) {
  const std::string A = makeBitcode(111);
  const std::string B = makeBitcode(222);
  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(A)));
  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(B)));

  EXPECT_EQ(compileAndRun(A, 1), 111u);
  EXPECT_EQ(compileAndRun(B, 2), 222u);
  EXPECT_EQ(Eng->getBitcodeCacheStats().entries, 2u);
}

// Identity is address *and* size, so a buffer that changes length is not
// mistaken for the cached entry.
TEST_F(EJitPreloadTest, SizeIsPartOfBlobIdentity) {
  const std::string One = makeBitcode(111, /*NEntries=*/1);
  const std::string Two = makeBitcode(222, /*NEntries=*/2);
  ASSERT_NE(One.size(), Two.size());

  std::vector<char> Buf(std::max(One.size(), Two.size()));
  std::copy(One.begin(), One.end(), Buf.begin());
  ASSERT_FALSE(errorToBool(
      Eng->preLoadBitcodeUtil(StringRef(Buf.data(), One.size()))));

  // Same address, different length: a fresh parse, not the cached template.
  std::copy(Two.begin(), Two.end(), Buf.begin());
  EXPECT_EQ(compileAndRun(StringRef(Buf.data(), Two.size()), 1), 222u);
  EXPECT_EQ(parses(), 2u);
}

// One blob holds every ejit_entry of a TU, which is why the entry-linkage
// fixup is applied per clone rather than baked into the template.
TEST_F(EJitPreloadTest, MultipleEntryPointsShareOneTemplate) {
  const std::string BC = makeBitcode(500, /*NEntries=*/3);
  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(BC)));

  EXPECT_EQ(compileAndRun(BC, 1, "spec_entry0"), 500u);
  EXPECT_EQ(compileAndRun(BC, 2, "spec_entry1"), 501u);
  EXPECT_EQ(compileAndRun(BC, 3, "spec_entry2"), 502u);
  EXPECT_EQ(parses(), 1u);
}

// Declarations are discovered once, but addresses must be re-resolved per
// compile: a symbol registered after the template was built still has to bind.
TEST_F(EJitPreloadTest, SymbolsRegisteredAfterCachingStillResolve) {
  static uint32_t GlobalStorage = 7;
  const std::string BC = makeBitcodeWithExternals();

  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(BC)));

  // Registered only after the template exists.
  Eng->addUserSymbol("ext_helper", reinterpret_cast<void *>(+[](uint32_t V) {
                       return V * 3;
                     }));
  Eng->addUserSymbol("ext_global", &GlobalStorage);

  EXPECT_EQ(compileAndRun(BC, 1), 21u) << "late-registered symbols did not bind";
  EXPECT_EQ(parses(), 1u);
}

// With room for one entry, caching a second blob evicts the first.
TEST_F(EJitPreloadTest, CacheStaysWithinItsByteBudget) {
  const std::string A = makeBitcode(111);
  const std::string B = makeBitcode(222);

  Config Small = Cfg;
  Small.maxPreloadCacheSize = A.size() * 20 + 16; // room for ~one entry
  ASSERT_NO_FATAL_FAILURE(reset(Small));

  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(A)));
  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(B)));

  auto S = Eng->getBitcodeCacheStats();
  EXPECT_EQ(S.entries, 1u);
  EXPECT_LE(S.approxBytes, Small.maxPreloadCacheSize);
  EXPECT_EQ(S.evictions, 1u);

  // The evicted blob still compiles, just via a fresh parse.
  EXPECT_EQ(compileAndRun(A, 1), 111u);
}

// A zero budget disables retention outright.
TEST_F(EJitPreloadTest, ZeroBudgetDisablesTheCache) {
  const std::string BC = makeBitcode(111);

  Config Off = Cfg;
  Off.maxPreloadCacheSize = 0;
  ASSERT_NO_FATAL_FAILURE(reset(Off));

  for (uint64_t Key = 1; Key <= 3; ++Key)
    EXPECT_EQ(compileAndRun(BC, Key), 111u);

  auto S = Eng->getBitcodeCacheStats();
  EXPECT_EQ(S.entries, 0u);
  EXPECT_EQ(S.approxBytes, 0u);
  EXPECT_EQ(S.parses, 3u) << "every compile should re-parse with no cache";
}

TEST_F(EJitPreloadTest, ClearBitcodeCacheReleasesEverything) {
  const std::string BC = makeBitcode(111);
  ASSERT_FALSE(errorToBool(Eng->preLoadBitcodeUtil(BC)));
  ASSERT_EQ(Eng->getBitcodeCacheStats().entries, 1u);

  Eng->clearBitcodeCache();
  auto S = Eng->getBitcodeCacheStats();
  EXPECT_EQ(S.entries, 0u);
  EXPECT_EQ(S.approxBytes, 0u);

  EXPECT_EQ(compileAndRun(BC, 1), 111u);
}

} // namespace
