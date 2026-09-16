//===-- EJitPreparedCodeTest.cpp - Exact identity and real ORC emission
//-----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitPreparedCode.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectTransformLayer.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Threading.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {
EJitCodeIdentityScope scope(StringRef Entry = "f") {
  EJitCodeIdentityScope S;
  S.source = SHA256::hash(arrayRefFromStringRef("original-bitcode-revision"));
  S.entry = Entry.str();
  S.compilerPolicy = "test/preserved/final-t2/default-host-target";
  S.bindingGeneration = 1;
  return S;
}

orc::ThreadSafeModule parse(StringRef IR) {
  auto C = std::make_unique<LLVMContext>();
  SMDiagnostic Diag;
  auto M = parseAssemblyString(IR, Diag, *C);
  EXPECT_TRUE(M != nullptr) << Diag.getMessage().str();
  if (!M)
    return {};
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  M->setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:"
                   "128-n8:16:32:64-S128");
  return orc::ThreadSafeModule(std::move(M), std::move(C));
}

std::unique_ptr<EJitPreparedCode>
prepare(StringRef IR, EJitCodeIdentityScope Scope = scope(),
        ArrayRef<EJitCodeBinding> Bindings = {}) {
  auto P = EJitPreparedCode::create(parse(IR), std::move(Scope), Bindings);
  EXPECT_TRUE(static_cast<bool>(P));
  if (!P) {
    ADD_FAILURE() << toString(P.takeError());
    return nullptr;
  }
  return std::move(*P);
}

const char *Simple = "define i32 @f(i32 %x) { %y = add i32 %x, 7 ret i32 %y }";

static std::unique_ptr<Module> makeCandidateCellPgoModule(LLVMContext &Ctx) {
  auto M = std::make_unique<Module>("candidate_cell_pgo", Ctx);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));
  M->setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");

  auto *I32 = Type::getInt32Ty(Ctx);
  auto *CellTy = StructType::create(Ctx, "struct.CandidateCell");
  CellTy->setBody({I32, I32});
  auto *CellsTy = ArrayType::get(CellTy, 10);
  auto *Cells =
      new GlobalVariable(*M, CellsTy, false, GlobalValue::InternalLinkage,
                         ConstantAggregateZero::get(CellsTy),
                         "g_candidate_cells");
  Metadata *PeriodOps[] = {MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
                           MDString::get(Ctx, "cell"),
                           ConstantAsMetadata::get(ConstantInt::get(I32, 10))};
  Metadata *FieldOps[] = {MDString::get(Ctx, TAG_EJIT_MAY_CONST_FIELD),
                          ConstantAsMetadata::get(ConstantInt::get(I32, 0))};
  Cells->setMetadata(MD_EJIT_METADATA,
                     MDNode::get(Ctx, {MDNode::get(Ctx, PeriodOps),
                                       MDNode::get(Ctx, FieldOps)}));

  auto *SinkTy = FunctionType::get(Type::getVoidTy(Ctx), {I32, I32}, false);
  FunctionCallee Even = M->getOrInsertFunction("candidate_sink_even", SinkTy);
  FunctionCallee Odd = M->getOrInsertFunction("candidate_sink_odd", SinkTy);
  auto *F =
      Function::Create(FunctionType::get(I32, {I32, I32, I32}, false),
                       Function::ExternalLinkage, "candidate_cell_pgo", M.get());
  Argument *Cell = F->getArg(0);
  Argument *Slot = F->getArg(1);
  Argument *Live = F->getArg(2);
  Cell->setName("cell");
  Slot->setName("slot");
  Live->setName("live");

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *EvenBB = BasicBlock::Create(Ctx, "even", F);
  BasicBlock *OddBB = BasicBlock::Create(Ctx, "odd", F);
  BasicBlock *Exit = BasicBlock::Create(Ctx, "exit", F);
  IRBuilder<> B(Entry);
  Value *Elem =
      B.CreateInBoundsGEP(CellsTy, Cells, {B.getInt32(0), Cell}, "element");
  Value *FrozenPtr = B.CreateStructGEP(CellTy, Elem, 0, "frozen.ptr");
  Value *DynamicPtr = B.CreateStructGEP(CellTy, Elem, 1, "dynamic.ptr");
  auto *Frozen = B.CreateLoad(I32, FrozenPtr, "frozen");
  Frozen->setMetadata(MD_EJIT_MAY_CONST, MDNode::get(Ctx, {}));
  Value *Dynamic = B.CreateLoad(I32, DynamicPtr, "dynamic");
  B.CreateStore(Live, DynamicPtr);
  Value *IsOdd =
      B.CreateICmpNE(B.CreateAnd(Dynamic, B.getInt32(1)), B.getInt32(0));
  B.CreateCondBr(IsOdd, OddBB, EvenBB);
  B.SetInsertPoint(EvenBB);
  B.CreateCall(Even, {Slot, Live});
  B.CreateBr(Exit);
  B.SetInsertPoint(OddBB);
  B.CreateCall(Odd, {Slot, Live});
  B.CreateBr(Exit);
  B.SetInsertPoint(Exit);
  B.CreateRet(B.CreateAdd(Frozen, Dynamic));

  Metadata *DimOps[] = {MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
                        MDString::get(Ctx, "cell"),
                        ConstantAsMetadata::get(ConstantInt::get(I32, 0))};
  F->setMetadata(
      MD_EJIT_METADATA,
      MDNode::getDistinct(
          Ctx, {MDNode::get(Ctx, {MDString::get(Ctx, TAG_EJIT_ENTRY)}),
                MDNode::get(Ctx, DimOps)}));
  return M;
}

uint64_t collideCandidateHash(ArrayRef<uint8_t>) { return 1; }

EJitCandidateResult captureCandidate(EJitCandidateDirectory &Directory,
                                     StringRef IR,
                                     ArrayRef<EJitCodeBinding> Bindings) {
  auto TSM = parse(IR);
  EJitCandidateCapture Capture(Directory, scope(), Bindings);
  TSM.withModuleDo([&](Module &M) {
    Function *Entry = M.getFunction("f");
    ASSERT_NE(Entry, nullptr);
    LLVMContext &C = M.getContext();
    MDNode *Tag = MDNode::get(C, {MDString::get(C, TAG_EJIT_ENTRY)});
    Entry->setMetadata(MD_EJIT_METADATA, MDNode::get(C, {Tag}));
    PeriodArrayRegistry Registry;
    EJitOptimizer Optimizer(Registry, /*PreserveDimensions=*/true);
    SpecializationContext Ctx;
    Ctx.fnName = "f";
    Ctx.tier = CompileTier::Instrumented;
    Ctx.candidateCapture = &Capture;
    Optimizer.runPipeline(M, Ctx);
    EXPECT_FALSE(Optimizer.getLastCounterNames().empty());
  });
  EXPECT_TRUE(Capture.prefixCaptured());
  EXPECT_TRUE(Capture.completed());
  auto R = Capture.takeResult();
  EXPECT_TRUE(static_cast<bool>(R));
  return R ? *R : EJitCandidateResult{};
}

TEST(EJitCandidateDirectory, RealOptimizerPrefixGroupsExactly) {
  const char *SameA = "define i32 @f(i32 %x) { %r = add i32 %x, 7 ret i32 %r }";
  const char *SameB =
      "source_filename=\"cell2.c\" define i32 @f(i32 %cell) { entry: "
      "%named = add i32 %cell, 7 ret i32 %named }";
  const char *Different =
      "define i32 @f(i32 %x) { %r = add i32 %x, 9 ret i32 %r }";
  EJitCandidateDirectory Directory({}, collideCandidateHash);
  auto A = captureCandidate(Directory, SameA, {});
  auto B = captureCandidate(Directory, SameB, {});
  auto C = captureCandidate(Directory, Different, {});
  EXPECT_EQ(A.groupId, B.groupId);
  EXPECT_TRUE(B.existing);
  EXPECT_NE(A.groupId, C.groupId);
  EXPECT_FALSE(C.existing);
}

TEST(EJitCandidateDirectory, BindingsSchemaCollisionAndBudgetsAreExact) {
  auto TSM = parse("declare i32 @ext() define i32 @f() { %r=call i32 @ext() "
                   "ret i32 %r }");
  PgoFunctionSchema S{"f", 1, 2, 1, 0, 0, 0};
  EJitCandidateLimits Limits;
  Limits.maxGroups = 3;
  EJitCandidateDirectory D(Limits, collideCandidateHash);
  EJitCandidateResult A, B;
  TSM.withModuleDo([&](Module &M) {
    A = cantFail(D.classify(M, scope(), {{"ext", 4096, true}}, {S}));
    B = cantFail(D.classify(M, scope(), {{"ext", 8192, true}}, {S}));
    auto S2 = S;
    ++S2.funcHash;
    auto C = cantFail(D.classify(M, scope(), {{"ext", 4096, true}}, {S2}));
    EXPECT_NE(A.groupId, C.groupId);
    auto Other = parse("declare i32 @ext() define i32 @f() { "
                       "%r=call i32 @ext() %x=add i32 %r, 1 ret i32 %x }");
    Other.withModuleDo([&](Module &OtherM) {
      auto Full = D.classify(OtherM, scope(), {{"ext", 4096, true}}, {S2});
      ASSERT_FALSE(static_cast<bool>(Full));
      EXPECT_EQ(errorToErrorCode(Full.takeError()),
                std::make_error_code(std::errc::no_buffer_space));
    });
  });
  EXPECT_NE(A.groupId, B.groupId);
  EXPECT_EQ(D.groupCount(), 3u);

  TSM.withModuleDo([&](Module &M) {
    EJitCandidateDirectory Validate;
    auto MissingBinding = Validate.classify(M, scope(), {}, {S});
    ASSERT_FALSE(static_cast<bool>(MissingBinding));
    EXPECT_EQ(errorToErrorCode(MissingBinding.takeError()),
              std::make_error_code(std::errc::invalid_argument));
    auto WrongKind =
        Validate.classify(M, scope(), {{"ext", 4096, false}}, {S});
    ASSERT_FALSE(static_cast<bool>(WrongKind));
    EXPECT_EQ(errorToErrorCode(WrongKind.takeError()),
              std::make_error_code(std::errc::invalid_argument));
    auto MissingEntry = Validate.classify(
        M, scope("missing"), {{"ext", 4096, true}}, {S});
    ASSERT_FALSE(static_cast<bool>(MissingEntry));
    EXPECT_EQ(errorToErrorCode(MissingEntry.takeError()),
              std::make_error_code(std::errc::invalid_argument));
    EXPECT_EQ(Validate.groupCount(), 0u);
  });

  EJitCandidateLimits Tiny;
  Tiny.maxIdentityBytes = 1;
  EJitCandidateDirectory ByteBounded(Tiny);
  TSM.withModuleDo([&](Module &M) {
    auto Full = ByteBounded.classify(M, scope(), {{"ext", 4096, true}}, {S});
    ASSERT_FALSE(static_cast<bool>(Full));
    EXPECT_EQ(errorToErrorCode(Full.takeError()),
              std::make_error_code(std::errc::no_buffer_space));
  });
  EXPECT_EQ(ByteBounded.groupCount(), 0u);
  EXPECT_EQ(ByteBounded.identityBytes(), 0u);
}

TEST(EJitCandidateDirectory, RealPreservedCellPrefixRetainsDynamicSemantics) {
  struct CellData {
    int32_t frozen;
    int32_t dynamic;
  } Data[10];
  for (unsigned I = 0; I != 10; ++I)
    Data[I] = {100, static_cast<int32_t>(I)};
  Data[1].frozen = 200;
  Data[2].frozen = 200;
  Data[3].frozen = 300;
  std::vector<EJitCodeBinding> Bindings = {{"candidate_sink_even", 4096, true},
                                           {"candidate_sink_odd", 8192, true}};
  EJitCandidateDirectory D({}, collideCandidateHash);
  auto Run = [&](unsigned Cell) {
    LLVMContext C;
    auto M = makeCandidateCellPgoModule(C);
    PeriodArrayRegistry R;
    R.registerArray("cell", "g_candidate_cells", Data, 10);
    EJitOptimizer O(R, true);
    EJitCandidateCapture Capture(D, scope("candidate_cell_pgo"), Bindings);
    SpecializationContext X;
    X.fnName = "candidate_cell_pgo";
    X.dimensions.push_back({"cell", static_cast<uint8_t>(Cell)});
    X.tier = CompileTier::Instrumented;
    X.candidateCapture = &Capture;
    O.runPipeline(*M, X);
    bool HasLoad = false, HasStore = false, HasSinkCall = false;
    for (Function &F : *M)
      for (BasicBlock &BB : F)
        for (Instruction &I : BB) {
          HasLoad |= isa<LoadInst>(I);
          HasStore |= isa<StoreInst>(I);
          if (auto *CB = dyn_cast<CallBase>(&I))
            if (Function *Callee = CB->getCalledFunction())
              HasSinkCall |= Callee->getName().starts_with("candidate_sink_");
        }
    EXPECT_TRUE(HasLoad);
    EXPECT_TRUE(HasStore);
    EXPECT_TRUE(HasSinkCall);
    return cantFail(Capture.takeResult());
  };
  auto A = Run(1);
  auto B = Run(2);
  auto C = Run(3);
  EXPECT_EQ(A.groupId, B.groupId);
  EXPECT_NE(A.groupId, C.groupId);
}

TEST(EJitFinalCodeIdentity, NormalizesOnlyDisplayNamesAndModulePaths) {
  auto A = prepare(Simple);
  auto B = prepare(R"(
    source_filename = "/another/path/cell5.c"
    define i32 @f(i32 %real_cell) {
    entry:
      %long_display_name = add i32 %real_cell, 7
      ret i32 %long_display_name
    })");
  ASSERT_TRUE(A && B);
  EXPECT_TRUE(A->identity().equals(B->identity()));
}

TEST(EJitFinalCodeIdentity, RetainsConstantsHelperBodiesAndSemanticMetadata) {
  const char *Pairs[][2] = {
      {Simple, "define i32 @f(i32 %x) { %y = add i32 %x, 9 ret i32 %y }"},
      {"define float @f() { ret float 0.0 }",
       "define float @f() { ret float -0.0 }"},
      {"define i32 @f() { %r = call i32 @h() ret i32 %r } define internal i32 "
       "@h() { ret i32 1 }",
       "define i32 @f() { %r = call i32 @h() ret i32 %r } define internal i32 "
       "@h() { ret i32 2 }"},
      {"define i32 @f(ptr %p) { %r = load i32, ptr %p, !range !0 ret i32 %r } "
       "!0 = !{i32 0, i32 10}",
       "define i32 @f(ptr %p) { %r = load i32, ptr %p, !range !0 ret i32 %r } "
       "!0 = !{i32 0, i32 20}"},
      {"define i32 @f(i32 %x) nounwind { ret i32 %x }",
       "define i32 @f(i32 %x) { ret i32 %x }"},
      {"define i32 @f(i32 %x) !prof !0 { ret i32 %x } !0 = "
       "!{!\"function_entry_count\", i64 64}",
       "define i32 @f(i32 %x) !prof !0 { ret i32 %x } !0 = "
       "!{!\"function_entry_count\", i64 65}"},
      {"@v = private unnamed_addr constant i32 7 define i32 @f() { %r = load "
       "i32, ptr @v ret i32 %r }",
       "@v = private unnamed_addr constant i32 9 define i32 @f() { %r = load "
       "i32, ptr @v ret i32 %r }"}};
  for (const auto &Pair : Pairs) {
    auto A = prepare(Pair[0]);
    auto B = prepare(Pair[1]);
    ASSERT_TRUE(A && B);
    EXPECT_FALSE(A->identity().equals(B->identity()));
  }
}

TEST(EJitFinalCodeIdentity, BindingOrderIsNormalizedButAddressesAreNot) {
  const char *IR = R"(@v = external global i32
    declare i32 @callee(i32)
    define i32 @f() {
      %v = load i32, ptr @v
      %r = call i32 @callee(i32 %v)
      ret i32 %r
    })";
  std::vector<EJitCodeBinding> Bindings = {{"v", 4096, false},
                                           {"callee", 8192, true}};
  auto A = prepare(IR, scope(), Bindings);
  std::swap(Bindings[0], Bindings[1]);
  auto B = prepare(IR, scope(), Bindings);
  Bindings[1].address += 64;
  auto C = prepare(IR, scope(), Bindings);
  ASSERT_TRUE(A && B && C);
  EXPECT_TRUE(A->identity().equals(B->identity()));
  EXPECT_FALSE(A->identity().equals(C->identity()));
}

TEST(EJitFinalCodeIdentity, SourcePolicyGenerationAndTargetRemainInIdentity) {
  auto Original = prepare(Simple);
  ASSERT_TRUE(Original);
  for (unsigned I = 0; I < 3; ++I) {
    auto S = scope();
    if (I == 0)
      S.source[0] ^= 1;
    if (I == 1)
      S.compilerPolicy += "/other-relocation-model";
    if (I == 2)
      ++S.bindingGeneration;
    auto Other = prepare(Simple, S);
    ASSERT_TRUE(Other);
    EXPECT_FALSE(Original->identity().equals(Other->identity()));
  }
  auto TSM = parse(Simple);
  TSM.withModuleDo([](Module &M) {
    M.setTargetTriple(Triple("aarch64_be-none-elf"));
    M.setDataLayout("E-m:e-i64:64-i128:128-n32:64-S128");
  });
  auto Big = EJitPreparedCode::create(std::move(TSM), scope(), {});
  ASSERT_TRUE(static_cast<bool>(Big));
  EXPECT_FALSE(Original->identity().equals((*Big)->identity()));
}

TEST(EJitFinalCodeIdentity, RejectsIndependentOrAddressObservableState) {
  const char *IRs[] = {
      "@counter = internal global i64 0 define i32 @f() { ret i32 1 }",
      "@x = internal constant i32 7 define ptr @f() { ret ptr @x }",
      "@x = internal thread_local global i32 0 define i32 @f() { ret i32 1 }",
      "@table = private unnamed_addr constant [1 x ptr] [ptr @h] define "
      "internal void @h() { ret void } define ptr @f() { ret ptr @table }",
      "@a = alias i32 (), ptr @f define i32 @f() { ret i32 1 }",
      "module asm \"nop\" define i32 @f() { ret i32 1 }",
      "define void @f() { call void asm sideeffect \"\", \"\"() ret void }",
      "@pc = private unnamed_addr constant ptr blockaddress(@f, %b) define "
      "void @f() { br label %b b: ret void }"};
  for (const char *IR : IRs) {
    auto P = EJitPreparedCode::create(parse(IR), scope(), {});
    EXPECT_FALSE(static_cast<bool>(P)) << IR;
    if (!P)
      consumeError(P.takeError());
  }
}

TEST(EJitFinalCodeIdentity,
     RejectsMissingDuplicateNullBindingsAndSmallBudgets) {
  const char *IR = "@v = external global i32 define i32 @f() { %v = load i32, "
                   "ptr @v ret i32 %v }";
  const std::vector<EJitCodeBinding> Cases[] = {
      {},
      {{"v", 0, false}},
      {{"v", 4096, false}, {"v", 8192, false}},
      {{"v", 4096, true}}};
  for (const auto &Bindings : Cases) {
    auto P = EJitPreparedCode::create(parse(IR), scope(), Bindings);
    EXPECT_FALSE(static_cast<bool>(P));
    if (!P)
      consumeError(P.takeError());
  }
  for (unsigned I = 0; I < 3; ++I) {
    EJitPreparedCodeLimits L;
    if (I == 0)
      L.maxModuleBytes = 8;
    if (I == 1)
      L.maxIdentityBytes = 8;
    if (I == 2)
      L.maxIRNodes = 1;
    auto P = EJitPreparedCode::create(parse(Simple), scope(), {}, L);
    EXPECT_FALSE(static_cast<bool>(P));
    if (!P)
      consumeError(P.takeError());
  }
}

TEST(EJitFinalCodeIdentity, TinyIRCannotHideHugeEmittedData) {
  auto P =
      EJitPreparedCode::create(parse("@huge = private unnamed_addr constant "
                                     "[1073741824 x i8] zeroinitializer "
                                     "define ptr @f() { ret ptr @huge }"),
                               scope(), {});
  ASSERT_FALSE(static_cast<bool>(P));
  EXPECT_NE(toString(P.takeError()).find("defined data budget"),
            std::string::npos);
}

TEST(EJitFinalCodeIdentity, BindingCannotShadowAnEmittedDefinition) {
  auto P =
      EJitPreparedCode::create(parse(Simple), scope(), {{"f", 4096, true}});
  ASSERT_FALSE(static_cast<bool>(P));
  consumeError(P.takeError());
}

TEST(EJitFinalCodeIdentity, ResourceExhaustionIsNotIndependentCodeFallback) {
  auto Private = EJitPreparedCode::create(
      parse("@x = internal global i32 0 define i32 @f() { ret i32 1 }"),
      scope(), {});
  ASSERT_FALSE(static_cast<bool>(Private));
  EXPECT_EQ(errorToErrorCode(Private.takeError()),
            std::make_error_code(std::errc::operation_not_supported));
  EJitPreparedCodeLimits Limits;
  Limits.maxModuleBytes = 8;
  auto Budget = EJitPreparedCode::create(parse(Simple), scope(), {}, Limits);
  ASSERT_FALSE(static_cast<bool>(Budget));
  EXPECT_EQ(errorToErrorCode(Budget.takeError()),
            std::make_error_code(std::errc::no_buffer_space));
}

#ifndef EJIT_FREESTANDING
class PreparedCodeNative : public testing::Test {
protected:
  std::unique_ptr<orc::LLJIT> J;
  unsigned Objects = 0;
  unsigned UnexpectedTransforms = 0;
  std::vector<std::string> Errors;

  void SetUp() override {
    static once_flag Initialized;
    llvm::call_once(Initialized, [] {
      cantFail(InitializeNativeTarget() ? createStringError("native target")
                                        : Error::success());
      cantFail(InitializeNativeTargetAsmPrinter()
                   ? createStringError("native asm printer")
                   : Error::success());
    });
    J = cantFail(orc::LLJITBuilder().create());
    J->getExecutionSession().setErrorReporter(
        [&](Error E) { Errors.push_back(toString(std::move(E))); });
    J->getObjTransformLayer().setTransform(
        [&](std::unique_ptr<MemoryBuffer> Object)
            -> Expected<std::unique_ptr<MemoryBuffer>> {
          ++Objects;
          return std::move(Object);
        });
    J->getIRTransformLayer().setTransform(
        [&](orc::ThreadSafeModule M, orc::MaterializationResponsibility &)
            -> Expected<orc::ThreadSafeModule> {
          ++UnexpectedTransforms;
          return std::move(M);
        });
  }

  void TearDown() override { J.reset(); }

  orc::ThreadSafeModule module(StringRef IR) {
    auto M = parse(IR);
    M.withModuleDo([&](Module &M) {
      M.setDataLayout(J->getDataLayout());
      M.setTargetTriple(J->getTargetTriple());
    });
    return M;
  }
  std::unique_ptr<EJitPreparedCode>
  prepared(StringRef IR, ArrayRef<EJitCodeBinding> B = {},
           EJitCodeIdentityScope S = scope()) {
    return cantFail(EJitPreparedCode::create(module(IR), S, B));
  }
};

TEST_F(PreparedCodeNative, ExactHitSkipsClaimsTransformsAndCodegen) {
  EJitPreparedCodeEmitter E(*J, 1);
  auto A = cantFail(E.link(prepared(Simple)));
  auto B = cantFail(E.link(prepared(Simple)));
  EXPECT_EQ(A.codeId, B.codeId);
  EXPECT_EQ(A.fn, B.fn);
  EXPECT_FALSE(A.reused);
  EXPECT_TRUE(B.reused);
  EXPECT_EQ(Objects, 1u);
  EXPECT_EQ(UnexpectedTransforms, 0u);
  EXPECT_EQ(reinterpret_cast<int (*)(int)>(B.fn)(35), 42);
}

TEST_F(PreparedCodeNative, ForcedHashCollisionStillComparesFullIdentity) {
  EJitPreparedCodeEmitter E(*J, 1, {},
                            [](ArrayRef<uint8_t>) { return uint64_t(0); });
  auto A = cantFail(E.link(prepared(Simple)));
  auto B = cantFail(E.link(
      prepared("define i32 @f(i32 %x) { %r = add i32 %x, 9 ret i32 %r }")));
  EXPECT_NE(A.codeId, B.codeId);
  EXPECT_NE(A.fn, B.fn);
  EXPECT_EQ(Objects, 2u);
  EXPECT_EQ(reinterpret_cast<int (*)(int)>(A.fn)(1), 8);
  EXPECT_EQ(reinterpret_cast<int (*)(int)>(B.fn)(1), 10);
}

TEST_F(PreparedCodeNative, DifferentBindingsGenerateIndependentCode) {
  int A = 7, B = 9;
  const char *IR = "@v = external global i32 define i32 @f() { %v = load i32, "
                   "ptr @v ret i32 %v }";
  EJitPreparedCodeEmitter E(*J, 1);
  auto First = cantFail(
      E.link(prepared(IR, {{"v", reinterpret_cast<uintptr_t>(&A), false}})));
  auto Second = cantFail(
      E.link(prepared(IR, {{"v", reinterpret_cast<uintptr_t>(&B), false}})));
  EXPECT_NE(First.codeId, Second.codeId);
  EXPECT_EQ(reinterpret_cast<int (*)()>(First.fn)(), 7);
  EXPECT_EQ(reinterpret_cast<int (*)()>(Second.fn)(), 9);
}

TEST_F(PreparedCodeNative, CapacityFailureKeepsExistingCodeAndAcceptsHits) {
  EJitPreparedCodeLimits L;
  L.maxCodeObjects = 1;
  EJitPreparedCodeEmitter E(*J, 1, L);
  auto A = cantFail(E.link(prepared(Simple)));
  auto Rejected = E.link(prepared("define i32 @f(i32 %x) { ret i32 9 }"));
  ASSERT_FALSE(static_cast<bool>(Rejected));
  consumeError(Rejected.takeError());
  auto B = cantFail(E.link(prepared(Simple)));
  EXPECT_EQ(A.codeId, B.codeId);
  EXPECT_EQ(E.stats().capacityRejected, 1u);
  EXPECT_EQ(Objects, 1u);
  EXPECT_EQ(reinterpret_cast<int (*)(int)>(A.fn)(2), 9);
}

TEST_F(PreparedCodeNative, TargetMismatchDoesNotCreateCode) {
  auto Other = module(Simple);
  Other.withModuleDo([](Module &M) {
    M.setTargetTriple(Triple("aarch64_be-none-elf"));
    M.setDataLayout("E-m:e-i64:64-i128:128-n32:64-S128");
  });
  auto P = cantFail(EJitPreparedCode::create(std::move(Other), scope(), {}));
  EJitPreparedCodeEmitter E(*J, 1);
  auto Rejected = E.link(std::move(P));
  ASSERT_FALSE(static_cast<bool>(Rejected));
  consumeError(Rejected.takeError());
  EXPECT_EQ(Objects, 0u);
}

TEST_F(PreparedCodeNative, LinkFailureDoesNotPoisonIdentityOrConsumeBudget) {
  // This dynamic memcpy becomes a backend libcall. Its binding is deliberately
  // omitted; a bare specialization JD must not find it through the process.
  const char *IR = R"(
    declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
    define void @f(ptr %d, ptr %s, i64 %n) {
      call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
      ret void
    })";
  EJitPreparedCodeLimits Limits;
  Limits.maxCodeObjects = 1;
  EJitPreparedCodeEmitter E(*J, 1, Limits);
  auto Failed = E.link(prepared(IR));
  ASSERT_FALSE(static_cast<bool>(Failed));
  consumeError(Failed.takeError());
  EXPECT_EQ(E.stats().codeObjects, 0u);
  EXPECT_EQ(E.stats().identityBytes, 0u);
  auto Good = cantFail(E.link(prepared(Simple)));
  EXPECT_FALSE(Good.reused);
  EXPECT_EQ(reinterpret_cast<int (*)(int)>(Good.fn)(1), 8);
}

TEST_F(PreparedCodeNative, FailedObjectCanRetryTheSameIdentity) {
  J->getObjTransformLayer().setTransform(
      [&](std::unique_ptr<MemoryBuffer> Object)
          -> Expected<std::unique_ptr<MemoryBuffer>> {
        if (++Objects == 1)
          return createStringError("injected post-CodeGen failure");
        return std::move(Object);
      });
  EJitPreparedCodeLimits Limits;
  Limits.maxCodeObjects = 1;
  EJitPreparedCodeEmitter E(*J, 1, Limits);
  auto Failed = E.link(prepared(Simple));
  ASSERT_FALSE(static_cast<bool>(Failed));
  consumeError(Failed.takeError());
  auto Retried = cantFail(E.link(prepared(Simple)));
  EXPECT_FALSE(Retried.reused);
  EXPECT_EQ(reinterpret_cast<int (*)(int)>(Retried.fn)(1), 8);
  for (unsigned I = 0; I < 300; ++I) {
    auto Reused = cantFail(E.link(prepared(Simple)));
    EXPECT_TRUE(Reused.reused);
    EXPECT_EQ(Reused.codeId, Retried.codeId);
  }
  EXPECT_EQ(Objects, 2u);
  EXPECT_EQ(E.stats().codeObjects, 1u);
  EXPECT_EQ(E.stats().failed, 1u);
}

TEST_F(PreparedCodeNative, RepresentativeProfileSixCellsTwentyEntries) {
  if (!J->getTargetTriple().isOSBinFormatELF())
    GTEST_SKIP() << "Native raw InstrProf fixture requires ELF";
  struct Row {
    uint32_t gain;
    uint32_t live[2];
  } Rows[6] = {};
  for (auto &R : Rows)
    R.gain = 7;
  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "cfg", Rows, 6);
  EJitOptimizer Optimizer(Registry, true);
  EJitPreparedCodeEmitter E(*J, 1);
  std::vector<EJitCodeBinding> Bindings = {
      {"cfg", reinterpret_cast<uintptr_t>(Rows), false}};
  SpecializationContext Spec;
  Spec.dimensions = {{"cell", 0}, {"trp", 1}};
  std::vector<std::string> CounterNames;
  unsigned T1Transforms = 0;
  J->getIRTransformLayer().setTransform(
      [&](orc::ThreadSafeModule M, orc::MaterializationResponsibility &MR)
          -> Expected<orc::ThreadSafeModule> {
        ++T1Transforms;
        orc::SymbolFlagsMap Flags;
        M.withModuleDo([&](Module &M) {
          Optimizer.clearAnalyses();
          Optimizer.runPipeline(M, Spec);
          CounterNames.assign(Optimizer.getLastCounterNames().begin(),
                              Optimizer.getLastCounterNames().end());
          for (const auto &Name : CounterNames) {
            Flags[J->mangleAndIntern("__profc_" + Name)] =
                JITSymbolFlags::Exported;
            Flags[J->mangleAndIntern("__profd_" + Name)] =
                JITSymbolFlags::Exported;
          }
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
          if (!Optimizer.getLastMayConstLoadSites().empty())
            Flags[J->mangleAndIntern("__ejit_mayconst_hits")] =
                JITSymbolFlags::Exported;
#endif
          EXPECT_FALSE(verifyModule(M));
          Optimizer.clearAnalyses();
        });
        if (!Flags.empty())
          if (auto Err = MR.defineMaterializing(std::move(Flags)))
            return std::move(Err);
        return std::move(M);
      });
  using Fn = uint32_t (*)(uint64_t, uint64_t, uint32_t);
  std::vector<void *> All;
  std::string LastIR;
  for (unsigned Entry = 0; Entry < 20; ++Entry) {
    std::string Name = "entry" + std::to_string(Entry);
    std::string IR = R"(
      %Cfg = type { i32, [2 x i32] }
      @cfg = external global [6 x %Cfg], !ejit.metadata !0
      define i32 @)" +
                     Name + R"((i64 %cell, i64 %trp, i32 %x) !ejit.metadata !3 {
        %gp = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 0
        %gain = load i32, ptr %gp, !ejit.may_const !7
        %lp = getelementptr [6 x %Cfg], ptr @cfg, i64 0, i64 %cell, i32 1, i64 %trp
        %live = load i32, ptr %lp
        %product = mul i32 %x, %gain
        %sum = add i32 %product, %live
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
    Spec.fnName = Name;
    Spec.cacheKey = uint64_t(Entry + 1) << 32;
    Spec.tier = CompileTier::Instrumented;
    Spec.dimensions[0].cellIdx = 0;
    Spec.profileData.clear();
    auto &T1JD = cantFail(J->createJITDylib("representative_" + Name));
    orc::SymbolMap Syms;
    Syms[J->mangleAndIntern("cfg")] = orc::ExecutorSymbolDef(
        orc::ExecutorAddr::fromPtr(Rows), JITSymbolFlags::Exported);
    cantFail(T1JD.define(orc::absoluteSymbols(std::move(Syms))));
    cantFail(J->addIRModule(T1JD, module(IR)));
    Fn T1 = cantFail(J->lookup(T1JD, Name)).toPtr<Fn>();
    for (unsigned I = 0; I < 64; ++I) {
      uint32_t Before = Rows[0].live[1];
      EXPECT_EQ(T1(0, 1, 3), Before + 21);
    }
    SmallVector<PgoCounterRef, 4> Counters;
    for (const auto &N : CounterNames)
      Counters.push_back(
          {N.c_str(), cantFail(J->lookup(T1JD, "__profc_" + N)).getValue(),
           cantFail(J->lookup(T1JD, "__profd_" + N)).getValue()});
    Spec.profileData = synthesizeProfileBuffer(Counters);
    ASSERT_FALSE(Spec.profileData.empty());
    Spec.tier = CompileTier::PGOUse;
    void *Shared = nullptr;
    for (unsigned Cell = 0; Cell < 6; ++Cell) {
      Spec.cacheKey = (uint64_t(Entry + 1) << 32) | Cell;
      Spec.dimensions[0].cellIdx = Cell;
      auto M = module(IR);
      M.withModuleDo([&](Module &M) {
        Optimizer.clearAnalyses();
        Optimizer.runPipeline(M, Spec);
        auto Count = M.getFunction(Name)->getEntryCount();
        ASSERT_TRUE(Count.has_value());
        EXPECT_EQ(Count->getCount(), 64u);
        EXPECT_FALSE(verifyModule(M));
        Optimizer.clearAnalyses();
      });
      auto S = scope(Name);
      S.source = SHA256::hash(arrayRefFromStringRef(IR));
      auto P = cantFail(EJitPreparedCode::create(std::move(M), S, Bindings));
      auto Code = cantFail(E.link(std::move(P)));
      EXPECT_EQ(Code.reused, Cell != 0);
      if (Cell == 0)
        Shared = Code.fn;
      EXPECT_EQ(Code.fn, Shared);
      for (unsigned TRP = 0; TRP < 2; ++TRP) {
        uint32_t Before = Rows[Cell].live[TRP];
        EXPECT_EQ(reinterpret_cast<Fn>(Code.fn)(Cell, TRP, 5),
                  Before + 35 + Cell);
        EXPECT_EQ(Rows[Cell].live[TRP], Before + 35);
      }
      All.push_back(Code.fn);
    }
    LastIR = IR;
  }
  EXPECT_EQ(T1Transforms, 20u);
  EXPECT_EQ(E.stats().codeObjects, 20u);
  EXPECT_EQ(E.stats().reused, 100u);
  EXPECT_EQ(Objects, 40u); // 20 real T1 objects + 20 real shared T2 objects.
  EXPECT_EQ(All.size(), 120u);
  RecordProperty("logical_ir_compilations", 120);
  RecordProperty("representative_t1_objects", T1Transforms);
  RecordProperty("all_codegen_objects", Objects);
  RecordProperty("physical_t2_objects", E.stats().codeObjects);
  RecordProperty("reused_t2_versions", E.stats().reused);
  // A changed configuration splits final code; reverting it finds the old
  // physical object again. This is not a logical invalidation/borrow test.
  Rows[5].gain = 9;
  Spec.dimensions[0].cellIdx = 5;
  auto Changed = module(LastIR);
  Changed.withModuleDo([&](Module &M) {
    Optimizer.clearAnalyses();
    Optimizer.runPipeline(M, Spec);
    Optimizer.clearAnalyses();
  });
  auto S = scope(Spec.fnName);
  S.source = SHA256::hash(arrayRefFromStringRef(LastIR));
  auto ChangedCode = cantFail(E.link(
      cantFail(EJitPreparedCode::create(std::move(Changed), S, Bindings))));
  EXPECT_FALSE(ChangedCode.reused);
  EXPECT_NE(ChangedCode.fn, All.back());
  uint32_t Before = Rows[5].live[0];
  EXPECT_EQ(reinterpret_cast<Fn>(ChangedCode.fn)(5, 0, 5), Before + 45 + 5);
  Rows[5].gain = 7;
  auto Restored = module(LastIR);
  Restored.withModuleDo([&](Module &M) {
    Optimizer.clearAnalyses();
    Optimizer.runPipeline(M, Spec);
    Optimizer.clearAnalyses();
  });
  auto RestoredCode = cantFail(E.link(
      cantFail(EJitPreparedCode::create(std::move(Restored), S, Bindings))));
  EXPECT_TRUE(RestoredCode.reused);
  EXPECT_EQ(RestoredCode.fn, All.back());
  EXPECT_EQ(E.stats().codeObjects, 21u);
}
#endif
} // namespace
