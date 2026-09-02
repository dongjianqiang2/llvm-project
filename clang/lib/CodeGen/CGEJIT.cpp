//===--- CGEJIT.cpp - EmbeddedJIT CodeGen Metadata ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements EmbeddedJIT metadata generation for LLVM IR.
//
//===----------------------------------------------------------------------===//

#include "CodeGenModule.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/AttrKinds.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace clang;
using namespace CodeGen;
using namespace llvm::ejit;

static void collectBoundMayConstFields(
    ASTContext &Ctx, const RecordDecl *RD, uint64_t BaseOffset,
    SmallVectorImpl<std::pair<uint64_t, uint64_t>> &Fields);

/// Emit !ejit.metadata for an ejit_entry or ejit_period_lc function.
void clang::CodeGen::emitEjitFunctionMetadata(CodeGenModule &CGM,
                                              const FunctionDecl *FD,
                                              llvm::Function *F) {
  llvm::LLVMContext &Ctx = CGM.getLLVMContext();
  SmallVector<llvm::Metadata *, 8> Entries;

  // ejit_entry
  if (FD->hasAttr<EjitEntryAttr>()) {
    Entries.push_back(llvm::MDNode::get(Ctx,
        llvm::MDString::get(Ctx, TAG_EJIT_ENTRY)));
    // Prevent the inliner from merging ejit_entry into its callers.
    // In LTO pipelines the inliner runs before EJitWrapperGenPass can
    // add this attribute; emitting it at CodeGen time ensures the
    // function survives so PASS1 can extract its bitcode and PASS3
    // can insert the JIT wrapper. Sema rejects always_inline on
    // ejit_entry; the AlwaysInline guard is a backstop for hand-written
    // IR (noinline + alwaysinline is illegal and aborts the verifier).
    if (!F->hasFnAttribute(llvm::Attribute::AlwaysInline))
      F->addFnAttr(llvm::Attribute::NoInline);
  }

  // ejit_period_lc
  for (const auto *LCA : FD->specific_attrs<EjitPeriodLcAttr>()) {
    Entries.push_back(llvm::MDNode::get(Ctx, {
        llvm::MDString::get(Ctx, TAG_EJIT_PERIOD_LC),
        llvm::MDString::get(Ctx, LCA->getPeriodName())
    }));
  }
  // Same LTO-inliner hazard as ejit_entry: in FullLTO the inliner runs
  // before EJitAotModulePass, so PASS4 (EJitPeriodHandler) would see an
  // inlined-away function and fail to emit ejit_deactivate/ejit_activate
  // at its entry/returns. Unlike ejit_entry, PASS4 does not self-defend
  // with NoInline, so the CodeGen-time attribute is the only guard.
  // Lifecycle guards are entered/left rarely, so NoInline has no real
  // perf cost. Sema rejects always_inline here too; the guard backstops
  // hand-written IR.
  if (FD->hasAttr<EjitPeriodLcAttr>() &&
      !F->hasFnAttribute(llvm::Attribute::AlwaysInline))
    F->addFnAttr(llvm::Attribute::NoInline);

  // ejit_period_arr_ind (on parameters)
  for (unsigned I = 0; I < FD->getNumParams(); ++I) {
    const ParmVarDecl *PD = FD->getParamDecl(I);
    if (const auto *IdxAttr = PD->getAttr<EjitPeriodArrIndAttr>()) {
      Entries.push_back(llvm::MDNode::get(Ctx, {
          llvm::MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
          llvm::MDString::get(Ctx, IdxAttr->getPeriodName()),
          llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), I))
      }));
    }
  }

  // ejit_bound_ptr (on pointer parameters). The pointee size is part of the
  // metadata so the wrapper can build a fixed borrowed descriptor at the
  // compile slow path. The runtime never takes ownership of the pointee.
  for (unsigned I = 0; I < FD->getNumParams(); ++I) {
    const ParmVarDecl *PD = FD->getParamDecl(I);
    if (const auto *BoundAttr = PD->getAttr<EjitBoundPtrAttr>()) {
      QualType Pointee = PD->getType()->getPointeeType();
      uint64_t Size =
          CGM.getContext().getTypeSizeInChars(Pointee).getQuantity();
      SmallVector<llvm::Metadata *, 8> BoundOps = {
          llvm::MDString::get(Ctx, TAG_EJIT_BOUND_PTR),
          llvm::MDString::get(Ctx, BoundAttr->getPeriodName()),
          llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), I)),
          llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), Size))};
      if (const auto *RD = Pointee->getAsRecordDecl()) {
        SmallVector<std::pair<uint64_t, uint64_t>, 8> Fields;
        collectBoundMayConstFields(CGM.getContext(), RD, 0, Fields);
        for (const auto &[Offset, FieldSize] : Fields)
          BoundOps.push_back(llvm::MDNode::get(
              Ctx, {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(Ctx), Offset)),
                    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(Ctx), FieldSize))}));
      }
      Entries.push_back(llvm::MDNode::get(Ctx, BoundOps));
    }
  }

  if (!Entries.empty()) {
    llvm::MDNode *MD = llvm::MDNode::getDistinct(Ctx, Entries);
    F->setMetadata(MD_EJIT_METADATA, MD);
  }
}

/// Recursively collect byte offsets of ejit_may_const fields in a record type.
static void collectMayConstFieldOffsets(ASTContext &Ctx, const RecordDecl *RD,
                                        uint64_t BaseOffset,
                                        SmallVectorImpl<uint64_t> &Offsets) {
  // Union members all share an offset, so an offset cannot identify one member:
  // recording a may_const member would also match a load of a mutable sibling.
  // The per-load !ejit.may_const marker stays, so a genuine may_const union
  // member is still specialized; only the offset-based fallback is withheld.
  if (RD->isUnion())
    return;

  const ASTRecordLayout &Layout = Ctx.getASTRecordLayout(RD);
  for (const FieldDecl *FD : RD->fields()) {
    if (FD->isBitField())
      continue;
    uint64_t FieldOff = BaseOffset + Layout.getFieldOffset(FD->getFieldIndex()) / 8;
    // A volatile field is read exactly as written and can never be folded, so
    // clang withholds its per-load marker (see EmitLValueForField). Recording
    // its offset would let the fallback hand that marker back.
    if (FD->hasAttr<EjitMayConstAttr>() && !FD->getType().isVolatileQualified())
      Offsets.push_back(FieldOff);
    if (const auto *InnerRD = FD->getType()->getAsRecordDecl())
      collectMayConstFieldOffsets(Ctx, InnerRD, FieldOff, Offsets);
  }
}

static void collectBoundMayConstFields(
    ASTContext &Ctx, const RecordDecl *RD, uint64_t BaseOffset,
    SmallVectorImpl<std::pair<uint64_t, uint64_t>> &Fields) {
  if (RD->isUnion())
    return;
  const ASTRecordLayout &Layout = Ctx.getASTRecordLayout(RD);
  for (const FieldDecl *FD : RD->fields()) {
    if (FD->isBitField())
      continue;
    uint64_t Offset =
        BaseOffset + Layout.getFieldOffset(FD->getFieldIndex()) / 8;
    QualType FieldType = FD->getType();
    if (FD->hasAttr<EjitMayConstAttr>() && !FieldType.isVolatileQualified())
      Fields.push_back(
          {Offset, Ctx.getTypeSizeInChars(FieldType).getQuantity()});
    if (const auto *InnerRD = FieldType->getAsRecordDecl())
      collectBoundMayConstFields(Ctx, InnerRD, Offset, Fields);
  }
}

/// Emit !ejit.metadata for an ejit_period or ejit_period_arr global variable.
void clang::CodeGen::emitEjitGlobalMetadata(CodeGenModule &CGM,
                                            const VarDecl *VD,
                                            llvm::GlobalVariable *GV) {
  llvm::LLVMContext &Ctx = CGM.getLLVMContext();
  SmallVector<llvm::Metadata *, 4> Entries;

  // ejit_period
  if (const auto *PA = VD->getAttr<EjitPeriodAttr>()) {
    Entries.push_back(llvm::MDNode::get(Ctx, {
        llvm::MDString::get(Ctx, TAG_EJIT_PERIOD),
        llvm::MDString::get(Ctx, PA->getPeriodName())
    }));
  }

  // ejit_period_arr
  if (const auto *PAA = VD->getAttr<EjitPeriodArrAttr>()) {
    uint64_t Size = 0;
    if (const auto *CAT =
            CGM.getContext().getAsConstantArrayType(VD->getType())) {
      Size = CAT->getSize().getZExtValue();
    }
    Entries.push_back(llvm::MDNode::get(Ctx, {
        llvm::MDString::get(Ctx, TAG_EJIT_PERIOD_ARR),
        llvm::MDString::get(Ctx, PAA->getPeriodName()),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), Size))
    }));
  }

  // ejit_may_const_field: encode byte offsets for PASS6 fallback
  QualType VT = VD->getType();
  if (const auto *AT = CGM.getContext().getAsArrayType(VT))
    VT = AT->getElementType();
  if (VT->isPointerType())
    VT = VT->getPointeeType();
  if (const auto *RD = VT->getAsRecordDecl()) {
    if (RD->isCompleteDefinition()) {
      SmallVector<uint64_t, 4> Offsets;
      collectMayConstFieldOffsets(CGM.getContext(), RD, 0, Offsets);
      for (uint64_t Off : Offsets) {
        Entries.push_back(llvm::MDNode::get(Ctx, {
            llvm::MDString::get(Ctx, TAG_EJIT_MAY_CONST_FIELD),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), Off))
        }));
      }
    }
  }

  if (!Entries.empty()) {
    llvm::MDNode *MD = llvm::MDNode::getDistinct(Ctx, Entries);
    GV->setMetadata(MD_EJIT_METADATA, MD);
  }
}
