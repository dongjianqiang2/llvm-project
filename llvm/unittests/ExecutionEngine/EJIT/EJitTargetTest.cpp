//===-- EJitTargetTest.cpp - JIT target tuning unit tests -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The register decode and the attribute rewrite are the two places where a
// mistake would be silent: a wrong field only changes which instructions the
// JIT may emit, and a wrong rewrite only changes codegen quality - until a core
// without the feature executes the code. Both are pinned here with synthetic
// register values, so the tests run on any host.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitTarget.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// MIDR_EL1 with the given implementer and part (variant/revision zero,
/// architecture field 0xf as real cores report it).
constexpr uint64_t midr(unsigned Implementer, unsigned Part) {
  return (uint64_t(Implementer) << 24) | (0xfULL << 16) | (uint64_t(Part) << 4);
}

constexpr uint64_t field(unsigned Lsb, unsigned Value) {
  return uint64_t(Value) << Lsb;
}

/// A Neoverse-N1-like core: every decoded feature except i8mm.
EJitCpuIdRegs n1Regs() {
  EJitCpuIdRegs R;
  R.midr = midr(0x41, 0xd0c);
  R.isar0 = field(16, 1) | field(20, 2) | field(28, 1) | field(44, 1);
  R.isar1 = field(20, 1);
  R.pfr0 = field(16, 1) | field(20, 1);
  return R;
}

TEST(EJitTargetTest, CpuNameFromMidr) {
  EXPECT_EQ(cpuNameFromMidr(midr(0x41, 0xd0c)), "neoverse-n1");
  EXPECT_EQ(cpuNameFromMidr(midr(0x48, 0xd01)), "tsv110");
  // Unknown part and unreadable register both mean "leave tuning alone".
  EXPECT_EQ(cpuNameFromMidr(midr(0x41, 0xfff)), "");
  EXPECT_EQ(cpuNameFromMidr(0), "");
}

TEST(EJitTargetTest, FeaturesFromIdRegs) {
  EXPECT_EQ(featuresFromIdRegs(n1Regs()),
            (std::vector<std::string>{"+crc", "+lse", "+rdm", "+dotprod",
                                      "+rcpc", "+fullfp16"}));
  EXPECT_TRUE(featuresFromIdRegs(EJitCpuIdRegs()).empty());

  EJitCpuIdRegs R;
  R.isar0 = field(20, 1); // Atomic 1 is the v8.0 exclusives, not LSE.
  R.isar1 = field(52, 1);
  R.pfr0 = field(16, 0xf) | field(20, 0xf); // No FP/AdvSIMD at all.
  EXPECT_EQ(featuresFromIdRegs(R), (std::vector<std::string>{"+i8mm"}));

  R = EJitCpuIdRegs();
  R.pfr0 = field(16, 1); // FP16 scalar without FP16 AdvSIMD.
  EXPECT_TRUE(featuresFromIdRegs(R).empty());
}

TEST(EJitTargetTest, ShouldReadIdRegs) {
  // Hosted: the default detects, and the OS check guards the read itself.
  EXPECT_TRUE(shouldReadIdRegs("", "", /*Hosted=*/true));
  EXPECT_TRUE(shouldReadIdRegs("detect", "", true));
  EXPECT_TRUE(shouldReadIdRegs("cortex-a55", "detect", true));
  EXPECT_FALSE(shouldReadIdRegs("cortex-a55", "", true));
  EXPECT_FALSE(shouldReadIdRegs("none", "detect", true));

  // Freestanding: the read can trap, so only an explicit "detect" CPU reads.
  EXPECT_FALSE(shouldReadIdRegs("", "", /*Hosted=*/false));
  EXPECT_FALSE(shouldReadIdRegs("", "detect", false));
  EXPECT_FALSE(shouldReadIdRegs("cortex-a55", "detect", false));
  EXPECT_FALSE(shouldReadIdRegs("none", "", false));
  EXPECT_TRUE(shouldReadIdRegs("detect", "", false));
  EXPECT_TRUE(shouldReadIdRegs("detect", "detect", false));
}

TEST(EJitTargetTest, ResolveJitTarget) {
  const EJitCpuIdRegs Regs = n1Regs();

  EJitJitTarget T = resolveJitTarget("", "", &Regs);
  EXPECT_EQ(T.tuneCpu, "neoverse-n1");
  EXPECT_TRUE(T.features.empty());

  T = resolveJitTarget("detect", "", &Regs);
  EXPECT_EQ(T.tuneCpu, "neoverse-n1");

  EXPECT_TRUE(resolveJitTarget("none", "detect", &Regs).empty());

  T = resolveJitTarget("cortex-a55", "", &Regs);
  EXPECT_EQ(T.tuneCpu, "cortex-a55");

  // Registers not read: detected parts stay empty, fixed settings still apply.
  EXPECT_TRUE(resolveJitTarget("", "detect", nullptr).empty());
  EXPECT_TRUE(resolveJitTarget("detect", "", nullptr).empty());
  T = resolveJitTarget("cortex-a55", "+lse", nullptr);
  EXPECT_EQ(T.tuneCpu, "cortex-a55");
  EXPECT_EQ(T.features, (std::vector<std::string>{"+lse"}));

  T = resolveJitTarget("", "detect", &Regs);
  EXPECT_EQ(T.features, featuresFromIdRegs(Regs));

  // Only well-formed additions are kept; a removal could drop a feature the
  // AOT code relies on.
  T = resolveJitTarget("", " +lse, -crc ,dotprod,,+", &Regs);
  EXPECT_EQ(T.features, (std::vector<std::string>{"+lse"}));

  // An unknown part tunes nothing but still passes detected features on.
  EJitCpuIdRegs Unknown = Regs;
  Unknown.midr = midr(0x41, 0xfff);
  T = resolveJitTarget("", "detect", &Unknown);
  EXPECT_EQ(T.tuneCpu, "");
  EXPECT_FALSE(T.features.empty());
}

TEST(EJitTargetTest, ReadCpuIdRegs) {
  EJitCpuIdRegs R;
  if (!readCpuIdRegs(R))
    GTEST_SKIP() << "ID registers not readable on this host";
  // Every AArch64 core reports an implementer.
  EXPECT_NE(R.midr, 0u);
}

class ApplyJitTargetTest : public testing::Test {
protected:
  LLVMContext Ctx;
  std::unique_ptr<Module> M;

  void parse(StringRef Src) {
    SMDiagnostic Err;
    M = parseAssemblyString(Src, Err, Ctx);
    ASSERT_TRUE(M) << Err.getMessage().str();
  }

  StringRef attr(StringRef Fn, StringRef Kind) {
    return M->getFunction(Fn)->getFnAttribute(Kind).getValueAsString();
  }
};

TEST_F(ApplyJitTargetTest, TuneOnlyLeavesSubtargetAlone) {
  parse(R"(
    define void @entry() #0 { ret void }
    define void @tuned() #1 { ret void }
    declare void @ext() #0
    attributes #0 = { "target-cpu"="generic" "target-features"="+neon,+v8a" }
    attributes #1 = { "target-cpu"="generic" "tune-cpu"="cortex-a76" }
  )");
  EJitJitTarget T;
  T.tuneCpu = "neoverse-n1";
  applyJitTarget(*M, T);

  EXPECT_EQ(attr("entry", "tune-cpu"), "neoverse-n1");
  EXPECT_EQ(attr("entry", "target-cpu"), "generic");
  EXPECT_EQ(attr("entry", "target-features"), "+neon,+v8a");
  // An explicit -mtune from the AOT compile wins.
  EXPECT_EQ(attr("tuned", "tune-cpu"), "cortex-a76");
  // Declarations resolve to AOT code; nothing is compiled for them.
  EXPECT_FALSE(M->getFunction("ext")->hasFnAttribute("tune-cpu"));
}

TEST_F(ApplyJitTargetTest, FeaturesAreAddedOnce) {
  parse(R"(
    define void @entry() #0 { ret void }
    define void @bare() { ret void }
    attributes #0 = { "target-features"="+neon,+crc,-lse,-outline-atomics" }
  )");
  EJitJitTarget T;
  T.features = {"+crc", "+lse", "+dotprod"};
  applyJitTarget(*M, T);

  // +crc already present, -lse is the AOT compile's explicit choice.
  EXPECT_EQ(attr("entry", "target-features"),
            "+neon,+crc,-lse,-outline-atomics,+dotprod");
  EXPECT_EQ(attr("bare", "target-features"), "+crc,+lse,+dotprod");
  EXPECT_FALSE(M->getFunction("entry")->hasFnAttribute("tune-cpu"));
}

TEST_F(ApplyJitTargetTest, EmptyTargetIsNoOp) {
  parse(R"(
    define void @entry() #0 { ret void }
    attributes #0 = { "target-cpu"="generic" "target-features"="+neon" }
  )");
  applyJitTarget(*M, EJitJitTarget());
  EXPECT_FALSE(M->getFunction("entry")->hasFnAttribute("tune-cpu"));
  EXPECT_EQ(attr("entry", "target-features"), "+neon");
}

} // namespace
