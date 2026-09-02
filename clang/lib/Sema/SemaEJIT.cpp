//===--- SemaEJIT.cpp - EmbeddedJIT Attribute Handling --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements EmbeddedJIT attribute processing.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"

using llvm::ejit::MAX_BOUND_PTR_PARAMS;
using llvm::ejit::MAX_PERIOD_ARR_IND_PARAMS;
using llvm::ejit::MAX_PERIOD_ARR_SIZE;

using namespace clang;

namespace {

/// RecursiveASTVisitor that checks if a function calls itself.
class RecursiveCallVisitor
    : public RecursiveASTVisitor<RecursiveCallVisitor> {
  const FunctionDecl *FD;
public:
  bool FoundRecursiveCall = false;

  explicit RecursiveCallVisitor(const FunctionDecl *FD) : FD(FD) {}

  bool VisitCallExpr(CallExpr *CE) {
    if (auto *Callee = dyn_cast<DeclRefExpr>(CE->getCallee()->IgnoreParens())) {
      if (Callee->getDecl() == FD) {
        FoundRecursiveCall = true;
        return false; // Stop traversal
      }
    }
    return true;
  }
};

} // anonymous namespace

/// handleEjitMayConstAttr - Process the ejit_may_const attribute.
/// Checks:
///   1. Applies only to FieldDecl
///   2. Field type must be integer, boolean, floating-point, struct, or array
void handleEjitMayConstAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *FD = dyn_cast<FieldDecl>(D);
  if (!FD) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "field declarations";
    return;
  }

  QualType FT = FD->getType();

  // Check type: integer, boolean, floating-point, struct/class, or array
  if (!FT->isIntegerType() && !FT->isBooleanType() &&
      !FT->isRealFloatingType() && !FT->isStructureOrClassType() &&
      !FT->isArrayType()) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute()
        << "fields of integer, boolean, floating-point, or struct type";
    return;
  }

  // volatile fields are silently skipped — their loads won't get
  // !ejit.may_const metadata, so the JIT naturally ignores them.

  D->addAttr(::new (S.Context) EjitMayConstAttr(S.Context, AL));
}

/// handleEjitPeriodAttr - Process the ejit_period(name) attribute.
/// Checks:
///   1. Applies only to VarDecl
///   2. Must have global storage
///   3. Cannot be an array (use ejit_period_arr)
///   4. No duplicate period/period_arr attributes
void handleEjitPeriodAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *VD = dyn_cast<VarDecl>(D);
  if (!VD) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "variable declarations";
    return;
  }

  // Must be a global variable
  if (!VD->hasGlobalStorage()) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "global variables";
    return;
  }

  // Cannot be an array — use ejit_period_arr for arrays
  if (VD->getType()->isArrayType()) {
    S.Diag(AL.getLoc(), diag::err_ejit_period_not_array) << VD;
    return;
  }

  // Check for duplicate period/period_arr attributes
  if (VD->hasAttr<EjitPeriodAttr>() || VD->hasAttr<EjitPeriodArrAttr>()) {
    S.Diag(AL.getLoc(), diag::err_ejit_period_conflict) << VD;
    return;
  }

  // Extract period name
  StringRef PeriodName;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, PeriodName))
    return;

  VD->addAttr(::new (S.Context) EjitPeriodAttr(S.Context, AL, PeriodName));
}

/// handleEjitPeriodArrAttr - Process the ejit_period_arr(name) attribute.
/// Checks:
///   1. Applies only to VarDecl
///   2. Must have global storage
///   3. Must be an array type (constant size < 100) OR pointer-to-struct
///   4. No duplicate period/period_arr attributes
void handleEjitPeriodArrAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *VD = dyn_cast<VarDecl>(D);
  if (!VD) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "variable declarations";
    return;
  }

  // Must be a global variable
  if (!VD->hasGlobalStorage()) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "global variables";
    return;
  }

  // Check type: array (with constant size < 100) or pointer-to-struct
  QualType VT = VD->getType();
  if (const ArrayType *AT = S.Context.getAsArrayType(VT)) {
    if (const auto *CAT = dyn_cast<ConstantArrayType>(AT)) {
      uint64_t Size = CAT->getSize().getZExtValue();
      if (Size > MAX_PERIOD_ARR_SIZE) {
        S.Diag(AL.getLoc(), diag::err_ejit_period_arr_too_large)
            << VD << static_cast<unsigned>(Size);
        return;
      }
    } else {
      // Non-constant array size (VLA, etc.) — error
      S.Diag(AL.getLoc(), diag::err_ejit_period_arr_not_scalar) << VD;
      return;
    }
  } else if (VT->isPointerType()) {
    // Pointer type — must point to a structure/class type.
    // Array size is dynamic (user guarantees correctness); Size=0 in metadata.
    QualType Pointee = VT->getPointeeType();
    if (!Pointee->isStructureOrClassType()) {
      S.Diag(AL.getLoc(), diag::err_ejit_period_arr_not_scalar) << VD;
      return;
    }
    // No size validation for pointer types.
  } else {
    S.Diag(AL.getLoc(), diag::err_ejit_period_arr_not_scalar) << VD;
    return;
  }

  // Check for duplicate period/period_arr attributes
  if (VD->hasAttr<EjitPeriodAttr>() || VD->hasAttr<EjitPeriodArrAttr>()) {
    S.Diag(AL.getLoc(), diag::err_ejit_period_conflict) << VD;
    return;
  }

  // Extract period name
  StringRef PeriodName;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, PeriodName))
    return;

  VD->addAttr(::new (S.Context) EjitPeriodArrAttr(S.Context, AL, PeriodName));
}

/// handleEjitPeriodArrIndAttr - Process the ejit_period_arr_ind(name) attribute.
/// Checks:
///   1. Applies only to ParmVarDecl
///   2. Parameter type must be integer
///   3. At most 4 such parameters per function
void handleEjitPeriodArrIndAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *PVD = dyn_cast<ParmVarDecl>(D);
  if (!PVD) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "function parameters";
    return;
  }

  // Parameter type must be integer
  QualType PT = PVD->getType();
  if (!PT->isIntegerType()) {
    S.Diag(AL.getLoc(), diag::err_ejit_period_arr_ind_invalid_type) << PVD;
    return;
  }

  // Max count check (4 per function) is deferred to
  // checkEjitPeriodArrIndLimit() in ActOnFunctionDeclarator because the
  // FunctionDecl is not yet set as ParmVarDecl DeclContext during parsing.

  // Extract period name
  StringRef PeriodName;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, PeriodName))
    return;

  PVD->addAttr(::new (S.Context)
      EjitPeriodArrIndAttr(S.Context, AL, PeriodName));
}

void handleEjitBoundPtrAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *PVD = dyn_cast<ParmVarDecl>(D);
  if (!PVD) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "function parameters";
    return;
  }

  QualType PT = PVD->getType();
  QualType Pointee;
  if (PT->isPointerType())
    Pointee = PT->getPointeeType();
  if (Pointee.isNull() || !Pointee->isObjectType() ||
      Pointee->isIncompleteType()) {
    S.Diag(AL.getLoc(), diag::err_ejit_bound_ptr_invalid_type) << PVD;
    return;
  }

  StringRef PeriodName;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, PeriodName))
    return;
  PVD->addAttr(::new (S.Context) EjitBoundPtrAttr(S.Context, AL, PeriodName));
}

/// handleEjitEntryAttr - Process the ejit_entry attribute.
/// Checks:
///   1. Applies only to FunctionDecl
///   2. Function must not be recursive
void handleEjitEntryAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "functions";
    return;
  }

  // Check for recursive function (only when body is available)
  if (FD->hasBody()) {
    RecursiveCallVisitor Visitor(FD);
    Visitor.TraverseStmt(FD->getBody());
    if (Visitor.FoundRecursiveCall) {
      S.Diag(AL.getLoc(), diag::err_ejit_entry_recursive) << FD;
      return;
    }
  }

  D->addAttr(::new (S.Context) EjitEntryAttr(S.Context, AL));
}

/// handleEjitPeriodLcAttr - Process the ejit_period_lc(name) attribute.
/// Checks:
///   1. Applies only to FunctionDecl
///
/// The "must have a matching ejit_period_arr_ind(name) parameter" check is
/// NOT here: at handler time only the CURRENT declaration's parameters exist,
/// and the arr_ind may legitimately live on an earlier declaration's
/// parameter (propagated by mergeParamDeclAttributes during the merge). The
/// check runs after the merge instead -- see checkEjitPeriodLcIndex.
void handleEjitPeriodLcAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
        << AL << AL.isRegularKeywordAttribute() << "functions";
    return;
  }

  // Extract period name
  StringRef PeriodName;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, PeriodName))
    return;

  D->addAttr(::new (S.Context) EjitPeriodLcAttr(S.Context, AL, PeriodName));
}

/// checkEjitPeriodArrIndLimit - Enforce the limit of at most 4
/// ejit_period_arr_ind parameters per function. Called from
/// ActOnFunctionDeclarator after all parameter attributes have been processed.
void checkEjitPeriodArrIndLimit(Sema &S, const FunctionDecl *FD) {
  if (!FD)
    return;

  unsigned Count = 0;
  const ParmVarDecl *OverflowPVD = nullptr;
  for (const ParmVarDecl *P : FD->parameters()) {
    if (P->hasAttr<EjitPeriodArrIndAttr>()) {
      Count++;
      if (Count > MAX_PERIOD_ARR_IND_PARAMS)
        OverflowPVD = P;
    }
  }

  if (Count > MAX_PERIOD_ARR_IND_PARAMS && OverflowPVD) {
    // Get the attribute location from the overflow parameter
    if (auto *A = OverflowPVD->getAttr<EjitPeriodArrIndAttr>()) {
      S.Diag(A->getLocation(), diag::err_ejit_period_arr_ind_too_many)
          << FD << Count;
    }
  }
}

void checkEjitBoundPtrIndex(Sema &S, const FunctionDecl *FD) {
  if (!FD)
    return;

  unsigned BoundCount = 0;
  const EjitBoundPtrAttr *LastBound = nullptr;
  for (const ParmVarDecl *P : FD->parameters())
    if (const auto *A = P->getAttr<EjitBoundPtrAttr>()) {
      ++BoundCount;
      LastBound = A;
    }

  if (BoundCount > MAX_BOUND_PTR_PARAMS && LastBound) {
    S.Diag(LastBound->getLocation(), diag::err_ejit_bound_ptr_too_many)
        << FD << BoundCount;
    return;
  }

  if (!BoundCount)
    return;

  for (const ParmVarDecl *P : FD->parameters()) {
    const auto *Bound = P->getAttr<EjitBoundPtrAttr>();
    if (!Bound)
      continue;
    unsigned MatchingDims = 0;
    for (const ParmVarDecl *DimP : FD->parameters())
      if (const auto *D = DimP->getAttr<EjitPeriodArrIndAttr>())
        if (D->getPeriodName() == Bound->getPeriodName())
          ++MatchingDims;
    if (MatchingDims != 1)
      S.Diag(Bound->getLocation(), diag::err_ejit_bound_ptr_missing_dim)
          << Bound->getPeriodName();
  }
}

/// checkEjitEntryLcConflict - ejit_entry and ejit_period_lc are mutually
/// exclusive on one function. PASS3 (EJitWrapperGen) replaces an entry's body
/// with a single-function dispatch wrapper, and PASS4 (EJitPeriodHandler)
/// inserts lifecycle guards at a lifecycle function's entry and returns; the
/// two rewrites would both claim the body. Reject the combination at the
/// source instead of letting the AOT pipeline fight over the function.
///
/// Called twice per declaration:
///  * from ActOnFunctionDeclarator after ProcessDeclAttributes (AfterMerge ==
///    false), where the check is independent of the source order of the two
///    attributes;
///  * from CheckFunctionDeclaration after MergeFunctionDecl (AfterMerge ==
///    true), which is the only point that sees a combination assembled from
///    attributes written on DIFFERENT declarations. There the check fires
///    only when exactly one attribute was inherited from an earlier
///    declaration: if both were written on this declarator the first call
///    already diagnosed the pair, and if both are inherited some earlier
///    declaration already was the first to carry both.
/// reportEjitEntryLcConflict - Emit the conflict diagnostic: the error at
/// \p ErrorLoc, the "conflicting attribute is here" note at \p NoteLoc.
/// Split out of checkEjitEntryLcConflict because the explicit-instantiation
/// site (ActOnExplicitInstantiation) knows which attribute ITS declarator
/// wrote and anchors the error there.
void reportEjitEntryLcConflict(Sema &S, const FunctionDecl *FD,
                               SourceLocation ErrorLoc,
                               SourceLocation NoteLoc) {
  S.Diag(ErrorLoc, diag::err_ejit_entry_lc_conflict) << FD;
  S.Diag(NoteLoc, diag::note_conflicting_attribute);
}

void checkEjitEntryLcConflict(Sema &S, FunctionDecl *FD, bool AfterMerge) {
  if (!FD)
    return;
  const EjitEntryAttr *EA = FD->getAttr<EjitEntryAttr>();
  const EjitPeriodLcAttr *LA = FD->getAttr<EjitPeriodLcAttr>();
  if (!EA || !LA)
    return;
  if (AfterMerge) {
    if (EA->isInherited() == LA->isInherited())
      return;
    // If the immediate previous declaration already carried both, the
    // conflict was diagnosed there; repeating one attribute on a later
    // redeclaration must not re-fire (attributes written after a definition
    // are rejected by clang's own redeclaration rules before they reach
    // this merge).
    const FunctionDecl *Prev = FD->getPreviousDecl();
    if (Prev && Prev->hasAttr<EjitEntryAttr>() &&
        Prev->hasAttr<EjitPeriodLcAttr>())
      return;
    // Instantiations reproduce the pattern's attribute pair; the pattern
    // declaration was diagnosed already.
    if (FD->isTemplateInstantiation())
      return;
    // Exactly one attribute came from an earlier declaration: point the
    // error at the attribute written on THIS declaration and the note at
    // the inherited one, not the other way around.
    if (EA->isInherited()) {
      reportEjitEntryLcConflict(S, FD, LA->getLocation(), EA->getLocation());
      return;
    }
    reportEjitEntryLcConflict(S, FD, EA->getLocation(), LA->getLocation());
    return;
  }
  reportEjitEntryLcConflict(S, FD, LA->getLocation(), EA->getLocation());
}

/// One attribute's no-index check: does the (merged) parameter list carry an
/// ejit_period_arr_ind with the same period name? Diagnose at \p LA if not.
static void checkEjitPeriodLcIndexForAttr(Sema &S, const FunctionDecl *FD,
                                          const EjitPeriodLcAttr *LA) {
  StringRef PeriodName = LA->getPeriodName();
  for (const ParmVarDecl *P : FD->parameters()) {
    if (auto *IdxAttr = P->getAttr<EjitPeriodArrIndAttr>())
      if (IdxAttr->getPeriodName() == PeriodName)
        return;
  }
  S.Diag(LA->getLocation(), diag::err_ejit_period_lc_no_index) << PeriodName;
}

/// checkEjitPeriodLcIndex - Every written (non-inherited) ejit_period_lc must
/// name a period that a parameter of the MERGED function carries
/// ejit_period_arr_ind for. Called from CheckFunctionDeclaration after
/// MergeFunctionDecl: at attribute-handling time only the current
/// declaration's parameters exist, and an arr_ind written on an earlier
/// declaration's parameter is only propagated to this declaration's
/// parameters by the merge (mergeParamDeclAttributes) -- checking before it
/// would reject a valid redeclaration. Inherited lc attributes were checked
/// on the declaration that wrote them, and template instantiations reproduce
/// a pattern that was checked there.
///
/// The arr_ind must therefore be visible at or BEFORE the declaration that
/// writes the lc: parameter attributes propagate forward only, so an arr_ind
/// on a LATER declaration never reaches the declaration carrying the lc.
void checkEjitPeriodLcIndex(Sema &S, const FunctionDecl *FD) {
  if (!FD || FD->isTemplateInstantiation())
    return;
  for (const EjitPeriodLcAttr *LA : FD->specific_attrs<EjitPeriodLcAttr>()) {
    if (LA->isInherited())
      continue;
    checkEjitPeriodLcIndexForAttr(S, FD, LA);
  }
}

/// checkEjitPeriodLcIndexNewAttrs - Variant for ActOnExplicitInstantiation:
/// only the lc attributes the declarator wrote (those not in
/// \p PreExisting) are checked against the specialization's parameters.
/// Attributes copied from the pattern are skipped here -- the pattern's own
/// declarations were checked by checkEjitPeriodLcIndex.
void checkEjitPeriodLcIndexNewAttrs(
    Sema &S, const FunctionDecl *FD,
    ArrayRef<const EjitPeriodLcAttr *> PreExisting) {
  if (!FD)
    return;
  for (const EjitPeriodLcAttr *LA : FD->specific_attrs<EjitPeriodLcAttr>()) {
    if (llvm::is_contained(PreExisting, LA))
      continue;
    checkEjitPeriodLcIndexForAttr(S, FD, LA);
  }
}

/// checkEjitAlwaysInlineConflict - An ejit_entry / ejit_period_lc function
/// must stay out-of-line: CodeGen and PASS3 mark it noinline so it survives
/// the LTO inliner for PASS1 (bitcode extraction), PASS3 (wrapper), and
/// PASS4 (lifecycle). A user-written always_inline would produce illegal IR
/// ("'noinline and alwaysinline' are incompatible"), which the verifier
/// aborts on. Warn and drop the always_inline so ejit semantics win.
/// Called from ActOnFunctionDeclarator after ProcessDeclAttributes, so the
/// check is independent of the source order of the two attributes.
void checkEjitAlwaysInlineConflict(Sema &S, FunctionDecl *FD) {
  if (!FD)
    return;
  bool IsEntry = FD->hasAttr<EjitEntryAttr>();
  bool IsLc = FD->hasAttr<EjitPeriodLcAttr>();
  if (!IsEntry && !IsLc)
    return;
  if (AlwaysInlineAttr *AI = FD->getAttr<AlwaysInlineAttr>()) {
    S.Diag(AI->getLocation(), diag::warn_ejit_always_inline_conflict)
        << (IsEntry ? "ejit_entry" : "ejit_period_lc");
    S.Diag(IsEntry ? FD->getAttr<EjitEntryAttr>()->getLocation()
                   : FD->getAttr<EjitPeriodLcAttr>()->getLocation(),
           diag::note_conflicting_attribute);
    FD->dropAttr<AlwaysInlineAttr>();
  }
}

/// checkEjitAttrMissingOnDefinition - A definition that inherits ejit_entry /
/// ejit_period_lc from a prior declaration (e.g. a header prototype) but does
/// not repeat the attribute itself is NOT JIT-specialized: warn and drop the
/// inherited attribute so CodeGen emits no !ejit.metadata and the
/// AOT/runtime pipelines skip the function. An attribute written on the
/// definition suppresses the inherited clone (DeclHasAttr in
/// mergeDeclAttribute), so isInherited() distinguishes the two cases.
/// Called from CheckFunctionDeclaration after MergeFunctionDecl and from
/// ActOnStartOfFunctionDef (for MSVC delayed template bodies).
void checkEjitAttrMissingOnDefinition(Sema &S, FunctionDecl *FD) {
  // Implicit and explicit instantiations reproduce the pattern definition
  // (whose attribute was already dropped, so the inherited clone here is the
  // only copy). The mismatch is diagnosed once, at the pattern definition;
  // instantiations are handled silently.
  bool Warn = !FD->isTemplateInstantiation();

  if (const EjitEntryAttr *EA = FD->getAttr<EjitEntryAttr>()) {
    if (EA->isInherited()) {
      if (Warn) {
        S.Diag(FD->getLocation(), diag::warn_ejit_attr_missing_on_definition)
            << FD << "ejit_entry";
        S.Diag(EA->getLocation(), diag::note_ejit_attr_declared_here)
            << "ejit_entry";
      }
      FD->dropAttr<EjitEntryAttr>();
    }
  }

  for (const EjitPeriodLcAttr *LA : FD->specific_attrs<EjitPeriodLcAttr>()) {
    if (LA->isInherited()) {
      if (Warn) {
        S.Diag(FD->getLocation(), diag::warn_ejit_attr_missing_on_definition)
            << FD << "ejit_period_lc";
        S.Diag(LA->getLocation(), diag::note_ejit_attr_declared_here)
            << "ejit_period_lc";
      }
      FD->dropAttrs<EjitPeriodLcAttr>();
      break;
    }
  }
}

/// If \p E is an lvalue that writes an ejit_may_const field, return that field
/// and set \p BaseVar to the underlying variable (if any). Strips parentheses
/// and implicit casts, and walks through array-subscript / member chains to
/// find the innermost variable (e.g. g_cells[i].a -> g_cells, p->a -> p,
/// s.inner.a -> s).
static const FieldDecl *findMayConstWriteTarget(Expr *E,
                                                const VarDecl *&BaseVar) {
  BaseVar = nullptr;
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();
  auto *ME = dyn_cast<MemberExpr>(E);
  if (!ME)
    return nullptr;

  auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
  if (!FD || !FD->hasAttr<EjitMayConstAttr>())
    return nullptr;

  // Walk the base of the MemberExpr to find the underlying variable.
  Expr *Base = ME->getBase()->IgnoreParenImpCasts();
  while (true) {
    if (auto *ASE = dyn_cast<ArraySubscriptExpr>(Base)) {
      Base = ASE->getBase()->IgnoreParenImpCasts();
    } else if (auto *InnerME = dyn_cast<MemberExpr>(Base)) {
      Base = InnerME->getBase()->IgnoreParenImpCasts();
    } else {
      break;
    }
  }
  if (auto *DRE = dyn_cast<DeclRefExpr>(Base))
    if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      BaseVar = VD;

  // The EjitMayConstAttr lives on the FieldDecl and applies to every struct
  // instance.  Suppress the warning when the base is a value type
  // (not pointer/reference) that has a visible definition in this TU and no
  // period annotation: such objects are local copies or plain globals that
  // do not alias period data.  Pointers and references are kept because they
  // may point to period data even though the variable itself lacks a period
  // annotation, and declaration-only externs are kept because the definition
  // in another TU may carry the attribute.  (getDefinition covers real
  // definitions, getActingDefinition covers C tentative definitions.)
  if (BaseVar &&
      !BaseVar->hasAttr<EjitPeriodArrAttr>() &&
      !BaseVar->hasAttr<EjitPeriodAttr>() &&
      !BaseVar->getType()->isPointerType() &&
      !BaseVar->getType()->isReferenceType() &&
      (BaseVar->getDefinition() || BaseVar->getActingDefinition()))
    return nullptr;

  return FD;
}

namespace {

/// Visitor that locates writes to ejit_may_const fields within a function body
/// and non-const pointers/references escaping to them.
class EjitMayConstWriteVisitor
    : public RecursiveASTVisitor<EjitMayConstWriteVisitor> {
  Sema &S;
  /// Return type of the innermost function whose body is being traversed.
  /// Starts as the checked function's return type and is updated when
  /// descending into lambda bodies, so a return inside a lambda is checked
  /// against the lambda's return type, not the enclosing function's.
  QualType CurReturnType;
  /// Whether warn_ejit_may_const_addr_without_const is enabled for this
  /// function. The address-of checks below are skipped when it is off
  /// (its default) to avoid wasted traversal work.
  bool AddrOfEnabled;

public:
  EjitMayConstWriteVisitor(Sema &S, const FunctionDecl *CurFD,
                           bool AddrOfEnabled)
      : S(S), CurReturnType(CurFD->getReturnType()),
        AddrOfEnabled(AddrOfEnabled) {}

  bool TraverseLambdaExpr(LambdaExpr *LE) {
    QualType Saved = CurReturnType;
    if (const auto *CallOp = LE->getCallOperator())
      CurReturnType = CallOp->getReturnType();
    bool Result =
        RecursiveASTVisitor<EjitMayConstWriteVisitor>::TraverseLambdaExpr(LE);
    CurReturnType = Saved;
    return Result;
  }

  bool TraverseFunctionDecl(FunctionDecl *Nested) {
    // Keep return checks inside nested functions (methods of local classes)
    // tied to their own return type, not the enclosing function's.
    QualType Saved = CurReturnType;
    CurReturnType = Nested->getReturnType();
    bool Result =
        RecursiveASTVisitor<EjitMayConstWriteVisitor>::TraverseFunctionDecl(
            Nested);
    CurReturnType = Saved;
    return Result;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->isAssignmentOp() || BO->isCompoundAssignmentOp()) {
      checkWrite(BO->getLHS(), BO->getOperatorLoc());
      // Also check RHS for &may_const_field assigned to non-const pointer.
      if (AddrOfEnabled && BO->getOpcode() == BO_Assign)
        checkAddrOfExpr(BO->getRHS(), BO->getLHS()->getType(),
                        BO->getOperatorLoc());
    }
    return true;
  }

  bool VisitDeclStmt(DeclStmt *DS) {
    if (!AddrOfEnabled)
      return true;
    for (auto *D : DS->decls()) {
      auto *VD = dyn_cast<VarDecl>(D);
      if (!VD || !VD->getInit())
        continue;
      checkAddrOfExpr(VD->getInit(), VD->getType(), VD->getLocation());
    }
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (!AddrOfEnabled)
      return true;
    // A call argument bound to a non-const pointer/reference parameter is an
    // escape of the same kind as an assignment: the callee can write through
    // it.  Calls into ejit_period_lc functions are skipped: those are
    // sanctioned to touch period data.  Variadic arguments have no parameter
    // type and are not checked.
    const FunctionDecl *Callee =
        dyn_cast_or_null<FunctionDecl>(CE->getCalleeDecl());
    if (Callee && Callee->hasAttr<EjitPeriodLcAttr>())
      return true;
    const auto *FPT = CE->getCallee()->getType()->getAs<FunctionProtoType>();
    if (!FPT && Callee)
      FPT = Callee->getType()->getAs<FunctionProtoType>();
    if (!FPT)
      return true;
    unsigned NumParams = FPT->getNumParams();
    for (unsigned I = 0; I < CE->getNumArgs() && I < NumParams; ++I)
      checkAddrOfExpr(CE->getArg(I), FPT->getParamType(I),
                      CE->getArg(I)->getBeginLoc());
    return true;
  }

  bool VisitReturnStmt(ReturnStmt *RS) {
    if (!AddrOfEnabled)
      return true;
    if (Expr *RetVal = RS->getRetValue())
      checkAddrOfExpr(RetVal, CurReturnType, RS->getReturnLoc());
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->isIncrementDecrementOp())
      checkWrite(UO->getSubExpr(), UO->getOperatorLoc());
    return true;
  }

private:
  /// If \p Target designates an ejit_may_const field, emit the warning.
  void checkWrite(Expr *Target, SourceLocation Loc) {
    const VarDecl *BaseVar = nullptr;
    const FieldDecl *MayConstField = findMayConstWriteTarget(Target, BaseVar);
    if (!MayConstField)
      return;
    emitMayConstFieldDiag(diag::warn_ejit_may_const_modified_without_lc,
                          MayConstField, BaseVar, Loc);
  }

  /// Emit a may-const-field diagnostic for \p FD.
  /// %0 = the may_const field; %1 = the variable that holds the struct
  /// (falls back to the field's parent record when no base variable is
  /// identifiable, e.g. writes through unusual rvalue bases).
  void emitMayConstFieldDiag(unsigned DiagID, const FieldDecl *FD,
                             const VarDecl *BaseVar, SourceLocation Loc) {
    if (BaseVar)
      S.Diag(Loc, DiagID) << FD << BaseVar;
    else
      S.Diag(Loc, DiagID) << FD << FD->getParent();
  }

  /// Check if \p E escapes a may_const field as a non-const pointer or
  /// reference.  Checked at consumer sites: VarDecl initializers,
  /// plain-assignment RHS, call arguments, and return values.
  /// \p DestType is the type of the destination (VarDecl type, LHS of
  /// assignment, parameter type, return type) — if its pointee/referent is
  /// const-qualified, the escape is safe and no warning is emitted.
  ///
  /// Uses IgnoreParenCasts() (not IgnoreParenImpCasts()) to see through
  /// explicit casts like (int*)&field, which would otherwise hide the
  /// underlying address-of operation.  A single-element braced initializer
  /// (int *p{&field};) is unwrapped too.
  void checkAddrOfExpr(Expr *E, QualType DestType, SourceLocation Loc) {
    if (!AddrOfEnabled || !E)
      return;

    // If the destination is a const-qualified pointer or reference, it's
    // safe — it cannot be used to write the field.
    if ((DestType->isPointerType() || DestType->isReferenceType()) &&
        DestType->getPointeeType().isConstQualified())
      return;

    // Walk through parens and all casts (implicit + explicit) to find the
    // underlying & operator.  The const check is done on DestType (final
    // type after all casts), so stripping intermediate casts is safe.
    Expr *Inner = E->IgnoreParenCasts();
    if (auto *ILE = dyn_cast<InitListExpr>(Inner))
      if (ILE->getNumInits() == 1)
        Inner = ILE->getInit(0)->IgnoreParenCasts();

    Expr *Target = nullptr;
    if (DestType->isReferenceType()) {
      // A reference binding has no address-of node in the AST: the
      // initializer is the referent itself, with the & implicit.  A
      // non-const reference to a may_const field is the same escape as a
      // non-const pointer.
      Target = Inner;
    } else {
      auto *UO = dyn_cast<UnaryOperator>(Inner);
      if (!UO || UO->getOpcode() != UO_AddrOf)
        return;
      Target = UO->getSubExpr();
    }

    const VarDecl *BaseVar = nullptr;
    const FieldDecl *FD = findMayConstWriteTarget(Target, BaseVar);
    if (!FD)
      return;

    emitMayConstFieldDiag(diag::warn_ejit_may_const_addr_without_const, FD,
                          BaseVar, Loc);
  }
};

} // anonymous namespace

/// checkEjitMayConstWrites - Warn when a function that is NOT marked
/// ejit_period_lc writes to an ejit_may_const field. Called from
/// ActOnFinishFunctionBody after the function body is parsed.
void checkEjitMayConstWrites(Sema &S, const FunctionDecl *FD, Stmt *Body) {
  if (!FD || !Body || FD->isInvalidDecl() || FD->isDependentContext())
    return;

  // ejit_period_lc functions are sanctioned to modify time-window data: the
  // compiler inserts ejit_deactivate/activate around them, so writes there are
  // safe and must not be flagged.
  if (FD->hasAttr<EjitPeriodLcAttr>())
    return;

  // Methods of local classes (including lambda call operators) are checked
  // as part of the enclosing function's traversal, which descends into their
  // bodies; checking them separately (ActOnFinishFunctionBody also runs for
  // them) would double-diagnose everything inside them.
  if (const auto *MD = dyn_cast<CXXMethodDecl>(FD))
    if (MD->getParent()->isLocalClass())
      return;

  // Skip the traversal entirely when BOTH diagnostics are disabled
  // (e.g. -Wno-embedded-jit plus the addr-of warning off, its default),
  // matching the AnalysisBasedWarnings pattern. The write warning is
  // default-on, so the visitor normally runs; the addr-of checks inside
  // it are gated separately on AddrOfEnabled.
  bool AddrOfEnabled = !S.getDiagnostics().isIgnored(
      diag::warn_ejit_may_const_addr_without_const, FD->getLocation());
  if (S.getDiagnostics().isIgnored(
          diag::warn_ejit_may_const_modified_without_lc, FD->getLocation()) &&
      !AddrOfEnabled)
    return;

  EjitMayConstWriteVisitor Visitor(S, FD, AddrOfEnabled);
  Visitor.TraverseStmt(Body);
}

namespace {

/// Visitor that collects DeclRefExprs to ejit_period_arr globals within a
/// function body.
class EjitPeriodArrRefVisitor
    : public RecursiveASTVisitor<EjitPeriodArrRefVisitor> {
  SmallVector<const DeclRefExpr *, 4> Refs;

public:
  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      if (VD->hasAttr<EjitPeriodArrAttr>())
        Refs.push_back(DRE);
    return true;
  }

  // The operand of sizeof/alignof is unevaluated: it needs no runtime
  // period data, so a reference there is not a real dependency
  // (e.g. sizeof(g_cells)).  Exception: sizeof/alignof on a VLA evaluates
  // the size expression at runtime, so both the operand and its VLA type
  // (which holds the size expression) are real dependencies and must be
  // traversed.
  bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *E) {
    if (E->isArgumentType())
      return TraverseType(E->getArgumentType());
    if (E->getKind() == UETT_SizeOf || E->getKind() == UETT_AlignOf) {
      if (E->getTypeOfArgument()->isVariableArrayType()) {
        if (!TraverseStmt(E->getArgumentExpr()))
          return false;
        return TraverseType(E->getTypeOfArgument());
      }
    }
    return true;
  }

  // decltype operands are unevaluated; skip both the type and TypeLoc
  // traversal paths (e.g. the declared type of `decltype(g_cells) copy;`).
  bool TraverseDecltypeType(DecltypeType *T) { return true; }
  bool TraverseDecltypeTypeLoc(DecltypeTypeLoc TL) { return true; }

  // typeid operands are unevaluated unless the operand is a potentially
  // evaluated polymorphic glvalue.
  bool TraverseCXXTypeidExpr(CXXTypeidExpr *E) {
    if (!E->isPotentiallyEvaluated())
      return true;
    return RecursiveASTVisitor<EjitPeriodArrRefVisitor>::TraverseCXXTypeidExpr(
        E);
  }

  // noexcept operands are unevaluated.
  bool TraverseCXXNoexceptExpr(CXXNoexceptExpr *E) { return true; }

  const SmallVectorImpl<const DeclRefExpr *> &getRefs() const { return Refs; }
};

} // anonymous namespace

/// checkEjitUndeclaredPeriodDeps - Warn when an ejit_entry function references
/// an ejit_period_arr global that is not declared as a dependency via an
/// ejit_period_arr_ind parameter. Called from ActOnFinishFunctionBody after
/// the function body is parsed.
///
/// Replaces the former AOT-pass check (EJitAotModulePass) that printed the
/// same warning to stderr: Sema can point at the exact reference site and the
/// diagnostic is controlled by -Wembedded-jit-undeclared-period-dep.
///
/// Note: only the entry function's own body is checked (pre-inline state).
/// A dependency hidden in a callee that is later inlined is not detected
/// here; declare it explicitly on the entry function.
void checkEjitUndeclaredPeriodDeps(Sema &S, const FunctionDecl *FD,
                                   Stmt *Body) {
  if (!FD || !Body || FD->isInvalidDecl() || FD->isDependentContext())
    return;

  // Only ejit_entry functions declare dependencies, mirroring the former
  // AOT-pass check which only looked at TAG_EJIT_ENTRY metadata.
  if (!FD->hasAttr<EjitEntryAttr>())
    return;

  // Skip the traversal entirely when the warning is disabled
  // (off by default via DefaultIgnore; enabled with
  // -Wembedded-jit-undeclared-period-dep), matching the
  // AnalysisBasedWarnings pattern.
  if (S.getDiagnostics().isIgnored(diag::warn_ejit_undeclared_period_dep,
                                   FD->getLocation()))
    return;

  // Collect declared period names from ejit_period_arr_ind parameters.
  SmallVector<StringRef, 4> Declared;
  for (const ParmVarDecl *P : FD->parameters())
    if (auto *IdxAttr = P->getAttr<EjitPeriodArrIndAttr>())
      Declared.push_back(IdxAttr->getPeriodName());

  EjitPeriodArrRefVisitor Visitor;
  Visitor.TraverseStmt(Body);

  // Warn once per undeclared period, at the first reference site, with a
  // note pointing at the period array definition.
  llvm::SmallSet<StringRef, 4> Warned;
  for (const DeclRefExpr *Ref : Visitor.getRefs()) {
    const auto *VD = cast<VarDecl>(Ref->getDecl());
    StringRef PeriodName = VD->getAttr<EjitPeriodArrAttr>()->getPeriodName();
    if (is_contained(Declared, PeriodName) || !Warned.insert(PeriodName).second)
      continue;
    S.Diag(Ref->getLocation(), diag::warn_ejit_undeclared_period_dep)
        << FD << PeriodName;
    // Anchor the note at the definition when visible; an earlier
    // declaration-only extern is not "defined here".
    const VarDecl *DefVD = VD->getDefinition();
    S.Diag(DefVD ? DefVD->getLocation() : VD->getLocation(),
           diag::note_ejit_period_arr_defined_here)
        << PeriodName;
  }
}
