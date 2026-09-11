//===-- EJitSmallTable.cpp - Shared small-table specialization ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Planning copies the authorized scalar values out of the borrowed source
// region; the pass then rewrites authorized may_const loads in the entry to
// loads from the emitted table with the real dynamic index values.
//
// The emitted table global is mutable data with a fixed address. It is never
// marked `constant` and no load from it is ever folded, because the runtime may
// publish a later row value into a not-yet-ready row without recompiling
// (EJIT_SMALL_TABLE_SPEC.md §6.5). Address materialization is direct
// (`adrp`+`add` on AArch64) because the global is `dso_local`.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSmallTable.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-small-table"

namespace {

/// Metadata recording the exact folded constants of the admission contract
/// (§6.6 step 1), so the runtime can validate a later member in its cold path.
constexpr const char *MD_SMALL_TABLE_CONTRACT = "ejit.smalltable.contract";
/// Metadata recording one emitted column's shape and readiness.
constexpr const char *MD_SMALL_TABLE_COLUMN = "ejit.smalltable.column";
/// Positive provenance tag on a synthesized table read (§9): it names the
/// planned entry and source global but is never `!ejit.may_const`, so a later
/// extraction round can tell it apart from an original authorized load.
constexpr const char *MD_SMALL_TABLE_LOAD = "ejit.smalltable.load";

/// The source array shape read from the IR. Strides and extents come from the
/// declared type, so the emitted table covers exactly the index space the
/// source GEP can address.
struct SourceShape {
  GlobalVariable *GV = nullptr;
  Type *ElementTy = nullptr;
  uint64_t ElementBytes = 0;
  SmallVector<uint64_t, 4> DimStrides;
  SmallVector<uint64_t, 4> DimExtents;
};

/// Peel the declared dimension levels off \p VarName's value type and record
/// the IR stride/extent of each level. Every plan dimension must match one
/// array level, in order, or the plan cannot describe this global.
std::optional<SourceShape> deriveSourceShape(Module &M, const DataLayout &DL,
                                             StringRef VarName,
                                             ArrayRef<EJitSmallTableDim> Dims,
                                             std::string *Error) {
  auto Fail = [&](const Twine &Msg) -> std::optional<SourceShape> {
    if (Error)
      *Error = Msg.str();
    return std::nullopt;
  };

  GlobalVariable *GV = M.getNamedGlobal(VarName);
  if (!GV)
    return Fail("source global not found: " + VarName);

  Type *Ty = GV->getValueType();
  if (!Ty->isSized())
    return Fail("source global is unsized: " + VarName);

  SourceShape Shape;
  Shape.GV = GV;
  for (const EJitSmallTableDim &Dim : Dims) {
    auto *AT = dyn_cast<ArrayType>(Ty);
    if (!AT)
      return Fail("source global has fewer array levels than plan dimensions");
    const uint64_t LevelExtent = AT->getNumElements();
    if (Dim.kind == EJitSmallTableDim::Kind::ModuloArgument) {
      // `urem(arg, modulus)` proves the index range, so this axis may be
      // compacted to exactly that range, but the compacted table must stay
      // inside the declared array level it indexes.
      if (Dim.modulus == 0 || Dim.extent != Dim.modulus)
        return Fail("modulo dimension modulus and extent disagree");
      if (Dim.extent > LevelExtent)
        return Fail("plan dimension extent exceeds the source array level");
    } else if (Dim.extent != LevelExtent) {
      return Fail("plan dimension extent does not match the source array");
    }
    TypeSize Stride = DL.getTypeAllocSize(AT->getElementType());
    if (Stride.isScalable())
      return Fail("scalable source element is not supported");
    Shape.DimStrides.push_back(Stride.getFixedValue());
    Shape.DimExtents.push_back(LevelExtent);
    Ty = AT->getElementType();
  }

  TypeSize ElemSize = DL.getTypeAllocSize(Ty);
  if (ElemSize.isScalable() || ElemSize.getFixedValue() == 0)
    return Fail("source element has no fixed size");
  Shape.ElementTy = Ty;
  Shape.ElementBytes = ElemSize.getFixedValue();
  return Shape;
}

/// True when the load carries its own may_const marker, or the global's
/// metadata lists the field and the access fits inside it — the same two
/// authorization routes PASS6 uses.
bool isAuthorizedMayConstLoad(const LoadInst *LI, const GlobalVariable *GV,
                              uint64_t FieldOffset, const DataLayout &DL) {
  if (LI->isVolatile() || LI->isAtomic())
    return false;
  if (LI->hasMetadata(MD_EJIT_MAY_CONST))
    return true;

  MDNode *MD = GV->getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return false;
  TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
  if (AccessSize.isScalable())
    return false;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 2)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    if (!Tag || Tag->getString() != TAG_EJIT_MAY_CONST_FIELD)
      continue;
    auto *CI = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(1));
    if (!CI || CI->getZExtValue() != FieldOffset)
      continue;
    if (ejitAccessFitsMayConstField(GV, FieldOffset,
                                    AccessSize.getFixedValue(), DL))
      return true;
  }
  return false;
}

/// Descend a value through casts that preserve which parameter it is derived
/// from. The row index is built from the post-cast SSA value, so accepting a
/// truncation here does not change which value the source used.
const Value *stripDimensionCasts(const Value *V) {
  while (const auto *CI = dyn_cast<CastInst>(V)) {
    if (!isa<ZExtInst>(CI) && !isa<SExtInst>(CI) && !isa<TruncInst>(CI))
      break;
    V = CI->getOperand(0);
  }
  return V;
}

const Argument *matchArgument(const Value *V, const Function &F,
                              unsigned ArgIndex) {
  const auto *Arg = dyn_cast<Argument>(stripDimensionCasts(V));
  if (!Arg || Arg->getParent() != &F || Arg->getArgNo() != ArgIndex)
    return nullptr;
  return Arg;
}

/// Match one dynamic GEP index against the declared dimension expression. Only
/// the real parameter (or `urem(param, modulus)`) is accepted: a representative
/// or witness value must never be treated as the dimension, and `urem(mod,
/// param)` is a different (not TTI-declared) expression and is refused.
bool matchesDimExpr(const Value *Idx, const EJitSmallTableDim &Dim,
                    const Function &F) {
  const Value *V = stripDimensionCasts(Idx);
  if (Dim.kind == EJitSmallTableDim::Kind::Argument)
    return matchArgument(V, F, Dim.argIndex) != nullptr;

  const auto *BO = dyn_cast<BinaryOperator>(V);
  if (!BO || BO->getOpcode() != Instruction::URem || Dim.modulus == 0)
    return false;
  const auto *C = dyn_cast<ConstantInt>(BO->getOperand(1));
  if (!C || !C->getValue().isIntN(64) || C->getZExtValue() != Dim.modulus)
    return false;
  return matchArgument(BO->getOperand(0), F, Dim.argIndex) != nullptr;
}

/// One load whose address is a legal table row.
struct MatchedAccess {
  uint64_t FieldOffset = 0;
  uint64_t AccessSize = 0;
  SmallVector<Value *, 4> DynValues;
};

/// Flatten the pointer operand's GEP chain and match it against the declared
/// dimensions. Returns false for any shape the plan does not describe; those
/// loads keep their original semantics. \p RootMatched, when non-null, tells
/// the caller whether the load does address the planned source global at all,
/// so a refusal can be counted against the right reason.
bool matchSmallTableAddress(const LoadInst *LI, const Function &F,
                            const SourceShape &Shape,
                            ArrayRef<EJitSmallTableDim> Dims,
                            const DataLayout &DL, MatchedAccess &Out,
                            bool *RootMatched = nullptr) {
  if (RootMatched)
    *RootMatched = false;
  if (LI->isVolatile() || LI->isAtomic())
    return false;
  if (LI->getPointerAddressSpace() != 0)
    return false;

  SmallVector<const GEPOperator *, 4> Path;
  const Value *V = LI->getPointerOperand();
  const GlobalVariable *RootGV = nullptr;
  while (V) {
    V = V->stripPointerCasts();
    if (const auto *GV = dyn_cast<GlobalVariable>(V)) {
      RootGV = GV;
      break;
    }
    const auto *GEP = dyn_cast<GEPOperator>(V);
    if (!GEP)
      return false;
    Path.push_back(GEP);
    V = GEP->getPointerOperand();
  }
  if (RootGV != Shape.GV)
    return false;
  if (RootMatched)
    *RootMatched = true;

  uint64_t ConstOff = 0;
  SmallVector<std::pair<Value *, uint64_t>, 4> Dyn;
  for (auto It = Path.rbegin(); It != Path.rend(); ++It) {
    const GEPOperator *GEP = *It;
    for (auto GTI = gep_type_begin(GEP), GTE = gep_type_end(GEP); GTI != GTE;
         ++GTI) {
      if (StructType *STy = GTI.getStructTypeOrNull()) {
        const auto *CI = dyn_cast<ConstantInt>(GTI.getOperand());
        if (!CI)
          return false;
        uint64_t Off = 0;
        if (__builtin_add_overflow(
                ConstOff,
                DL.getStructLayout(STy)->getElementOffset(CI->getZExtValue())
                    .getFixedValue(),
                &Off))
          return false;
        ConstOff = Off;
        continue;
      }
      TypeSize TS = DL.getTypeAllocSize(GTI.getIndexedType());
      if (TS.isScalable())
        return false;
      const uint64_t Stride = TS.getFixedValue();
      if (const auto *CI = dyn_cast<ConstantInt>(GTI.getOperand())) {
        const int64_t Idx = CI->getSExtValue();
        if (Idx < 0)
          return false;
        uint64_t Off = 0;
        if (__builtin_mul_overflow(static_cast<uint64_t>(Idx), Stride, &Off) ||
            __builtin_add_overflow(ConstOff, Off, &Off))
          return false;
        ConstOff = Off;
        continue;
      }
      // A dynamic index outside an inbounds GEP carries no bound on the value,
      // so the linearized row index could leave the emitted table.
      if (!GEP->isInBounds())
        return false;
      Dyn.push_back({GTI.getOperand(), Stride});
    }
  }

  if (Dyn.size() != Dims.size())
    return false;

  Out.DynValues.clear();
  for (unsigned I = 0; I < Dims.size(); ++I) {
    if (Dyn[I].second != Shape.DimStrides[I])
      return false;
    if (!matchesDimExpr(Dyn[I].first, Dims[I], F))
      return false;
    Out.DynValues.push_back(Dyn[I].first);
  }

  TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
  if (AccessSize.isScalable())
    return false;
  const uint64_t Access = AccessSize.getFixedValue();
  if (Access == 0 || Shape.ElementBytes == 0)
    return false;
  // The constant part of the address must stay inside the addressed element.
  // A constant index that hops whole array elements is a different address than
  // the row the plan describes, so it must keep its original load instead of
  // being folded onto a same-offset field of the planned row.
  if (ConstOff >= Shape.ElementBytes)
    return false;
  const uint64_t FieldOffset = ConstOff;
  if (FieldOffset > Shape.ElementBytes || Access > Shape.ElementBytes - FieldOffset)
    return false;
  Out.FieldOffset = FieldOffset;
  Out.AccessSize = Access;
  return true;
}

/// Scalar types the table can represent. Everything else (pointer, aggregate,
/// vector, integer wider than 64 bits) keeps its original load.
bool supportedScalarType(Type *Ty, uint64_t &BitWidth,
                         EJitSmallTableKind &Kind) {
  if (Ty->isIntegerTy()) {
    const unsigned W = Ty->getIntegerBitWidth();
    if (W == 0 || W > 64)
      return false;
    BitWidth = W;
    Kind = EJitSmallTableKind::Integer;
    return true;
  }
  if (Ty->isFloatTy()) {
    BitWidth = 32;
    Kind = EJitSmallTableKind::Float;
    return true;
  }
  if (Ty->isDoubleTy()) {
    BitWidth = 64;
    Kind = EJitSmallTableKind::Double;
    return true;
  }
  return false;
}

/// True when \p Ty reads exactly the storage of \p Field. Besides the declared
/// scalar type this accepts a same-store-size scalar reinterpretation: the
/// optimizer routinely rewrites `bitcast (load float)` into `load i32`, and the
/// table keeps the field's own type, so the load is coerced back at the
/// replacement site. The store-size equality checked here is what makes the
/// reinterpretation bit-exact; unsupported, wider, pointer, aggregate and
/// scalable types are still refused by supportedScalarType().
bool fieldMatchesType(const EJitSmallTableField &Field, Type *Ty,
                      const DataLayout &DL) {
  uint64_t BitWidth = 0;
  EJitSmallTableKind Kind = EJitSmallTableKind::Integer;
  if (!supportedScalarType(Ty, BitWidth, Kind))
    return false;
  TypeSize Store = DL.getTypeStoreSize(Ty);
  if (Store.isScalable() || Store.getFixedValue() != Field.accessSize)
    return false;
  return true;
}

/// Reinterpret a scalar read from a table column (or a folded contract
/// constant) as the type the original load had. Both types are supported
/// scalars with the same store size, so only the view changes and no bit is
/// lost: integer views use zext/trunc for the i1 <-> i8 case, everything else
/// is a bitcast (i32 <-> float, i64 <-> double).
Value *coerceScalarToLoadType(IRBuilder<> &Builder, Value *V, Type *LoadTy) {
  Type *From = V->getType();
  if (From == LoadTy)
    return V;
  if (From->isIntegerTy() && LoadTy->isIntegerTy())
    return Builder.CreateZExtOrTrunc(V, LoadTy);
  return Builder.CreateBitCast(V, LoadTy);
}

Type *scalarTypeForField(LLVMContext &Ctx, const EJitSmallTableField &Field) {
  switch (Field.kind) {
  case EJitSmallTableKind::Integer:
    return IntegerType::get(Ctx, Field.bitWidth);
  case EJitSmallTableKind::Float:
    return Type::getFloatTy(Ctx);
  case EJitSmallTableKind::Double:
    return Type::getDoubleTy(Ctx);
  }
  llvm_unreachable("unknown small-table scalar kind");
}

/// Rebuild the bit pattern of one scalar from raw target-ordered bytes. The
/// result is the integer the target's LLVM type needs: for integers the value,
/// for float/double the APFloat bit pattern.
uint64_t readScalarBits(const uint8_t *Addr, const EJitSmallTableField &Field,
                        const DataLayout &DL) {
  uint64_t Raw = 0;
  const unsigned Bytes = static_cast<unsigned>(Field.accessSize);
  if (DL.isLittleEndian()) {
    std::memcpy(&Raw, Addr, Bytes);
  } else {
    for (unsigned I = 0; I < Bytes; ++I)
      Raw = (Raw << 8) | Addr[I];
  }
  return Raw;
}

Constant *constantFromBits(Type *Ty, const EJitSmallTableField &Field,
                           uint64_t Bits, LLVMContext &Ctx) {
  switch (Field.kind) {
  case EJitSmallTableKind::Integer:
    return ConstantInt::get(Ty, APInt(Field.bitWidth, Bits));
  case EJitSmallTableKind::Float:
    return ConstantFP::get(
        Ty, APFloat(APFloat::IEEEsingle(), APInt(32, Bits)));
  case EJitSmallTableKind::Double:
    return ConstantFP::get(Ty, APFloat(APFloat::IEEEdouble(), APInt(64, Bits)));
  }
  llvm_unreachable("unknown small-table scalar kind");
}

/// True when the emitted table global should be marked `dso_local`.
///
/// Direct binding (`adrp`+`add`, no GOT) needs a non-preemptible symbol, and
/// that is what the freestanding AArch64 product target uses (verified on the
/// emitted object). The host x86-64 JIT runs the small code model at addresses
/// above 4 GiB, where a `dso_local` global would need a 32-bit absolute
/// relocation that JITLink cannot resolve; there the symbol stays preemptible
/// so the access is PC-relative instead. This is a host-toolchain constraint,
/// not a change to the product target.
bool wantDSOLocal(const Module &M) {
  return Triple(M.getTargetTriple()).isAArch64();
}

const EJitSmallTableField *findField(ArrayRef<EJitSmallTableField> Fields,
                                     const MatchedAccess &Match,
                                     const DataLayout &DL, Type *LoadTy) {
  for (const EJitSmallTableField &Field : Fields) {
    if (Field.sourceOffset != Match.FieldOffset ||
        Field.accessSize != Match.AccessSize)
      continue;
    if (!fieldMatchesType(Field, LoadTy, DL))
      continue;
    return &Field;
  }
  return nullptr;
}

} // namespace

//===----------------------------------------------------------------------===//
// Plan queries
//===----------------------------------------------------------------------===//

uint64_t EJitSmallTablePlan::numRows() const {
  if (dims.empty())
    return 0;
  uint64_t Rows = 1;
  for (const EJitSmallTableDim &Dim : dims) {
    if (Dim.extent == 0 || Rows > std::numeric_limits<uint64_t>::max() / Dim.extent)
      return 0;
    Rows *= Dim.extent;
  }
  return Rows;
}

uint64_t EJitSmallTablePlan::tableRowStride(unsigned Dim) const {
  uint64_t Stride = 1;
  for (unsigned I = Dim + 1; I < dims.size(); ++I)
    Stride *= dims[I].extent;
  return Stride;
}

uint64_t EJitSmallTablePlan::sourceElementOffset(uint64_t Row) const {
  uint64_t Off = 0;
  for (unsigned I = dims.size(); I-- > 0;) {
    const uint64_t Extent = dims[I].extent;
    if (Extent == 0)
      return Off;
    const uint64_t Index = Row % Extent;
    Row /= Extent;
    Off += Index * sourceStrides[I];
  }
  return Off;
}

uint64_t EJitSmallTablePlan::uniformFieldCount() const {
  return static_cast<uint64_t>(
      llvm::count_if(fields, [](const EJitSmallTableField &F) {
        return F.uniformValue.has_value();
      }));
}

bool EJitSmallTablePlan::allRowsReady() const {
  return !rows.empty() &&
         llvm::all_of(rows, [](const EJitSmallTableRow &R) { return R.ready; });
}

uint64_t EJitSmallTablePlan::readyRowCount() const {
  return static_cast<uint64_t>(
      llvm::count_if(rows, [](const EJitSmallTableRow &R) { return R.ready; }));
}

bool EJitSmallTablePlan::isConsistent(std::string *Why) const {
  auto Fail = [&](const char *Msg) {
    if (Why)
      *Why = Msg;
    return false;
  };

  if (entryName.empty())
    return Fail("empty entry name");
  if (sourceVarName.empty())
    return Fail("empty source global name");
  if (dims.empty())
    return Fail("no dimensions");
  if (elementBytes == 0)
    return Fail("zero element size");
  if (sourceStrides.size() != dims.size())
    return Fail("source stride count does not match dimensions");
  if (fields.empty())
    return Fail("no authorized fields");

  const uint64_t Rows = numRows();
  if (Rows == 0 || Rows > MaxRows)
    return Fail("row count out of range");
  if (rows.size() != Rows)
    return Fail("row count does not match the declared extents");

  uint64_t Bytes = 0;
  for (const EJitSmallTableField &Field : fields) {
    if (Field.accessSize == 0 || Field.bitWidth == 0)
      return Fail("field has no access size or width");
    if (Field.accessSize > 8)
      return Fail("field access is wider than a supported scalar");
    switch (Field.kind) {
    case EJitSmallTableKind::Integer:
      if (Field.bitWidth > 64 ||
          Field.accessSize != (Field.bitWidth + 7) / 8)
        return Fail("integer field width and access size disagree");
      break;
    case EJitSmallTableKind::Float:
      if (Field.bitWidth != 32 || Field.accessSize != 4)
        return Fail("float field width and access size disagree");
      break;
    case EJitSmallTableKind::Double:
      if (Field.bitWidth != 64 || Field.accessSize != 8)
        return Fail("double field width and access size disagree");
      break;
    }
    if (Field.sourceOffset > elementBytes ||
        Field.accessSize > elementBytes - Field.sourceOffset)
      return Fail("field lies outside the source element");
    if (Field.uniformValue)
      continue;
    if (Field.columnName.empty())
      return Fail("table field has no column name");
    uint64_t ColumnBytes = 0;
    if (__builtin_mul_overflow(Rows, Field.accessSize, &ColumnBytes) ||
        __builtin_add_overflow(Bytes, ColumnBytes, &Bytes))
      return Fail("table byte count overflow");
  }
  if (Bytes > MaxTableBytes)
    return Fail("table exceeds the storage bound");

  for (const EJitSmallTableRow &Row : rows)
    if (Row.bits.size() != fields.size())
      return Fail("row width does not match the field count");
  return true;
}

//===----------------------------------------------------------------------===//
// Plan set
//===----------------------------------------------------------------------===//

void EJitSmallTablePlanSet::add(std::shared_ptr<const EJitSmallTablePlan> Plan) {
  if (!Plan)
    return;
  plans_[Plan->entryName] = std::move(Plan);
}

const EJitSmallTablePlan *EJitSmallTablePlanSet::find(StringRef EntryName) const {
  auto It = plans_.find(EntryName);
  return It == plans_.end() ? nullptr : It->second.get();
}

//===----------------------------------------------------------------------===//
// Planner
//===----------------------------------------------------------------------===//

std::optional<EJitSmallTablePlan>
EJitSmallTablePlanner::planShape(const Module &M, StringRef EntryName,
                                 StringRef SourceVarName,
                                 ArrayRef<EJitSmallTableDim> Dims,
                                 std::string &Error) {
  auto Fail = [&](const Twine &Msg) -> std::optional<EJitSmallTablePlan> {
    Error = Msg.str();
    return std::nullopt;
  };

  Module &MutM = const_cast<Module &>(M);
  const DataLayout &DL = M.getDataLayout();

  const Function *Entry = M.getFunction(EntryName);
  if (!Entry || Entry->isDeclaration())
    return Fail("entry function not found or not defined: " + EntryName);
  if (Dims.empty())
    return Fail("no dimensions declared");

  auto Shape = deriveSourceShape(MutM, DL, SourceVarName, Dims, &Error);
  if (!Shape)
    return std::nullopt;
  for (const EJitSmallTableDim &Dim : Dims)
    if (Dim.argIndex >= Entry->arg_size())
      return Fail("dimension argument index is outside the entry signature");
  // One dimension per argument: two axes reading the same parameter cannot be
  // filled independently, so the schema must not admit it.
  for (unsigned I = 0; I < Dims.size(); ++I)
    for (unsigned J = I + 1; J < Dims.size(); ++J)
      if (Dims[I].argIndex == Dims[J].argIndex)
        return Fail("two plan dimensions use the same argument");

  // Bound the declared domain before allocating anything: a mistaken extent
  // must be refused, never turned into a huge allocation or an overflowed row
  // count (which would make the row indexing below unsafe).
  uint64_t RowCount = 1;
  for (const EJitSmallTableDim &Dim : Dims) {
    if (Dim.extent == 0)
      return Fail("plan dimension extent is zero");
    if (RowCount > EJitSmallTablePlan::MaxRows / Dim.extent)
      return Fail("declared extents exceed the small-table row bound");
    RowCount *= Dim.extent;
  }

  // Discover the authorized may_const scalar fields actually loaded from the
  // source array. The plan must describe the loads that exist; a field that no
  // load names has no reason to occupy a column.
  struct Site {
    uint64_t Offset;
    uint64_t AccessSize;
    uint64_t BitWidth;
    EJitSmallTableKind Kind;
  };
  SmallVector<Site, 8> Sites;
  for (const Instruction &I : instructions(*Entry)) {
    const auto *LI = dyn_cast<LoadInst>(&I);
    if (!LI)
      continue;
    MatchedAccess Match;
    if (!matchSmallTableAddress(LI, *Entry, *Shape, Dims, DL, Match))
      continue;
    if (!isAuthorizedMayConstLoad(LI, Shape->GV, Match.FieldOffset, DL))
      continue;
    uint64_t BitWidth = 0;
    EJitSmallTableKind Kind = EJitSmallTableKind::Integer;
    if (!supportedScalarType(LI->getType(), BitWidth, Kind))
      continue;
    if (llvm::any_of(Sites, [&](const Site &S) {
          return S.Offset == Match.FieldOffset &&
                 S.AccessSize == Match.AccessSize;
        }))
      continue;
    Sites.push_back({Match.FieldOffset, Match.AccessSize, BitWidth, Kind});
  }
  if (Sites.empty())
    return Fail("no authorized may_const load matches the declared dimensions");
  llvm::sort(Sites, [](const Site &L, const Site &R) {
    if (L.Offset != R.Offset)
      return L.Offset < R.Offset;
    return L.AccessSize < R.AccessSize;
  });

  EJitSmallTablePlan Plan;
  Plan.entryName = EntryName.str();
  Plan.sourceVarName = SourceVarName.str();
  Plan.elementBytes = Shape->ElementBytes;
  Plan.dims.append(Dims.begin(), Dims.end());
  Plan.sourceStrides = Shape->DimStrides;
  for (unsigned I = 0; I < Sites.size(); ++I) {
    EJitSmallTableField Field;
    Field.sourceOffset = Sites[I].Offset;
    Field.accessSize = Sites[I].AccessSize;
    Field.bitWidth = Sites[I].BitWidth;
    Field.kind = Sites[I].Kind;
    Field.columnName = (Twine(EJitSmallTablePlan::TableGlobalPrefix) +
                        EntryName + "_c" + Twine(I))
                           .str();
    Plan.fields.push_back(std::move(Field));
  }
  Plan.rows.assign(RowCount,
                   EJitSmallTableRow{false,
                                     std::vector<uint64_t>(Plan.fields.size(), 0)});
  return Plan;
}

std::optional<EJitSmallTablePlan>
EJitSmallTablePlanner::plan(const Module &M, StringRef EntryName,
                            StringRef SourceVarName,
                            ArrayRef<EJitSmallTableDim> Dims,
                            const EJitSmallTableSource &Source,
                            ArrayRef<EJitSmallTableRowKey> AuthorizedRows,
                            ArrayRef<std::optional<uint64_t>> UniformContracts,
                            std::string &Error) {
  auto Plan = planShape(M, EntryName, SourceVarName, Dims, Error);
  if (!Plan)
    return std::nullopt;
  auto Fail = [&](const Twine &Msg) -> std::optional<EJitSmallTablePlan> {
    Error = Msg.str();
    return std::nullopt;
  };

  if (!Source.baseAddr || Source.size == 0)
    return Fail("source region is null or empty");
  if (!UniformContracts.empty() &&
      UniformContracts.size() != Plan->fields.size())
    return Fail("uniform contract count does not match the field count");

  // Fill only the rows whose member configuration is confirmed ready. A row
  // that is not authorized stays unreachable by contract (§6.5): the runtime
  // must not dispatch its logical slot to this code before the row is ready.
  for (const EJitSmallTableRowKey &Key : AuthorizedRows) {
    if (Key.indices.size() != Plan->dims.size())
      return Fail("authorized row key has the wrong dimension count");
    uint64_t Row = 0;
    for (unsigned I = 0; I < Plan->dims.size(); ++I) {
      if (Key.indices[I] >= Plan->dims[I].extent)
        return Fail("authorized row index is outside the declared extent");
      Row = Row * Plan->dims[I].extent + Key.indices[I];
    }
    if (Row >= Plan->rows.size())
      return Fail("authorized row index is outside the plan");
    if (Plan->rows[Row].ready)
      return Fail("authorized row appears twice");

    uint64_t ElementOff = Plan->sourceElementOffset(Row);
    for (unsigned I = 0; I < Plan->fields.size(); ++I) {
      const EJitSmallTableField &Field = Plan->fields[I];
      uint64_t FieldOff = 0;
      if (__builtin_add_overflow(ElementOff, Field.sourceOffset, &FieldOff) ||
          FieldOff > Source.size ||
          Field.accessSize > Source.size - FieldOff)
        return Fail("authorized row field access leaves the source region");
      Plan->rows[Row].bits[I] =
          readScalarBits(Source.baseAddr + FieldOff, Field, M.getDataLayout());
    }
    Plan->rows[Row].ready = true;
  }

  for (unsigned I = 0; I < Plan->fields.size(); ++I) {
    if (UniformContracts.empty() || !UniformContracts[I])
      continue;
    // An explicit admission contract, not an inference from the visible rows:
    // the caller asserts the invariant, and planning checks every row it can
    // already read. A contract that no ready row confirms is not a validated
    // observation, so it is refused rather than recorded and folded blindly.
    bool Confirmed = false;
    for (const EJitSmallTableRow &Row : Plan->rows) {
      if (!Row.ready)
        continue;
      if (Row.bits[I] != *UniformContracts[I])
        return Fail("uniform admission contract is violated by an authorized "
                    "row");
      Confirmed = true;
    }
    if (!Confirmed)
      return Fail("uniform admission contract has no confirmed ready row");
    Plan->fields[I].uniformValue = *UniformContracts[I];
    Plan->fields[I].columnName.clear();
  }

  if (!Plan->isConsistent(&Error))
    return std::nullopt;
  return Plan;
}

//===----------------------------------------------------------------------===//
// Table materialization
//===----------------------------------------------------------------------===//

bool EJitSmallTablePass::materialize(Module &M, const EJitSmallTablePlan &Plan,
                                     std::string *Error) {
  if (!Plan.isConsistent(Error))
    return false;

  // Spec §5: every row that an optimized read can reach must be initialized
  // and stable, and §5/latest §6.5: a missing row must never be emitted as
  // 0/undef or as a representative cell. Incremental row admission needs the
  // runtime `tableReady(row)` gate (§6.5), which this milestone does not have,
  // so a plan with holes must keep the entry on its original loads instead of
  // producing a table that a business call could read as zeros.
  if (!Plan.allRowsReady()) {
    if (Error)
      *Error = "small-table plan has " +
               std::to_string(Plan.readyRowCount()) + " of " +
               std::to_string(Plan.numRows()) +
               " rows ready; refusing executable lowering until runtime row "
               "admission exists";
    return false;
  }

  LLVMContext &Ctx = M.getContext();
  const DataLayout &DL = M.getDataLayout();
  Type *I64 = Type::getInt64Ty(Ctx);
  const uint64_t ReadyRows = Plan.readyRowCount();

  // Validate every column before creating or filling any of them: a refusal
  // must leave the module untouched, and the three replace rounds call this
  // repeatedly.
  for (const EJitSmallTableField &Field : Plan.fields) {
    if (Field.uniformValue)
      continue;
    Type *ScalarTy = scalarTypeForField(Ctx, Field);
    ArrayType *TableTy = ArrayType::get(ScalarTy, Plan.numRows());
    GlobalVariable *Existing = M.getNamedGlobal(Field.columnName);
    if (!Existing)
      continue;
    if (Existing->getValueType() != TableTy) {
      if (Error)
        *Error = "existing small-table global has a different type: " +
                 Field.columnName;
      return false;
    }
    if (Existing->hasInitializer()) {
      // Only the exact global a previous round created is reusable. A
      // constant or foreign-linkage definition is not a table this pass can
      // vouch for.
      if (Existing->isConstant() ||
          Existing->isDSOLocal() != wantDSOLocal(M)) {
        if (Error)
          *Error = "existing small-table global is not a reusable table: " +
                   Field.columnName;
        return false;
      }
      continue;
    }
    // A pre-declared slot with external linkage is a table this pass now
    // defines and fills. It is never reused as an uninitialized table: the
    // definition below sets the initializer before any load can be replaced.
    if (Existing->getLinkage() != GlobalValue::ExternalLinkage) {
      if (Error)
        *Error = "declared small-table slot has non-external linkage: " +
                 Field.columnName;
      return false;
    }
  }

  for (unsigned I = 0; I < Plan.fields.size(); ++I) {
    const EJitSmallTableField &Field = Plan.fields[I];
    if (Field.uniformValue)
      continue; // A uniform field has no table payload.

    Type *ScalarTy = scalarTypeForField(Ctx, Field);
    ArrayType *TableTy = ArrayType::get(ScalarTy, Plan.numRows());
    // Validated above: either the table a previous round created, or a
    // pre-declared slot this round defines.
    GlobalVariable *GV = M.getNamedGlobal(Field.columnName);

    SmallVector<Constant *, 32> Elements;
    Elements.reserve(Plan.rows.size());
    for (const EJitSmallTableRow &Row : Plan.rows)
      Elements.push_back(Row.ready
                             ? constantFromBits(ScalarTy, Field, Row.bits[I], Ctx)
                             : Constant::getNullValue(ScalarTy));

    if (!GV) {
      GV = new GlobalVariable(M, TableTy, /*isConstant=*/false,
                              GlobalValue::ExternalLinkage,
                              ConstantArray::get(TableTy, Elements),
                              Field.columnName);
    } else {
      GV->setInitializer(ConstantArray::get(TableTy, Elements));
    }
    // Non-preemptible and directly addressable on the product target: AArch64
    // emits adrp+add instead of a GOT load, and the fixed address is what the
    // runtime publishes into (see wantDSOLocal for the host exception).
    GV->setDSOLocal(wantDSOLocal(M));
    GV->setAlignment(DL.getABITypeAlign(ScalarTy));
    Metadata *ColumnOps[] = {
        ConstantAsMetadata::get(ConstantInt::get(I64, Plan.numRows())),
        ConstantAsMetadata::get(ConstantInt::get(I64, ReadyRows)),
        ConstantAsMetadata::get(ConstantInt::get(I64, Field.sourceOffset)),
        ConstantAsMetadata::get(ConstantInt::get(I64, Field.accessSize)),
        ConstantAsMetadata::get(
            ConstantInt::get(I64, static_cast<uint64_t>(Field.kind)))};
    GV->setMetadata(MD_SMALL_TABLE_COLUMN, MDNode::get(Ctx, ColumnOps));
  }

  // Record the folded constants of the admission contract on the entry so the
  // runtime can validate a later member before it is admitted to this code.
  SmallVector<Metadata *, 4> ContractOps;
  for (const EJitSmallTableField &Field : Plan.fields) {
    if (!Field.uniformValue)
      continue;
    Metadata *Ops[] = {
        ConstantAsMetadata::get(ConstantInt::get(I64, Field.sourceOffset)),
        ConstantAsMetadata::get(ConstantInt::get(I64, Field.bitWidth)),
        ConstantAsMetadata::get(
            ConstantInt::get(I64, static_cast<uint64_t>(Field.kind))),
        ConstantAsMetadata::get(ConstantInt::get(I64, *Field.uniformValue))};
    ContractOps.push_back(MDNode::get(Ctx, Ops));
  }
  if (!ContractOps.empty())
    if (Function *Entry = M.getFunction(Plan.entryName))
      Entry->setMetadata(MD_SMALL_TABLE_CONTRACT, MDNode::get(Ctx, ContractOps));

  return true;
}

//===----------------------------------------------------------------------===//
// Replacement
//===----------------------------------------------------------------------===//

PreservedAnalyses EJitSmallTablePass::run(Function &F,
                                          FunctionAnalysisManager &AM) {
  Module *M = F.getParent();
  if (!M)
    return PreservedAnalyses::all();
  // Executable lowering requires a plan that covers its whole declared domain;
  // see materialize(). This is the second entry point, so it enforces the same
  // rule: a caller that bypasses materialize() (or holds a stale table) must
  // not fold a uniform contract or index a table with holes.
  if (!plan_.allRowsReady()) {
    if (F.getName() == plan_.entryName)
      ++stats_.refusedNotReady;
    EJIT_DIAG_VERBOSE("small-table run SKIP func=%s: plan not fully ready",
                      F.getName().str().c_str());
    return PreservedAnalyses::all();
  }
  // The table index is built from this function's own parameters. A callee that
  // is not itself the planned entry is a conservative boundary (§5): its
  // arguments are not known to carry the parent's ready-domain dimensions.
  if (F.getName() != plan_.entryName)
    return PreservedAnalyses::all();
  for (const EJitSmallTableDim &Dim : plan_.dims)
    if (Dim.argIndex >= F.arg_size())
      return PreservedAnalyses::all();

  const DataLayout &DL = M->getDataLayout();
  std::string Error;
  auto Shape = deriveSourceShape(*M, DL, plan_.sourceVarName, plan_.dims, &Error);
  if (!Shape) {
    EJIT_DIAG_VERBOSE("small-table run SKIP func=%s: %s", F.getName().str().c_str(),
                      Error.c_str());
    return PreservedAnalyses::all();
  }

  struct Replacement {
    LoadInst *LI;
    const EJitSmallTableField *Field;
    SmallVector<Value *, 4> DynValues;
  };
  SmallVector<Replacement, 8> Replacements;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *LI = dyn_cast<LoadInst>(&I);
      if (!LI)
        continue;
      MatchedAccess Match;
      bool RootMatched = false;
      if (!matchSmallTableAddress(LI, F, *Shape, plan_.dims, DL, Match,
                                  &RootMatched)) {
        // A may_const load of the source global whose address the plan cannot
        // express keeps its original form and is reported as a refused shape.
        if (RootMatched && LI->hasMetadata(MD_EJIT_MAY_CONST))
          ++stats_.refusedShape;
        continue;
      }
      const EJitSmallTableField *Field =
          findField(plan_.fields, Match, DL, LI->getType());
      if (!Field ||
          !isAuthorizedMayConstLoad(LI, Shape->GV, Match.FieldOffset, DL)) {
        ++stats_.keptOriginal;
        continue;
      }
      ++stats_.mayConstSites;
      Replacements.push_back({LI, Field, std::move(Match.DynValues)});
    }
  }

  bool Changed = false;
  for (Replacement &R : Replacements) {
    Value *New = nullptr;
    if (R.Field->uniformValue) {
      IRBuilder<> Builder(R.LI);
      Value *Folded =
          constantFromBits(scalarTypeForField(M->getContext(), *R.Field),
                           *R.Field, *R.Field->uniformValue, M->getContext());
      New = coerceScalarToLoadType(Builder, Folded, R.LI->getType());
      ++stats_.uniformFolded;
    } else {
      GlobalVariable *Table = M->getNamedGlobal(R.Field->columnName);
      if (!Table) {
        ++stats_.keptOriginal;
        continue;
      }
      IRBuilder<> Builder(R.LI);
      Type *I64 = Builder.getInt64Ty();
      Value *RowIndex = nullptr;
      for (unsigned I = 0; I < R.DynValues.size(); ++I) {
        Value *Idx = R.DynValues[I];
        if (!Idx->getType()->isIntegerTy()) {
          RowIndex = nullptr;
          break;
        }
        if (Idx->getType() != I64)
          Idx = Builder.CreateZExtOrTrunc(Idx, I64);
        const uint64_t Stride = plan_.tableRowStride(I);
        if (Stride != 1)
          Idx = Builder.CreateMul(Idx, Builder.getInt64(Stride));
        RowIndex = RowIndex ? Builder.CreateAdd(RowIndex, Idx) : Idx;
      }
      if (!RowIndex) {
        ++stats_.keptOriginal;
        continue;
      }
      Value *Ptr = Builder.CreateInBoundsGEP(
          Table->getValueType(), Table, {Builder.getInt64(0), RowIndex});
      Type *FieldTy = scalarTypeForField(M->getContext(), *R.Field);
      auto *NewLoad = Builder.CreateLoad(FieldTy, Ptr);
      NewLoad->setAlignment(DL.getABITypeAlign(FieldTy));
      NewLoad->setDebugLoc(R.LI->getDebugLoc());
      // Positive provenance tag (§9): environment names the plan source while
      // the load deliberately stays outside may_const authorization.
      NewLoad->setMetadata(
          MD_SMALL_TABLE_LOAD,
          MDNode::get(Builder.getContext(),
                      {MDString::get(Builder.getContext(), plan_.entryName),
                       MDString::get(Builder.getContext(),
                                     plan_.sourceVarName)}));
      New = coerceScalarToLoadType(Builder, NewLoad, R.LI->getType());
      ++stats_.tableReplaced;
    }
    R.LI->replaceAllUsesWith(New);
    R.LI->eraseFromParent();
    Changed = true;
  }

  EJIT_DIAG_VERBOSE("small-table run func=%s sites=%llu table=%llu uniform=%llu "
                    "kept=%llu refused_shape=%llu",
                    F.getName().str().c_str(),
                    static_cast<unsigned long long>(stats_.mayConstSites),
                    static_cast<unsigned long long>(stats_.tableReplaced),
                    static_cast<unsigned long long>(stats_.uniformFolded),
                    static_cast<unsigned long long>(stats_.keptOriginal),
                    static_cast<unsigned long long>(stats_.refusedShape));
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
