//===-- EJitRegisterPeriodTest.cpp - exact shared-object AOT inventory ----===//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"
#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <cstddef>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

extern cl::opt<bool> EnableEJitSmallTableHooks;
extern cl::opt<bool> EnableEJitGlobalCtors;

namespace {
struct InventoryRow {
  const GlobalVariable *table;
  const GlobalVariable *object;
  uint32_t kind;
  uint64_t bytes;
};

class SharedObjectInventoryTest : public testing::Test {
protected:
  LLVMContext Context;
  bool SavedHooks = false, SavedCtors = false;

  void SetUp() override {
    SavedHooks = EnableEJitSmallTableHooks;
    SavedCtors = EnableEJitGlobalCtors;
    EnableEJitSmallTableHooks = true;
    EnableEJitGlobalCtors = true;
  }
  void TearDown() override {
    EnableEJitSmallTableHooks = SavedHooks;
    EnableEJitGlobalCtors = SavedCtors;
  }

  std::unique_ptr<Module> parse(StringRef Body, bool BigEndian = false) {
    std::string Text = BigEndian
        ? "target datalayout = \"E-m:e-i64:64-i128:128-n32:64-S128\"\n"
        : "target datalayout = \"e-m:e-i64:64-i128:128-n32:64-S128\"\n";
    Text += Body.str();
    SMDiagnostic Error;
    auto M = parseAssemblyString(Text, Error, Context);
    if (!M)
      Error.print("SharedObjectInventoryTest", errs());
    return M;
  }

  void run(Module &M, bool Coordinator = false) {
    ModuleAnalysisManager AM;
    if (Coordinator)
      EJitAotModulePass().run(M, AM);
    else
      EJitRegisterPeriodPass().run(M, AM);
  }

  std::vector<InventoryRow> rows(const Module &M) {
    std::vector<InventoryRow> Result;
    for (const GlobalVariable &Table : M.globals()) {
      if (Table.getSection() != SECT_EJIT_PERIOD || !Table.hasInitializer())
        continue;
      const auto *Array = dyn_cast<ConstantArray>(Table.getInitializer());
      if (!Array)
        continue;
      for (const Use &U : Array->operands()) {
        const auto *Record = dyn_cast<ConstantStruct>(U.get());
        if (!Record || Record->getNumOperands() != 5)
          continue;
        const auto *Kind = dyn_cast<ConstantInt>(Record->getOperand(0));
        if (!Kind || (Kind->getZExtValue() != EJIT_REG_SHARED_OBJECT_RO &&
                      Kind->getZExtValue() != EJIT_REG_SHARED_OBJECT_RW))
          continue;
        const auto *Object = dyn_cast<GlobalVariable>(
            Record->getOperand(3)->stripPointerCasts());
        const auto *Bytes = dyn_cast<ConstantInt>(Record->getOperand(4));
        EXPECT_NE(Object, nullptr);
        EXPECT_NE(Bytes, nullptr);
        EXPECT_TRUE(isa<ConstantPointerNull>(Record->getOperand(2)));
        EXPECT_EQ(Record->getType()->getNumElements(), 5u);
        EXPECT_EQ(M.getDataLayout().getTypeAllocSize(Record->getType()), 40u);
        EXPECT_TRUE(Table.isConstant());
        EXPECT_TRUE(Table.hasPrivateLinkage());
        if (Object && Bytes)
          Result.push_back({&Table, Object,
                            static_cast<uint32_t>(Kind->getZExtValue()),
                            Bytes->getZExtValue()});
      }
    }
    return Result;
  }

  size_t tableCount(const Module &M) {
    size_t Count = 0;
    for (const GlobalVariable &GV : M.globals())
      Count += GV.getSection() == SECT_EJIT_PERIOD;
    return Count;
  }
};

TEST_F(SharedObjectInventoryTest, OriginalRecordLayoutAndValuesAreUnchanged) {
  EXPECT_EQ(EJIT_REG_BITCODE, 0);
  EXPECT_EQ(EJIT_REG_PERIOD_ARRAY, 1);
  EXPECT_EQ(EJIT_REG_STATIC_VAR, 2);
  EXPECT_EQ(EJIT_REG_SYMBOL, 3);
  EXPECT_EQ(EJIT_REG_NONE, 4);
  EXPECT_EQ(EJIT_REG_LIFECYCLE, 5);
  EXPECT_EQ(EJIT_REG_FUNCINDEX, 6);
  EXPECT_EQ(EJIT_REG_ICACHE_SLOT, 7);
  EXPECT_EQ(EJIT_REG_SHARED_OBJECT_RO, 8);
  EXPECT_EQ(EJIT_REG_SHARED_OBJECT_RW, 9);
  EXPECT_EQ(sizeof(ejit_reg_entry_t), 40u);
  EXPECT_EQ(offsetof(ejit_reg_entry_t, name1), 8u);
  EXPECT_EQ(offsetof(ejit_reg_entry_t, name2), 16u);
  EXPECT_EQ(offsetof(ejit_reg_entry_t, ptr), 24u);
  EXPECT_EQ(offsetof(ejit_reg_entry_t, size), 32u);
}

TEST_F(SharedObjectInventoryTest, IncludesUnreferencedControllerObjects) {
  auto M = parse(R"(
    @config = global [6 x [2 x { i32, i32, i32, i32, i32 }]] zeroinitializer,
      section ".mc_shared"
    @output = global [6 x [2 x i64]] zeroinitializer, section ".mc_shared"
    @source = private global { i64, i64, i32, i32 } zeroinitializer,
      section ".mc_shared"
    @stage = private global i32 0, section ".mc_shared"
    @arm = private global i32 0, section ".mc_shared"
    @result = private global i32 0, section ".mc_shared"
    @slot = global ptr null, section ".mc_shared"
    define void @unrelated() { ret void }
  )");
  ASSERT_NE(M, nullptr);
  run(*M);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  auto R = rows(*M);
  ASSERT_EQ(R.size(), 7u);
  const uint64_t Expected[] = {240, 96, 24, 4, 4, 4, 8};
  for (size_t I = 0; I < R.size(); ++I) {
    EXPECT_EQ(R[I].kind, EJIT_REG_SHARED_OBJECT_RW);
    EXPECT_EQ(R[I].bytes, Expected[I]);
  }
  EXPECT_NE(M->getGlobalVariable("llvm.used"), nullptr);
  EXPECT_EQ(M->getFunction(FN_AUTO_REGISTER), nullptr);
  EXPECT_EQ(M->getGlobalVariable("llvm.global_ctors"), nullptr);
}

TEST_F(SharedObjectInventoryTest, AllocationExtentUsesTargetPaddingLE) {
  auto M = parse(R"(
    @padded = global { i8, i64, i8 } zeroinitializer, section ".mc_shared.payload"
  )");
  ASSERT_NE(M, nullptr);
  run(*M);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  auto R = rows(*M);
  ASSERT_EQ(R.size(), 1u);
  EXPECT_EQ(R[0].bytes, 24u);
  EXPECT_EQ(R[0].object, M->getNamedGlobal("padded"));
}

TEST_F(SharedObjectInventoryTest, AllocationExtentUsesTargetPaddingBE) {
  auto M = parse(R"(
    @padded = global { i8, i64, i8 } zeroinitializer, section ".mc_shared.payload"
  )", true);
  ASSERT_NE(M, nullptr);
  run(*M);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  auto R = rows(*M);
  ASSERT_EQ(R.size(), 1u);
  EXPECT_EQ(R[0].bytes, 24u);
  EXPECT_EQ(R[0].object, M->getNamedGlobal("padded"));
}

TEST_F(SharedObjectInventoryTest, ConstantsHaveOnlyReadPermission) {
  auto M = parse(R"(
    @ro = private constant [4 x i32] [i32 1, i32 2, i32 3, i32 4],
      section ".mc_shared"
    @rw = private global i64 0, section ".mc_shared"
  )");
  ASSERT_NE(M, nullptr);
  run(*M);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  auto R = rows(*M);
  ASSERT_EQ(R.size(), 2u);
  EXPECT_EQ(R[0].kind, EJIT_REG_SHARED_OBJECT_RO);
  EXPECT_EQ(R[0].bytes, 16u);
  EXPECT_EQ(R[1].kind, EJIT_REG_SHARED_OBJECT_RW);
  EXPECT_EQ(R[1].bytes, 8u);
}

TEST_F(SharedObjectInventoryTest, SharedOnlyCoordinatorNeedsNoMetadataOrCtor) {
  auto M = parse(R"(
    @source_state = private global [24 x i8] zeroinitializer,
      section ".mc_shared"
  )");
  ASSERT_NE(M, nullptr);
  run(*M, true);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  ASSERT_EQ(rows(*M).size(), 1u);
  EXPECT_EQ(M->getFunction(FN_AUTO_REGISTER), nullptr);
  EXPECT_EQ(M->getFunction(FN_REGISTER_PERIOD_ARRAY), nullptr);
  EXPECT_EQ(M->getFunction(FN_REGISTER_STATIC_VAR), nullptr);
  EXPECT_EQ(M->getGlobalVariable("llvm.global_ctors"), nullptr);
}

TEST_F(SharedObjectInventoryTest, InventoryWorksWithGlobalConstructorsDisabled) {
  EnableEJitGlobalCtors = false;
  auto M = parse("@x = private global i32 0, section \".mc_shared\"\n");
  ASSERT_NE(M, nullptr);
  run(*M);
  ASSERT_EQ(rows(*M).size(), 1u);
  EXPECT_EQ(M->getGlobalVariable("llvm.global_ctors"), nullptr);
  ASSERT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(SharedObjectInventoryTest, ExistingOptInOffLeavesSharedOnlyTUUnchanged) {
  EnableEJitSmallTableHooks = false;
  auto M = parse("@x = private global i32 0, section \".mc_shared\"\n");
  ASSERT_NE(M, nullptr);
  run(*M);
  run(*M, true);
  EXPECT_TRUE(rows(*M).empty());
  EXPECT_EQ(tableCount(*M), 0u);
  EXPECT_EQ(M->getFunction(FN_AUTO_REGISTER), nullptr);
  EXPECT_EQ(M->getGlobalVariable("llvm.used"), nullptr);
  ASSERT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(SharedObjectInventoryTest, FiltersSectionSpellingAndNonRealStorage) {
  auto M = parse(R"(
    @valid = private global i32 0, section ".mc_shared"
    @suffix = private global i32 0, section ".mc_shared.config"
    @wrong_prefix = private global i32 0, section ".mc_shared_config"
    @wrong_tail = private global i32 0, section ".mc_sharedness"
    @regular = private global i32 0
    @external = external global i32, section ".mc_shared"
    @tls = thread_local global i32 0, section ".mc_shared"
    @nonzero_as = addrspace(1) global i32 0, section ".mc_shared"
    @zero = private global [0 x i8] zeroinitializer, section ".mc_shared"
    @not_emitted = available_externally global i32 0, section ".mc_shared"
    @shared_alias = alias i32, ptr @valid
    @regular_alias = alias i32, ptr @regular
  )");
  ASSERT_NE(M, nullptr);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  run(*M, true);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  auto R = rows(*M);
  ASSERT_EQ(R.size(), 2u);
  EXPECT_EQ(R[0].object, M->getNamedGlobal("valid"));
  EXPECT_EQ(R[1].object, M->getNamedGlobal("suffix"));
}

TEST_F(SharedObjectInventoryTest, ScalableOrUnsizedNegativeInjectionIsNotClaimed) {
  auto M = parse("@valid = private global i32 0, section \".mc_shared\"\n");
  ASSERT_NE(M, nullptr);
  auto *Scalable = VectorType::get(Type::getInt32Ty(Context),
                                 ElementCount::getScalable(4));
  auto *Bad = new GlobalVariable(*M, Scalable, false,
      GlobalValue::InternalLinkage, Constant::getNullValue(Scalable), "scalable");
  Bad->setSection(".mc_shared");
  auto *Opaque = StructType::create(Context, "unsized");
  auto *Unsized = new GlobalVariable(*M, Opaque, false,
      GlobalValue::ExternalLinkage, nullptr, "unsized");
  Unsized->setSection(".mc_shared");
  // These deliberately invalid/non-emitted storage shapes are injected into
  // IR only to prove no allocation-size assertion or inventory claim occurs.
  // They are not presented as valid codegen inputs.
  run(*M);
  auto R = rows(*M);
  ASSERT_EQ(R.size(), 1u);
  EXPECT_EQ(R[0].object, M->getNamedGlobal("valid"));
}

TEST_F(SharedObjectInventoryTest, RepeatedPassAndNewObjectDoNotDuplicateInventory) {
  auto M = parse("@first = private global i32 0, section \".mc_shared\"\n");
  ASSERT_NE(M, nullptr);
  run(*M, true);
  ASSERT_EQ(rows(*M).size(), 1u);
  EXPECT_EQ(tableCount(*M), 1u);
  run(*M);
  run(*M, true);
  EXPECT_EQ(rows(*M).size(), 1u);
  EXPECT_EQ(tableCount(*M), 1u);
  auto *Added = new GlobalVariable(*M, Type::getInt64Ty(Context), false,
      GlobalValue::PrivateLinkage,
      ConstantInt::get(Type::getInt64Ty(Context), 0), "second");
  Added->setSection(".mc_shared.added");
  run(*M);
  EXPECT_EQ(rows(*M).size(), 2u);
  EXPECT_EQ(tableCount(*M), 2u);
  run(*M, true);
  EXPECT_EQ(rows(*M).size(), 2u);
  EXPECT_EQ(tableCount(*M), 2u);
  ASSERT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(SharedObjectInventoryTest, TwoTUsKeepPrivateRecordsAndDistinctRealObjects) {
  auto A = parse("@local = private global i32 0, section \".mc_shared\"\n");
  auto B = parse("@local = private global i64 0, section \".mc_shared\"\n");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  A->setModuleIdentifier("inventory-a.c");
  B->setModuleIdentifier("inventory-b.c");
  run(*A, true);
  run(*B, true);
  ASSERT_EQ(rows(*A).size(), 1u);
  ASSERT_EQ(rows(*B).size(), 1u);
  ASSERT_FALSE(Linker::linkModules(*A, std::move(B)));
  ASSERT_FALSE(verifyModule(*A, &errs()));
  auto R = rows(*A);
  ASSERT_EQ(R.size(), 2u);
  EXPECT_NE(R[0].table, R[1].table);
  EXPECT_NE(R[0].object, R[1].object);
  EXPECT_EQ(R[0].bytes + R[1].bytes, 12u);
  EXPECT_EQ(tableCount(*A), 2u);
  run(*A, true);
  EXPECT_EQ(rows(*A).size(), 2u);
  EXPECT_EQ(tableCount(*A), 2u);
  ASSERT_FALSE(verifyModule(*A, &errs()));
}

TEST_F(SharedObjectInventoryTest,
       TwoTUsCoalesceExactWeakLinkonceAndCommonObjects) {
  for (StringRef Linkage : {"weak", "linkonce_odr", "common"}) {
    SCOPED_TRACE(Linkage.str());
    std::string Source = "@coalesced = " + Linkage.str() +
        " global i32 0, section \".mc_shared\"\n";
    auto A = parse(Source);
    auto B = parse(Source);
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);
    A->setModuleIdentifier("coalesced-a.c");
    B->setModuleIdentifier("coalesced-b.c");
    run(*A, true);
    run(*B, true);
    ASSERT_EQ(rows(*A).size(), 1u);
    ASSERT_EQ(rows(*B).size(), 1u);
    ASSERT_FALSE(Linker::linkModules(*A, std::move(B)));
    ASSERT_FALSE(verifyModule(*A, &errs()));
    auto R = rows(*A);
    ASSERT_EQ(R.size(), 2u);
    EXPECT_NE(R[0].table, R[1].table);
    EXPECT_EQ(R[0].object, R[1].object);
    EXPECT_EQ(R[0].object, A->getNamedGlobal("coalesced"));
    EXPECT_EQ(R[0].bytes, 4u);
    EXPECT_EQ(R[1].bytes, 4u);
    EXPECT_EQ(R[0].kind, EJIT_REG_SHARED_OBJECT_RW);
    EXPECT_EQ(R[1].kind, EJIT_REG_SHARED_OBJECT_RW);
    // Duplicate rows describe one real coalesced object, not two disjoint
    // allocations. The runtime accepts only exact same-base/extent/access
    // duplicates; conflicting records and partial overlap remain refused.
    run(*A, true);
    EXPECT_EQ(rows(*A).size(), 2u);
    EXPECT_EQ(tableCount(*A), 2u);
    ASSERT_FALSE(verifyModule(*A, &errs()));
  }
}

TEST_F(SharedObjectInventoryTest, OriginalPeriodCountsAndConstructorStayIntact) {
  auto M = parse(R"(
    @source = global [6 x [2 x i32]] zeroinitializer, section ".mc_shared",
      !ejit.metadata !0
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 6}
  )");
  ASSERT_NE(M, nullptr);
  run(*M);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  auto *Table = M->getGlobalVariable(".ejit.registry.period", true);
  ASSERT_NE(Table, nullptr);
  auto *First = dyn_cast<ConstantStruct>(
      Table->getInitializer()->getAggregateElement(0u));
  ASSERT_NE(First, nullptr);
  EXPECT_EQ(cast<ConstantInt>(First->getOperand(0))->getZExtValue(),
            EJIT_REG_PERIOD_ARRAY);
  EXPECT_EQ(cast<ConstantInt>(First->getOperand(4))->getZExtValue(), 6u);
  auto R = rows(*M);
  ASSERT_EQ(R.size(), 1u);
  EXPECT_EQ(R[0].bytes, 48u);
  EXPECT_EQ(R[0].kind, EJIT_REG_SHARED_OBJECT_RW);
  EXPECT_NE(M->getFunction(FN_AUTO_REGISTER), nullptr);
  EXPECT_NE(M->getFunction(FN_REGISTER_PERIOD_ARRAY), nullptr);
  EXPECT_NE(M->getGlobalVariable("llvm.global_ctors"), nullptr);
}

TEST_F(SharedObjectInventoryTest, OptInOffStillEmitsOriginalPeriodRecords) {
  EnableEJitSmallTableHooks = false;
  auto M = parse(R"(
    @source = global [6 x i32] zeroinitializer, section ".mc_shared",
      !ejit.metadata !0
    !0 = !{!1}
    !1 = !{!"ejit_period_arr", !"cell", i32 6}
  )");
  ASSERT_NE(M, nullptr);
  run(*M);
  ASSERT_FALSE(verifyModule(*M, &errs()));
  EXPECT_TRUE(rows(*M).empty());
  EXPECT_EQ(tableCount(*M), 1u);
  EXPECT_NE(M->getFunction(FN_AUTO_REGISTER), nullptr);
}
} // namespace
