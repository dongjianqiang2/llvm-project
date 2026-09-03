//===-- EJitRegisterBitcodeTest.cpp - AOT bitcode extraction tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

static void markEntry(Function &F) {
  LLVMContext &Ctx = F.getContext();
  Metadata *Entry = MDNode::get(Ctx, MDString::get(Ctx, TAG_EJIT_ENTRY));
  F.setMetadata(MD_EJIT_METADATA, MDNode::getDistinct(Ctx, {Entry}));
}

static Function *createUnaryFunction(Module &M, StringRef Name,
                                     GlobalValue::LinkageTypes Linkage,
                                     int Addend) {
  LLVMContext &Ctx = M.getContext();
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *FT = FunctionType::get(I32, {I32}, false);
  auto *F = Function::Create(FT, Linkage, Name, M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
  B.CreateRet(B.CreateAdd(F->getArg(0), B.getInt32(Addend)));
  return F;
}

static std::string serializeModule(const Module &M) {
  std::string Bitcode;
  raw_string_ostream OS(Bitcode);
  WriteBitcodeToFile(M, OS);
  OS.flush();
  return Bitcode;
}

static Expected<std::unique_ptr<Module>>
parseEmbeddedBitcode(Module &Host, LLVMContext &BitcodeCtx) {
  GlobalVariable *Embedded =
      Host.getGlobalVariable(GV_EJIT_BITCODE, /*AllowInternal=*/true);
  if (!Embedded)
    return createStringError(inconvertibleErrorCode(),
                             "embedded EJIT bitcode global is missing");
  auto *Data = dyn_cast<ConstantDataArray>(Embedded->getInitializer());
  if (!Data)
    return createStringError(inconvertibleErrorCode(),
                             "embedded EJIT bitcode is not byte data");
  return parseBitcodeFile(
      MemoryBufferRef(Data->getRawDataValues(), "ejit-embedded.bc"),
      BitcodeCtx);
}

TEST(EJitRegisterBitcode,
     PreSerializationGlobalDCERemovesOnlyUnreachableInternalBodies) {
  LLVMContext Ctx;
  Module M("pre_serialization_dce", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *FT = FunctionType::get(I32, {I32}, false);

  Function *Live =
      createUnaryFunction(M, "live_helper", GlobalValue::InternalLinkage, 1);
  Function *Shared =
      createUnaryFunction(M, "shared_helper", GlobalValue::InternalLinkage, 2);
  Function *AddressTaken = createUnaryFunction(M, "address_taken_helper",
                                               GlobalValue::InternalLinkage, 3);
  Function *Dead = Function::Create(FT, GlobalValue::InternalLinkage,
                                    "inlined_source_body", M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Dead));
  Value *V = Dead->getArg(0);
  for (int I = 0; I != 256; ++I)
    V = B.CreateAdd(V, B.getInt32(I + 1));
  B.CreateRet(V);

  auto *Callback = new GlobalVariable(M, PtrTy, /*isConstant=*/true,
                                      GlobalValue::InternalLinkage,
                                      AddressTaken, "callback");

  Function *EntryA =
      Function::Create(FT, GlobalValue::ExternalLinkage, "entry_a", M);
  markEntry(*EntryA);
  B.SetInsertPoint(BasicBlock::Create(Ctx, "entry", EntryA));
  Value *LiveValue = B.CreateCall(Live, {EntryA->getArg(0)});
  Value *SharedValue = B.CreateCall(Shared, {EntryA->getArg(0)});
  Value *Target = B.CreateLoad(PtrTy, Callback);
  Value *IndirectValue = B.CreateCall(FT, Target, {EntryA->getArg(0)});
  B.CreateRet(B.CreateAdd(B.CreateAdd(LiveValue, SharedValue), IndirectValue));

  Function *EntryB =
      Function::Create(FT, GlobalValue::InternalLinkage, "entry_b", M);
  markEntry(*EntryB);
  B.SetInsertPoint(BasicBlock::Create(Ctx, "entry", EntryB));
  B.CreateRet(B.CreateCall(Shared, {EntryB->getArg(0)}));

  std::string Before = serializeModule(M);
  auto Stats = llvm::ejit::detail::runEJitPreSerializationGlobalDCE(M);
  std::string After = serializeModule(M);

  RecordProperty("bitcode_bytes_before", Before.size());
  RecordProperty("bitcode_bytes_after", After.size());

  EXPECT_EQ(Stats.FunctionDefinitionsBefore, 6u);
  EXPECT_EQ(Stats.FunctionDefinitionsAfter, 5u);
  EXPECT_GT(Stats.InstructionsBefore, Stats.InstructionsAfter);
  EXPECT_LT(After.size(), Before.size());
  EXPECT_EQ(M.getFunction("inlined_source_body"), nullptr);
  EXPECT_NE(M.getFunction("live_helper"), nullptr);
  EXPECT_NE(M.getFunction("shared_helper"), nullptr);
  EXPECT_NE(M.getFunction("address_taken_helper"), nullptr);
  ASSERT_NE(M.getFunction("entry_b"), nullptr);
  EXPECT_TRUE(M.getFunction("entry_b")->hasInternalLinkage());
}

TEST(EJitRegisterBitcode,
     ExtractedBitcodeKeepsLiveSharedAndAddressTakenDefinitions) {
  LLVMContext Ctx;
  Module M("aot_extract_reachability", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *FT = FunctionType::get(I32, {I32}, false);

  Function *Shared =
      createUnaryFunction(M, "shared_helper", GlobalValue::InternalLinkage, 4);
  Shared->addFnAttr(Attribute::NoInline);
  Function *AddressTaken = createUnaryFunction(M, "address_taken_helper",
                                               GlobalValue::InternalLinkage, 5);
  AddressTaken->addFnAttr(Attribute::NoInline);
  createUnaryFunction(M, "unreachable_helper", GlobalValue::InternalLinkage,
                      99);

  auto *Callback = new GlobalVariable(M, PtrTy, /*isConstant=*/true,
                                      GlobalValue::InternalLinkage,
                                      AddressTaken, "callback");

  Function *EntryA =
      Function::Create(FT, GlobalValue::ExternalLinkage, "entry_a", M);
  markEntry(*EntryA);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", EntryA));
  Value *SharedValue = B.CreateCall(Shared, {EntryA->getArg(0)});
  Value *Target = B.CreateLoad(PtrTy, Callback);
  Value *IndirectValue = B.CreateCall(FT, Target, {EntryA->getArg(0)});
  B.CreateRet(B.CreateAdd(SharedValue, IndirectValue));

  Function *EntryB =
      Function::Create(FT, GlobalValue::ExternalLinkage, "entry_b", M);
  markEntry(*EntryB);
  B.SetInsertPoint(BasicBlock::Create(Ctx, "entry", EntryB));
  B.CreateRet(B.CreateCall(Shared, {EntryB->getArg(0)}));

  ModuleAnalysisManager MAM;
  EJitRegisterBitcodePass().run(M, MAM);

  LLVMContext BitcodeCtx;
  auto ExtractedOrErr = parseEmbeddedBitcode(M, BitcodeCtx);
  if (!ExtractedOrErr)
    FAIL() << toString(ExtractedOrErr.takeError());
  Module &Extracted = **ExtractedOrErr;
  for (StringRef Name :
       {"entry_a", "entry_b", "shared_helper", "address_taken_helper"}) {
    Function *F = Extracted.getFunction(Name);
    ASSERT_NE(F, nullptr) << Name.str();
    EXPECT_FALSE(F->isDeclaration()) << Name.str();
  }
  EXPECT_EQ(Extracted.getFunction("unreachable_helper"), nullptr);
}

TEST(EJitRegisterBitcode, ExtractedBitcodeKeepsDiscardableEntryDefinition) {
  LLVMContext Ctx;
  Module M("aot_extract_discardable_entry", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *FT = FunctionType::get(I32, {I32}, false);
  Function *Entry =
      Function::Create(FT, GlobalValue::LinkOnceODRLinkage, "entry", M);
  markEntry(*Entry);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Entry));
  B.CreateRet(B.CreateAdd(Entry->getArg(0), B.getInt32(1)));

  ModuleAnalysisManager MAM;
  EJitRegisterBitcodePass().run(M, MAM);

  LLVMContext BitcodeCtx;
  auto ExtractedOrErr = parseEmbeddedBitcode(M, BitcodeCtx);
  if (!ExtractedOrErr)
    FAIL() << toString(ExtractedOrErr.takeError());
  Function *ExtractedEntry = (*ExtractedOrErr)->getFunction("entry");
  ASSERT_NE(ExtractedEntry, nullptr);
  EXPECT_FALSE(ExtractedEntry->isDeclaration());
  EXPECT_TRUE(ExtractedEntry->hasLinkOnceODRLinkage());
}

TEST(EJitRegisterBitcode,
     ExtractedBitcodeKeepsConservativeUnknownIndirectTargets) {
  LLVMContext Ctx;
  Module M("aot_extract_unknown_indirect", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *FT = FunctionType::get(I32, {I32}, false);

  Function *Candidate = createUnaryFunction(M, "dynamic_candidate",
                                            GlobalValue::InternalLinkage, 6);
  Candidate->addFnAttr(Attribute::NoInline);
  new GlobalVariable(M, PtrTy, /*isConstant=*/true,
                     GlobalValue::InternalLinkage, Candidate,
                     "unrelated_address_escape");

  auto *ResolverTy = FunctionType::get(PtrTy, {}, false);
  Function *Resolver = Function::Create(
      ResolverTy, GlobalValue::ExternalLinkage, "resolve_callback", M);
  Function *Entry =
      Function::Create(FT, GlobalValue::ExternalLinkage, "entry", M);
  markEntry(*Entry);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Entry));
  Value *Target = B.CreateCall(Resolver);
  B.CreateRet(B.CreateCall(FT, Target, {Entry->getArg(0)}));

  ModuleAnalysisManager MAM;
  EJitRegisterBitcodePass().run(M, MAM);

  LLVMContext BitcodeCtx;
  auto ExtractedOrErr = parseEmbeddedBitcode(M, BitcodeCtx);
  if (!ExtractedOrErr)
    FAIL() << toString(ExtractedOrErr.takeError());
  Module &Extracted = **ExtractedOrErr;
  Function *Retained = Extracted.getFunction("dynamic_candidate");
  ASSERT_NE(Retained, nullptr);
  EXPECT_FALSE(Retained->isDeclaration());
  EXPECT_EQ(Extracted.getGlobalVariable("unrelated_address_escape", true),
            nullptr);
}

TEST(EJitRegisterBitcode,
     ExtractionShrinksDeadBodiesWithoutChangingProfileSiteShape) {
  LLVMContext Ctx;
  Module M("aot_extract_profile_shape", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *FT = FunctionType::get(I32, {I32}, false);

  Function *Target =
      createUnaryFunction(M, "target", GlobalValue::InternalLinkage, 9);
  Target->addFnAttr(Attribute::NoInline);
  createUnaryFunction(M, "dead_helper", GlobalValue::InternalLinkage, 99);
  new GlobalVariable(M, PtrTy, /*isConstant=*/true,
                     GlobalValue::InternalLinkage, Target,
                     "unrelated_address_escape");
  auto *ResolverTy = FunctionType::get(PtrTy, {}, false);
  Function *Resolver = Function::Create(
      ResolverTy, GlobalValue::ExternalLinkage, "resolve_callback", M);

  Function *Entry =
      Function::Create(FT, GlobalValue::ExternalLinkage, "entry", M);
  markEntry(*Entry);
  BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", Entry);
  BasicBlock *HotBB = BasicBlock::Create(Ctx, "hot", Entry);
  BasicBlock *ColdBB = BasicBlock::Create(Ctx, "cold", Entry);
  BasicBlock *ExitBB = BasicBlock::Create(Ctx, "exit", Entry);
  IRBuilder<> B(EntryBB);
  B.CreateCondBr(B.CreateICmpSGT(Entry->getArg(0), B.getInt32(0)), HotBB,
                 ColdBB);
  B.SetInsertPoint(HotBB);
  Value *Callee = B.CreateCall(Resolver);
  Value *HotValue = B.CreateCall(FT, Callee, {Entry->getArg(0)});
  B.CreateBr(ExitBB);
  B.SetInsertPoint(ColdBB);
  Value *ColdValue = B.CreateSub(B.getInt32(0), Entry->getArg(0));
  B.CreateBr(ExitBB);
  B.SetInsertPoint(ExitBB);
  PHINode *Result = B.CreatePHI(I32, 2);
  Result->addIncoming(HotValue, HotBB);
  Result->addIncoming(ColdValue, ColdBB);
  B.CreateRet(Result);

  ModuleAnalysisManager MAM;
  EJitRegisterBitcodePass().run(M, MAM);

  LLVMContext BitcodeCtx;
  auto ExtractedOrErr = parseEmbeddedBitcode(M, BitcodeCtx);
  if (!ExtractedOrErr)
    FAIL() << toString(ExtractedOrErr.takeError());
  Module &Extracted = **ExtractedOrErr;
  Function *ExtractedEntry = Extracted.getFunction("entry");
  ASSERT_NE(ExtractedEntry, nullptr);
  unsigned ConditionalBranches = 0;
  unsigned IndirectCalls = 0;
  for (BasicBlock &BB : *ExtractedEntry) {
    for (Instruction &I : BB) {
      if (auto *Br = dyn_cast<BranchInst>(&I))
        ConditionalBranches += Br->isConditional();
      if (auto *CB = dyn_cast<CallBase>(&I))
        IndirectCalls += CB->getCalledFunction() == nullptr;
    }
  }
  EXPECT_EQ(ConditionalBranches, 1u);
  EXPECT_EQ(IndirectCalls, 1u);
  EXPECT_EQ(Extracted.getFunction("dead_helper"), nullptr);
}

#ifdef NDEBUG
TEST(EJitRegisterBitcode,
     ExtractedBitcodeDropsSourceBodyAfterAOTInliningAndInternalization) {
  LLVMContext Ctx;
  Module M("aot_extract_post_inline_dce", Ctx);
  auto *I32 = Type::getInt32Ty(Ctx);
  auto *FT = FunctionType::get(I32, {I32}, false);

  Function *InlineSource = createUnaryFunction(M, "inlined_source_body",
                                               GlobalValue::ExternalLinkage, 7);
  InlineSource->addFnAttr(Attribute::AlwaysInline);
  Function *Entry =
      Function::Create(FT, GlobalValue::ExternalLinkage, "entry", M);
  markEntry(*Entry);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Entry));
  B.CreateRet(B.CreateCall(InlineSource, {Entry->getArg(0)}));

  ModuleAnalysisManager MAM;
  EJitRegisterBitcodePass().run(M, MAM);

  LLVMContext BitcodeCtx;
  auto ExtractedOrErr = parseEmbeddedBitcode(M, BitcodeCtx);
  if (!ExtractedOrErr)
    FAIL() << toString(ExtractedOrErr.takeError());
  Module &Extracted = **ExtractedOrErr;
  ASSERT_NE(Extracted.getFunction("entry"), nullptr);
  EXPECT_EQ(Extracted.getFunction("inlined_source_body"), nullptr);
}
#endif

} // namespace
