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
#include "clang/Basic/DiagnosticFrontend.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/STLFunctionalExtras.h"

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

  // Every parameter attribute below records an index that its consumer
  // resolves with Function::getArg(), so it must be an IR argument number.
  // getEjitIRArgIndex maps through the ABI lowering and refuses any parameter
  // that is not exactly one directly-passed IR argument; such a parameter is
  // dropped with a warning rather than annotated at a guessed index, because a
  // wrong index would specialize on an argument the user never marked.
  auto irArg = [&](unsigned ParamIndex) -> std::optional<unsigned> {
    return getEjitIRArgIndex(CGM, GlobalDecl(FD), ParamIndex);
  };
  auto diagnoseUnmappable = [&](const ParmVarDecl *PD, StringRef AttrName) {
    CGM.getDiags().Report(PD->getLocation(), diag::warn_ejit_param_not_lowered)
        << AttrName << PD;
  };

  // ejit_period_arr_ind (on parameters)
  for (unsigned I = 0; I < FD->getNumParams(); ++I) {
    const ParmVarDecl *PD = FD->getParamDecl(I);
    if (const auto *IdxAttr = PD->getAttr<EjitPeriodArrIndAttr>()) {
      std::optional<unsigned> ArgNo = irArg(I);
      if (!ArgNo) {
        diagnoseUnmappable(PD, "ejit_period_arr_ind");
        continue;
      }
      Entries.push_back(llvm::MDNode::get(
          Ctx, {llvm::MDString::get(Ctx, TAG_EJIT_PERIOD_ARR_IND),
                llvm::MDString::get(Ctx, IdxAttr->getPeriodName()),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(Ctx), *ArgNo))}));
    }
  }

  // ejit_free_dim (on parameters). Same {tag, name, argIndex} shape as
  // ejit_period_arr_ind with an empty name, so a consumer that identifies a dim
  // by operand position needs no special case. It is deliberately NOT a
  // dimension on the wire: no consumer of TAG_EJIT_PERIOD_ARR_IND matches this
  // tag, so the wrapper registers no dim, the cache key does not grow, and the
  // parameter is never substituted into the IR. Only PASS6 reads it, to
  // evaluate may_const addresses at a witness of 0.
  for (unsigned I = 0; I < FD->getNumParams(); ++I) {
    const ParmVarDecl *PD = FD->getParamDecl(I);
    if (PD->hasAttr<EjitFreeDimAttr>()) {
      std::optional<unsigned> ArgNo = irArg(I);
      if (!ArgNo) {
        diagnoseUnmappable(PD, "ejit_free_dim");
        continue;
      }
      Entries.push_back(llvm::MDNode::get(
          Ctx, {llvm::MDString::get(Ctx, TAG_EJIT_FREE_DIM),
                llvm::MDString::get(Ctx, ""),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(Ctx), *ArgNo))}));
    }
  }

  // ejit_bound_ptr (on pointer parameters). The pointee size is part of the
  // metadata so the wrapper can build a fixed borrowed descriptor at the
  // compile slow path. The runtime never takes ownership of the pointee.
  for (unsigned I = 0; I < FD->getNumParams(); ++I) {
    const ParmVarDecl *PD = FD->getParamDecl(I);
    if (const auto *BoundAttr = PD->getAttr<EjitBoundPtrAttr>()) {
      std::optional<unsigned> ArgNo = irArg(I);
      if (!ArgNo) {
        diagnoseUnmappable(PD, "ejit_bound_ptr");
        continue;
      }
      QualType Pointee = PD->getType()->getPointeeType();
      uint64_t Size =
          CGM.getContext().getTypeSizeInChars(Pointee).getQuantity();
      // Opaque pointers erase the pointee type, so the JIT cannot recover
      // alignof(T) from the IR. ejit_bound_ptr requires a complete object
      // type, so it holds for any conforming caller.
      uint64_t PointeeAlign =
          CGM.getContext().getTypeAlignInChars(Pointee).getQuantity();
      SmallVector<llvm::Metadata *, 8> BoundOps = {
          llvm::MDString::get(Ctx, TAG_EJIT_BOUND_PTR),
          llvm::MDString::get(Ctx, BoundAttr->getPeriodName()),
          llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), *ArgNo)),
          llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), Size)),
          llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx),
                                     PointeeAlign))};
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

// Share the traversal for global offsets and bound-object field extents.
// Arrays of records matter for a local witness selecting slots within a cell.
// The budget bounds metadata growth; omitted descriptors only miss folding.
static void collectMayConstFields(
    ASTContext &Ctx, QualType Type, uint64_t BaseOffset,
    llvm::function_ref<void(uint64_t, QualType)> Emit, unsigned &Budget) {
  if (!Budget || Type.isVolatileQualified())
    return;
  --Budget;
  if (const auto *Array = Ctx.getAsConstantArrayType(Type)) {
    if (Array->getSize().getActiveBits() > 64)
      return;
    QualType Element = Array->getElementType();
    const uint64_t Stride = Ctx.getTypeSizeInChars(Element).getQuantity();
    for (uint64_t I = 0, Count = Array->getSize().getZExtValue();
         I < Count && Budget; ++I) {
      if (Stride && I > (UINT64_MAX - BaseOffset) / Stride)
        break;
      collectMayConstFields(Ctx, Element, BaseOffset + I * Stride, Emit, Budget);
    }
    return;
  }
  const auto *RD = Type->getAsRecordDecl();
  // A union's shared offset cannot authorize mutable sibling members.
  if (!RD || !RD->isCompleteDefinition() || RD->isUnion())
    return;
  const ASTRecordLayout &Layout = Ctx.getASTRecordLayout(RD);
  for (const FieldDecl *FD : RD->fields()) {
    if (!Budget)
      break;
    if (FD->isBitField())
      continue;
    uint64_t Relative = Layout.getFieldOffset(FD->getFieldIndex()) / 8;
    if (Relative > UINT64_MAX - BaseOffset)
      continue;
    const uint64_t Offset = BaseOffset + Relative;
    QualType FieldType = FD->getType();
    if (FD->hasAttr<EjitMayConstAttr>() && !FieldType.isVolatileQualified())
      Emit(Offset, FieldType);
    collectMayConstFields(Ctx, FieldType, Offset, Emit, Budget);
  }
}

static void collectMayConstFieldOffsets(ASTContext &Ctx, const RecordDecl *RD,
                                        uint64_t BaseOffset,
                                        SmallVectorImpl<uint64_t> &Offsets) {
  unsigned Budget = 4096;
  collectMayConstFields(Ctx, Ctx.getRecordType(RD), BaseOffset,
                       [&](uint64_t Offset, QualType) {
                         // Preserve the existing i32 global-offset encoding.
                         if (Offset <= UINT32_MAX)
                           Offsets.push_back(Offset);
                       }, Budget);
}

static void collectBoundMayConstFields(
    ASTContext &Ctx, const RecordDecl *RD, uint64_t BaseOffset,
    SmallVectorImpl<std::pair<uint64_t, uint64_t>> &Fields) {
  unsigned Budget = 4096;
  collectMayConstFields(Ctx, Ctx.getRecordType(RD), BaseOffset,
                       [&](uint64_t Offset, QualType Type) {
                         Fields.push_back(
                             {Offset, Ctx.getTypeSizeInChars(Type).getQuantity()});
                       }, Budget);
}

/// Record complete field ranges in one pointee element, including nested slot
/// arrays. Keep this separate from the legacy offset-only fallback: these
/// ranges authorize bounded absolute-address reads after pointer substitution.
/// A work budget bounds metadata growth; an incomplete layout authorizes
/// nothing.
static bool collectPointerPeriodFields(
    ASTContext &Ctx, QualType Ty, uint64_t Base,
    SmallVectorImpl<std::pair<uint64_t, uint64_t>> &Fields, unsigned &Budget) {
  if (!Budget || Ty->isIncompleteType() || Ty->isVariablyModifiedType())
    return false;
  --Budget;
  if (Ty.isVolatileQualified() || Ty->isAtomicType())
    return true;
  if (const auto *AT = Ctx.getAsConstantArrayType(Ty)) {
    uint64_t Count = AT->getSize().getLimitedValue();
    uint64_t Stride =
        Ctx.getTypeSizeInChars(AT->getElementType()).getQuantity();
    // Visit the element type once so large unmarked arrays stay cheap, but
    // charge every expanded range against the budget.
    SmallVector<std::pair<uint64_t, uint64_t>, 16> ElementFields;
    if (!collectPointerPeriodFields(Ctx, AT->getElementType(), 0,
                                    ElementFields, Budget))
      return false;
    if (ElementFields.empty())
      return true;
    if (Count > Budget / ElementFields.size())
      return false;
    Budget -= Count * ElementFields.size();
    for (uint64_t I = 0; I < Count; ++I)
      for (const auto &[Offset, Size] : ElementFields)
        Fields.emplace_back(Base + I * Stride + Offset, Size);
    return true;
  }
  const auto *RD = Ty->getAsRecordDecl();
  if (!RD || RD->isUnion())
    return true;
  // Base subobjects are not described by this C field walker.
  if (const auto *CXXRD = dyn_cast<CXXRecordDecl>(RD))
    if (CXXRD->getNumBases())
      return false;
  const ASTRecordLayout &Layout = Ctx.getASTRecordLayout(RD);
  for (const FieldDecl *FD : RD->fields()) {
    QualType FT = FD->getType();
    if (FD->isBitField() || FT.isVolatileQualified() || FT->isAtomicType())
      continue;
    if (FT->isIncompleteType() || FT->isVariablyModifiedType())
      continue;
    uint64_t Offset = Base + Layout.getFieldOffset(FD->getFieldIndex()) / 8;
    if (FD->hasAttr<EjitMayConstAttr>())
      Fields.emplace_back(Offset, Ctx.getTypeSizeInChars(FT).getQuantity());
    if (!collectPointerPeriodFields(Ctx, FT, Offset, Fields, Budget))
      return false;
  }
  return true;
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

  if (!Entries.empty() && VD->getType()->isPointerType()) {
    QualType Pointee = VD->getType()->getPointeeType();
    SmallVector<std::pair<uint64_t, uint64_t>, 16> Fields;
    unsigned Budget = 4096;
    if (collectPointerPeriodFields(CGM.getContext(), Pointee, 0, Fields,
                                   Budget) &&
        !Fields.empty()) {
      auto IntMD = [&](uint64_t V) {
        return llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), V));
      };
      SmallVector<llvm::Metadata *, 16> Layout = {
          llvm::MDString::get(Ctx, "ejit_pointer_layout"),
          IntMD(CGM.getContext().getTypeSizeInChars(Pointee).getQuantity())};
      for (const auto &[Offset, Size] : Fields)
        Layout.push_back(llvm::MDNode::get(Ctx, {IntMD(Offset), IntMD(Size)}));
      Entries.push_back(llvm::MDNode::get(Ctx, Layout));
    }
  }

  if (!Entries.empty()) {
    llvm::MDNode *MD = llvm::MDNode::getDistinct(Ctx, Entries);
    GV->setMetadata(MD_EJIT_METADATA, MD);
  }
}
