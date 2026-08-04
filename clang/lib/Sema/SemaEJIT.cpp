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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"

using llvm::ejit::MAX_PERIOD_ARR_IND_PARAMS;

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
      if (Size > 100) {
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
///   2. Must have a corresponding ejit_period_arr_ind(name) parameter
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

  // Check for matching ejit_period_arr_ind parameter
  bool HasMatchingIdx = false;
  for (const ParmVarDecl *P : FD->parameters()) {
    if (auto *IdxAttr = P->getAttr<EjitPeriodArrIndAttr>()) {
      if (IdxAttr->getPeriodName() == PeriodName) {
        HasMatchingIdx = true;
        break;
      }
    }
  }

  if (!HasMatchingIdx) {
    S.Diag(AL.getLoc(), diag::err_ejit_period_lc_no_index) << PeriodName;
    return;
  }

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

  if (Count > 4 && OverflowPVD) {
    // Get the attribute location from the overflow parameter
    if (auto *A = OverflowPVD->getAttr<EjitPeriodArrIndAttr>()) {
      S.Diag(A->getLocation(), diag::err_ejit_period_arr_ind_too_many)
          << FD << Count;
    }
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

  return FD;
}

namespace {

/// Visitor that locates writes to ejit_may_const fields within a function body.
class EjitMayConstWriteVisitor
    : public RecursiveASTVisitor<EjitMayConstWriteVisitor> {
  Sema &S;

public:
  explicit EjitMayConstWriteVisitor(Sema &S) : S(S) {}

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->isAssignmentOp() || BO->isCompoundAssignmentOp())
      checkWrite(BO->getLHS(), BO->getOperatorLoc());
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
    // %0 = the may_const field; %1 = the variable that holds the struct.
    // Falls back to the field's parent record when no base variable is
    // identifiable (e.g. writes through unusual rvalue bases).
    if (BaseVar)
      S.Diag(Loc, diag::warn_ejit_may_const_modified_without_lc)
          << MayConstField << BaseVar;
    else
      S.Diag(Loc, diag::warn_ejit_may_const_modified_without_lc)
          << MayConstField << MayConstField->getParent();
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

  // Skip the traversal entirely when the warning is disabled
  // (e.g. -Wno-embedded-jit), matching the AnalysisBasedWarnings pattern.
  if (S.getDiagnostics().isIgnored(
          diag::warn_ejit_may_const_modified_without_lc, FD->getLocation()))
    return;

  EjitMayConstWriteVisitor Visitor(S);
  Visitor.TraverseStmt(Body);
}
