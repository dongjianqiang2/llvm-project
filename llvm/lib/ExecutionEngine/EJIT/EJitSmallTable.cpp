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

/// True when \p Ty reads exactly the storage of \p Field *and* carries the
/// same scalar width.
///
/// Besides the declared scalar type this accepts a same-width scalar
/// reinterpretation: the optimizer routinely rewrites `bitcast (load float)`
/// into `load i32`, and the table keeps the field's own type, so the load is
/// coerced back at the replacement site. Two supported scalars of equal width
/// are the same bit pattern (integer N vs IEEE float/double N), so that
/// reinterpretation is bit-exact, and `coerceScalarToLoadType` uses a bitcast
/// for it.
///
/// The width equality is what an integer column must preserve (spec §5
/// "整数保留位宽"): `i1` and `i8` at the same address are different typed
/// values, and serving the wider load from a narrower column would either drop
/// its high bits (zext) or feed the narrower load bits no native load of that
/// type would see. Sites with different widths therefore get separate columns,
/// and this predicate is the single gate that keeps a load from being coerced
/// across widths.
bool fieldMatchesType(const EJitSmallTableField &Field, Type *Ty,
                      const DataLayout &DL) {
  uint64_t BitWidth = 0;
  EJitSmallTableKind Kind = EJitSmallTableKind::Integer;
  if (!supportedScalarType(Ty, BitWidth, Kind))
    return false;
  TypeSize Store = DL.getTypeStoreSize(Ty);
  if (Store.isScalable() || Store.getFixedValue() != Field.accessSize)
    return false;
  return BitWidth == Field.bitWidth;
}

/// Reinterpret a scalar read from a table column (or a folded contract
/// constant) as the type the original load had. `findField`/`fieldMatchesType`
/// guarantee both types are supported scalars of the same bit width, so only
/// the view changes and no bit is lost: equal-width integer views are the same
/// type, and integer <-> float/double at equal width is a bitcast.
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

ArrayType *columnTypeForField(LLVMContext &Ctx, const EJitSmallTableField &Field,
                              EJitSmallTableStorage Storage) {
  // Runtime storage packs exactly accessSize bytes per row. A typed array has
  // the scalar's allocation stride, which can exceed its store size (i17: 4/3,
  // i33: 8/5). Its declaration must not promise bytes the runtime never owns.
  if (Storage == EJitSmallTableStorage::RuntimeOwned)
    return ArrayType::get(Type::getInt8Ty(Ctx), Field.tableBytes);
  return ArrayType::get(scalarTypeForField(Ctx, Field), Field.tableRows);
}

/// Rebuild the bit pattern of one scalar from raw target-ordered bytes. The
/// result is the integer the target's LLVM type needs: for integers the value,
/// for float/double the APFloat bit pattern.
///
/// The read is masked to the declared scalar width. A sub-byte integer load
/// (i1..i7, i9..i15, ...) reads `accessSize` bytes but its typed value is the
/// low `bitWidth` bits — the target legalizer promotes such a load the same way
/// — and the storage padding above `bitWidth` is not part of the value. Keeping
/// those padding bits would hand `APInt(bitWidth, ...)` an out-of-range value
/// (assertions-on abort, NDEBUG silent truncation) and would make the uniform
/// contract comparison depend on padding. This is the one place raw memory
/// becomes a typed value, so the mask lives here.
uint64_t readScalarBitsOrdered(const uint8_t *Addr, const EJitSmallTableField &Field,
                               bool LittleEndian) {
  uint64_t Raw = 0;
  const unsigned Bytes = static_cast<unsigned>(Field.accessSize);
  if (LittleEndian) {
    // Source byte order belongs to the target contract, not the host executing
    // the planner/admission decoder. A partial memcpy into a uint64_t is also
    // incorrect for narrow little-endian fields on a big-endian host.
    for (unsigned I = 0; I < Bytes; ++I)
      Raw |= static_cast<uint64_t>(Addr[I]) << (8 * I);
  } else {
    for (unsigned I = 0; I < Bytes; ++I)
      Raw = (Raw << 8) | Addr[I];
  }
  if (Field.bitWidth < 64)
    Raw &= maskTrailingOnes<uint64_t>(static_cast<unsigned>(Field.bitWidth));
  return Raw;
}

uint64_t readScalarBits(const uint8_t *Addr, const EJitSmallTableField &Field,
                        const DataLayout &DL) {
  return readScalarBitsOrdered(Addr, Field, DL.isLittleEndian());
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

uint64_t EJitSmallTablePlan::projectRow(const EJitSmallTableField &Field,
                                        uint64_t Row) const {
  uint64_t Out = 0;
  for (unsigned Pos = 0; Pos < Field.retainedAxes.size(); ++Pos) {
    const unsigned Dim = Field.retainedAxes[Pos];
    if (Dim >= dims.size())
      return 0;
    uint64_t Stride = 1;
    for (unsigned I = Dim + 1; I < dims.size(); ++I)
      Stride *= dims[I].extent;
    const uint64_t Extent = dims[Dim].extent;
    const uint64_t Index = Extent == 0 ? 0 : (Row / Stride) % Extent;
    Out += Index * fieldRowStride(Field, Pos);
  }
  return Out;
}

uint64_t EJitSmallTablePlan::fieldRowStride(const EJitSmallTableField &Field,
                                            unsigned Pos) const {
  uint64_t Stride = 1;
  for (unsigned I = Pos + 1; I < Field.retainedAxes.size(); ++I)
    Stride *= dims[Field.retainedAxes[I]].extent;
  return Stride;
}

bool EJitSmallTablePlan::fieldRetainsDim(const EJitSmallTableField &Field,
                                         unsigned Dim) const {
  return llvm::is_contained(Field.retainedAxes, Dim);
}

uint64_t EJitSmallTablePlan::tableBytes() const {
  uint64_t Bytes = 0;
  for (const EJitSmallTableField &Field : fields)
    Bytes += Field.tableBytes;
  return Bytes;
}

uint64_t EJitSmallTablePlan::uniformFieldCount() const {
  return static_cast<uint64_t>(
      llvm::count_if(fields, [](const EJitSmallTableField &F) {
        return F.strategy == EJitSmallTableStrategy::Uniform;
      }));
}

uint64_t EJitSmallTablePlan::tableFieldCount() const {
  return static_cast<uint64_t>(
      llvm::count_if(fields, [](const EJitSmallTableField &F) {
        return F.strategy == EJitSmallTableStrategy::Table;
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

bool EJitSmallTablePlan::verifyProjections(std::string *Why) const {
  auto Fail = [&](const Twine &Msg) {
    if (Why)
      *Why = Msg.str();
    return false;
  };

  if (dims.empty())
    return Fail("no dimensions");
  for (unsigned I = 0; I < dims.size(); ++I)
    if (dims[I].extent == 0)
      return Fail("declared extent is zero");

  uint64_t Proven = 0;
  for (const EJitSmallTableRow &Row : rows)
    Proven += Row.ready ? 1 : 0;
  if (Proven == 0)
    return Fail("the proven dependency domain is empty: a vacuous constant is "
                "not a proof");

  for (unsigned F = 0; F < fields.size(); ++F) {
    const EJitSmallTableField &Field = fields[F];
    if (Field.strategy == EJitSmallTableStrategy::Uniform) {
      if (!Field.uniformValue)
        return Fail("uniform field carries no value");
      for (const EJitSmallTableRow &Row : rows) {
        if (!Row.ready)
          continue;
        if (F >= Row.bits.size())
          return Fail("row width does not match the field count");
        if (Row.bits[F] != *Field.uniformValue)
          return Fail("uniform value is not equal on the proven domain");
      }
      continue;
    }

    // Table field: the retained axes must group the complete proven domain into
    // bit-exactly equal projected coordinates. This is the final joint
    // projection proof (spec §4.1 step 4); it is re-run here so a plan whose
    // retained axes were never verified can never reach code generation.
    uint64_t TableRows = 1;
    for (unsigned Dim : Field.retainedAxes) {
      if (Dim >= dims.size())
        return Fail("retained axis is outside the declared dimensions");
      if (TableRows > std::numeric_limits<uint64_t>::max() / dims[Dim].extent)
        return Fail("retained-axis row count overflow");
      TableRows *= dims[Dim].extent;
    }
    if (TableRows != Field.tableRows)
      return Fail("retained axes and table row count disagree");
    if (TableRows == 0 || TableRows > MaxRows)
      return Fail("projected table row count is out of range");
    for (unsigned I = 1; I < Field.retainedAxes.size(); ++I)
      if (Field.retainedAxes[I] <= Field.retainedAxes[I - 1])
        return Fail("retained axes are not strictly ascending");

    std::vector<char> Seen(TableRows, 0);
    std::vector<uint64_t> First(TableRows, 0);
    for (uint64_t Row = 0; Row < rows.size(); ++Row) {
      if (!rows[Row].ready)
        continue;
      if (F >= rows[Row].bits.size())
        return Fail("row width does not match the field count");
      const uint64_t Coord = projectRow(Field, Row);
      if (Coord >= TableRows)
        return Fail("projected coordinate leaves the table");
      if (!Seen[Coord]) {
        Seen[Coord] = 1;
        First[Coord] = rows[Row].bits[F];
      } else if (First[Coord] != rows[Row].bits[F]) {
        return Fail("retained axes do not prove equality on the proven domain");
      }
    }
  }
  return true;
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

    if (Field.strategy == EJitSmallTableStrategy::Uniform) {
      if (!Field.uniformValue)
        return Fail("uniform field carries no value");
      if (Field.bitWidth < 64 &&
          *Field.uniformValue >= (uint64_t{1} << Field.bitWidth))
        return Fail("uniform value does not fit the field width");
      if (!Field.retainedAxes.empty())
        return Fail("uniform field retains an axis");
      if (Field.tableRows != 0 || Field.tableBytes != 0)
        return Fail("uniform field has a table payload");
      if (!Field.columnName.empty())
        return Fail("uniform field names a table column");
      continue;
    }

    if (Field.uniformValue)
      return Fail("table field carries a uniform value");
    if (Field.columnName.empty())
      return Fail("table field has no column name");
    if (Field.retainedAxes.empty())
      return Fail("table field retains no axis");
    uint64_t TableRows = 1;
    for (unsigned Dim : Field.retainedAxes) {
      if (Dim >= dims.size())
        return Fail("retained axis is outside the declared dimensions");
      if (TableRows > std::numeric_limits<uint64_t>::max() / dims[Dim].extent)
        return Fail("table row count overflow");
      TableRows *= dims[Dim].extent;
    }
    if (TableRows > MaxRows)
      return Fail("table row count is out of range");
    if (TableRows != Field.tableRows)
      return Fail("retained axes and table row count disagree");
    if (Field.tableBytes != TableRows * Field.accessSize)
      return Fail("table byte count does not match the retained axes");
    if (__builtin_add_overflow(Bytes, Field.tableBytes, &Bytes))
      return Fail("table byte count overflow");
  }
  if (Bytes > MaxTableBytes)
    return Fail("table exceeds the storage bound");

  for (const EJitSmallTableRow &Row : rows)
    if (Row.bits.size() != fields.size())
      return Fail("row width does not match the field count");

  // The final joint projection must hold on the complete original domain before
  // anything may be materialized (spec §4.1 step 4).
  return verifyProjections(Why);
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
  //
  // The site key is (offset, accessSize, bitWidth): an integer column preserves
  // the load's declared width (spec §5), so `load i1` and `load i8` at the same
  // address are two different typed values and must not share one column. Two
  // sites of the same width but different kinds (an `i32` view of a `float`
  // field, the form InstCombine produces) do share the column, because equal
  // width makes the reinterpretation bit-exact.
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
                 S.AccessSize == Match.AccessSize && S.BitWidth == BitWidth;
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
  Plan.littleEndian = DL.isLittleEndian();
  Plan.columnsFixedAddress = wantDSOLocal(M);
  for (unsigned I = 0; I < Sites.size(); ++I) {
    EJitSmallTableField Field;
    Field.sourceOffset = Sites[I].Offset;
    Field.accessSize = Sites[I].AccessSize;
    Field.bitWidth = Sites[I].BitWidth;
    Field.kind = Sites[I].Kind;
    Field.strategy = EJitSmallTableStrategy::Table;
    // A shape plan is not solved yet: it describes the widest supported
    // lowering (every declared axis retained). plan() narrows this per field
    // once the value domain is known.
    for (unsigned Dim = 0; Dim < Dims.size(); ++Dim)
      Field.retainedAxes.push_back(Dim);
    Field.tableRows = RowCount;
    Field.tableBytes = RowCount * Field.accessSize;
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

/// Bounded deterministic projection solver (spec §4.1 steps 1-4).
namespace {

/// Work/memory budget of one field's solver run (§13). Exhaustion is reported
/// and the plan is refused; it is never silently truncated.
constexpr uint64_t MaxSolverAxes = 8;
constexpr uint64_t MaxSolverScratchBytes = 32ull << 20;
constexpr uint64_t MaxSolverWork = 1ull << 28;

/// One candidate axis set and its accounting under the fixed capacity.
struct AxisCandidate {
  SmallVector<unsigned, 2> Axes;
  uint64_t TableRows = 0;
  uint64_t Bytes = 0;
};

/// Deterministic "fewest axes, then fewest allocated bytes, then schema axis
/// order" ordering of two proven candidates (§4.1 step 3).
bool candidateWins(const AxisCandidate &L, const AxisCandidate &R) {
  if (L.Axes.size() != R.Axes.size())
    return L.Axes.size() < R.Axes.size();
  if (L.Bytes != R.Bytes)
    return L.Bytes < R.Bytes;
  return std::lexicographical_compare(L.Axes.begin(), L.Axes.end(),
                                      R.Axes.begin(), R.Axes.end());
}

/// Tests every axis subset of \p Extents against the COMPLETE proven domain and
/// returns the winning candidate. \p Values is the field's bit-exact typed value
/// per row (only ready rows are inspected). No density or neighbour threshold is
/// involved: a candidate either holds for every proven row or it is discarded.
class ProjectionSolver {
public:
  ProjectionSolver(ArrayRef<EJitSmallTableRow> Rows, ArrayRef<uint64_t> Extents,
                   uint64_t AccessSize)
      : Rows_(Rows), Extents_(Extents), AccessSize_(AccessSize) {}

  bool solve(ArrayRef<uint64_t> Values, AxisCandidate &Winner,
             std::string &Error) {
    const unsigned Dims = static_cast<unsigned>(Extents_.size());
    if (Dims == 0 || Dims > MaxSolverAxes)
      return fail("declared axis count is outside the bounded solver", Error);
    if (!Rows_.empty() && Values.size() != Rows_.size())
      return fail("field value count does not match the domain", Error);

    bool HaveWinner = false;
    const uint64_t Masks = uint64_t{1} << Dims;
    for (uint64_t Mask = 0; Mask < Masks; ++Mask) {
      SmallVector<unsigned, 2> Axes;
      for (unsigned I = 0; I < Dims; ++I)
        if (Mask & (uint64_t{1} << I))
          Axes.push_back(I);

      uint64_t TableRows = 1;
      for (unsigned Dim : Axes)
        TableRows *= Extents_[Dim];
      if (TableRows > EJitSmallTablePlan::MaxRows)
        continue; // Capacity refusal: a candidate above the bound never wins.
      const uint64_t Scratch = TableRows * (sizeof(uint64_t) + sizeof(char));
      if (Scratch > MaxSolverScratchBytes ||
          Work_ + Scratch > MaxSolverWork)
        continue; // Budget refusal, reported if nothing at all survives.

      if (!projectionHolds(Axes, Values, TableRows))
        continue;

      AxisCandidate Candidate;
      Candidate.Axes = std::move(Axes);
      Candidate.TableRows = TableRows;
      Candidate.Bytes = TableRows * AccessSize_;
      if (!HaveWinner || candidateWins(Candidate, Winner)) {
        Winner = std::move(Candidate);
        HaveWinner = true;
      }
    }

    if (!HaveWinner)
      return fail("no supported safe projection of the proven domain", Error);
    if (Work_ > MaxSolverWork)
      return fail("solver work budget exhausted", Error);

    // Step 4: re-verify the chosen R with a fresh full-domain scan, so a
    // candidate that was never re-proven can not continue into code generation.
    if (!projectionHolds(Winner.Axes, Values, Winner.TableRows))
      return fail("final joint projection re-verification failed", Error);
    return true;
  }

  uint64_t work() const { return Work_; }

private:
  bool fail(const char *Message, std::string &Error) {
    Error = Message;
    return false;
  }

  /// Group every proven row by its projection onto \p Axes and require
  /// bit-exact equality inside each group. \p TableRows is the projected
  /// cardinality the caller already bounded.
  bool projectionHolds(ArrayRef<unsigned> Axes, ArrayRef<uint64_t> Values,
                       uint64_t TableRows) {
    if (TableRows == 0)
      return false;
    std::vector<char> Seen(static_cast<size_t>(TableRows), 0);
    std::vector<uint64_t> First(static_cast<size_t>(TableRows), 0);
    Work_ += TableRows;
    for (uint64_t Row = 0; Row < Rows_.size(); ++Row) {
      if (!Rows_[Row].ready)
        continue;
      ++Work_;
      const uint64_t Coord = project(Row, Axes);
      if (Coord >= TableRows)
        return false;
      if (!Seen[static_cast<size_t>(Coord)]) {
        Seen[static_cast<size_t>(Coord)] = 1;
        First[static_cast<size_t>(Coord)] = Values[static_cast<size_t>(Row)];
      } else if (First[static_cast<size_t>(Coord)] !=
                 Values[static_cast<size_t>(Row)]) {
        return false;
      }
    }
    return true;
  }

  uint64_t project(uint64_t Row, ArrayRef<unsigned> Axes) const {
    uint64_t Out = 0;
    for (unsigned Pos = 0; Pos < Axes.size(); ++Pos) {
      const unsigned Dim = Axes[Pos];
      uint64_t Stride = 1;
      for (unsigned I = Dim + 1; I < Extents_.size(); ++I)
        Stride *= Extents_[I];
      const uint64_t Index = (Row / Stride) % Extents_[Dim];
      uint64_t OutStride = 1;
      for (unsigned Q = Pos + 1; Q < Axes.size(); ++Q)
        OutStride *= Extents_[Axes[Q]];
      Out += Index * OutStride;
    }
    return Out;
  }

  ArrayRef<EJitSmallTableRow> Rows_;
  ArrayRef<uint64_t> Extents_;
  uint64_t AccessSize_ = 0;
  uint64_t Work_ = 0;
};

} // namespace

std::optional<EJitSmallTablePlan>
EJitSmallTablePlanner::plan(const EJitSmallTableRequest &Req, std::string &Error) {
  auto Fail = [&](const Twine &Msg) -> std::optional<EJitSmallTablePlan> {
    Error = Msg.str();
    return std::nullopt;
  };

  if (!Req.module)
    return Fail("no module supplied to the planner");

  auto Plan = planShape(*Req.module, Req.entryName, Req.sourceVarName, Req.dims,
                        Error);
  if (!Plan)
    return std::nullopt;

  const bool ExplicitMode = Req.mode == EJitSmallTablePlanMode::ExplicitContracts;
  if (!ExplicitMode && !Req.uniformContracts.empty())
    return Fail("automatic mode does not take caller uniform contracts: the "
                "default contract is the proven dependency domain");
  if (!Req.uniformContracts.empty() &&
      Req.uniformContracts.size() != Plan->fields.size())
    return Fail("uniform contract count does not match the field count");

  if (!Req.source.baseAddr || Req.source.size == 0)
    return Fail("source region is null or empty");

  Plan->mode = Req.mode;
  Plan->readiness = Req.readiness;

  // Fill only the rows whose member configuration is confirmed ready. A row
  // that is not authorized stays unreachable by contract (§6.5): the runtime
  // must not dispatch its logical slot to this code before the row is ready.
  for (const EJitSmallTableRowKey &Key : Req.authorizedRows) {
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
          FieldOff > Req.source.size ||
          Field.accessSize > Req.source.size - FieldOff)
        return Fail("authorized row field access leaves the source region");
      Plan->rows[Row].bits[I] = readScalarBits(Req.source.baseAddr + FieldOff,
                                               Field, Req.module->getDataLayout());
    }
    Plan->rows[Row].ready = true;
  }

  uint64_t ProvenRows = 0;
  for (const EJitSmallTableRow &Row : Plan->rows)
    ProvenRows += Row.ready ? 1 : 0;

  SmallVector<uint64_t, 4> Extents;
  for (const EJitSmallTableDim &Dim : Plan->dims)
    Extents.push_back(Dim.extent);

  if (ExplicitMode) {
    // Comparison mode: an explicit caller contract is validated against every
    // proven row (never inferred, never silently truncated), and every field
    // without one keeps a table over ALL declared axes — the historical
    // pure-table shape, deliberately selected rather than defaulted to.
    for (unsigned I = 0; I < Plan->fields.size(); ++I) {
      EJitSmallTableField &Field = Plan->fields[I];
      Field.strategy = EJitSmallTableStrategy::Table;
      Field.retainedAxes.clear();
      for (unsigned Dim = 0; Dim < Plan->dims.size(); ++Dim)
        Field.retainedAxes.push_back(Dim);
      Field.tableRows = Plan->numRows();
      Field.tableBytes = Field.tableRows * Field.accessSize;
      if (Req.uniformContracts.empty() || !Req.uniformContracts[I])
        continue;

      // A contract is a typed bit value of exactly this field's width. A value
      // with bits above the width is not that typed value (it is a raw storage
      // byte), so refuse it instead of truncating it silently.
      if (Field.bitWidth < 64 &&
          *Req.uniformContracts[I] >= (uint64_t{1} << Field.bitWidth))
        return Fail("uniform admission contract value does not fit the field "
                    "width");
      bool Confirmed = false;
      for (const EJitSmallTableRow &Row : Plan->rows) {
        if (!Row.ready)
          continue;
        if (Row.bits[I] != *Req.uniformContracts[I])
          return Fail("uniform admission contract is violated by an authorized "
                      "row");
        Confirmed = true;
      }
      if (!Confirmed)
        return Fail("uniform admission contract has no confirmed ready row");
      Field.strategy = EJitSmallTableStrategy::Uniform;
      Field.uniformValue = *Req.uniformContracts[I];
      Field.uniformFromContract = true;
      Field.columnName.clear();
      Field.retainedAxes.clear();
      Field.tableRows = 0;
      Field.tableBytes = 0;
    }
  } else {
    // Required default (§4.1): solve every field independently on the complete
    // proven domain. The per-field table then keeps only the axes the field
    // provably varies along, and an all-equal field becomes a constant with no
    // column, no payload and no load.
    //
    // An empty proven domain must never produce a vacuous constant (§4.1
    // step 1): equality over zero observations proves nothing.
    if (ProvenRows == 0)
      return Fail("the proven dependency domain is empty: refusing a vacuous "
                  "constant");
    std::vector<uint64_t> Values(Plan->rows.size(), 0);
    for (unsigned I = 0; I < Plan->fields.size(); ++I) {
      EJitSmallTableField &Field = Plan->fields[I];
      for (uint64_t Row = 0; Row < Plan->rows.size(); ++Row)
        Values[static_cast<size_t>(Row)] = Plan->rows[Row].bits[I];

      ProjectionSolver Solver(Plan->rows, Extents, Field.accessSize);
      AxisCandidate Winner;
      std::string SolveError;
      if (!Solver.solve(Values, Winner, SolveError))
        return Fail("field " + Twine(I) + ": " + SolveError);

      Field.retainedAxes = Winner.Axes;
      if (Winner.Axes.empty()) {
        // The empty candidate only wins when every proven row holds the same
        // bit-exact typed value, so the first ready row's masked value is that
        // value. Unready rows are not observations and never supply it.
        uint64_t UniformValue = 0;
        bool HaveValue = false;
        for (uint64_t Row = 0; Row < Plan->rows.size(); ++Row)
          if (Plan->rows[Row].ready) {
            UniformValue = Values[static_cast<size_t>(Row)];
            HaveValue = true;
            break;
          }
        if (!HaveValue)
          return Fail("the proven dependency domain is empty");
        Field.strategy = EJitSmallTableStrategy::Uniform;
        Field.uniformValue = UniformValue;
        Field.uniformFromContract = false;
        Field.columnName.clear();
        Field.tableRows = 0;
        Field.tableBytes = 0;
      } else {
        Field.strategy = EJitSmallTableStrategy::Table;
        Field.uniformValue.reset();
        Field.columnName = (Twine(EJitSmallTablePlan::TableGlobalPrefix) +
                            Plan->entryName + "_c" + Twine(I))
                               .str();
        Field.tableRows = Winner.TableRows;
        Field.tableBytes = Winner.Bytes;
      }
    }
  }

  // Final joint-projection re-verification on the complete original domain
  // (spec §4.1 step 4) and structural self-check before the plan is returned.
  if (!Plan->verifyProjections(&Error))
    return std::nullopt;
  if (!Plan->isConsistent(&Error))
    return std::nullopt;
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
  EJitSmallTableRequest Req;
  Req.module = &M;
  Req.entryName = EntryName;
  Req.sourceVarName = SourceVarName;
  Req.dims = Dims;
  Req.source = Source;
  Req.authorizedRows = AuthorizedRows;
  Req.uniformContracts = UniformContracts;
  // A caller that supplies contracts selected the comparison mode; a caller
  // that supplies none gets the required automatic default.
  Req.mode = UniformContracts.empty() ? EJitSmallTablePlanMode::Automatic
                                      : EJitSmallTablePlanMode::ExplicitContracts;
  return plan(Req, Error);
}

//===----------------------------------------------------------------------===//
// Table materialization
//===----------------------------------------------------------------------===//

bool EJitSmallTablePass::materialize(Module &M, const EJitSmallTablePlan &Plan,
                                     std::string *Error) {
  if (!Plan.isConsistent(Error))
    return false;

  if (Plan.littleEndian != M.getDataLayout().isLittleEndian()) {
    if (Error)
      *Error = "small-table plan byte order does not match module data layout";
    return false;
  }

  const bool RuntimeOwned = Plan.storage == EJitSmallTableStorage::RuntimeOwned;
  // Runtime-owned storage without the runtime admission gate would emit an
  // unfilled declaration that an un-validated member could read: refuse the
  // combination outright instead of trusting the caller.
  if (RuntimeOwned && !Plan.runtimeRowAdmission) {
    if (Error)
      *Error = "runtime-owned small-table storage requires the runtime "
               "row-admission gate (plan.runtimeRowAdmission)";
    return false;
  }

  // Spec §5: every row that an optimized read can reach must be initialized
  // and stable, and §5/latest §6.5: a missing row must never be emitted as
  // 0/undef or as a representative cell. Incremental row admission needs the
  // runtime `tableReady(row)` gate (§6.5); a plan without that gate therefore
  // keeps the entry on its original loads instead of producing a table that a
  // business call could read as zeros.
  if (!Plan.allRowsReady() && !Plan.runtimeRowAdmission) {
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
    if (Field.strategy == EJitSmallTableStrategy::Uniform)
      continue;
    ArrayType *TableTy = columnTypeForField(Ctx, Field, Plan.storage);
    GlobalVariable *Existing = M.getNamedGlobal(Field.columnName);
    if (!Existing)
      continue;
    if (Existing->getValueType() != TableTy) {
      if (Error)
        *Error = "existing small-table global has a different type: " +
                 Field.columnName;
      return false;
    }
    if (RuntimeOwned && Existing->isConstant()) {
      if (Error)
        *Error = "runtime-owned small-table column is not a mutable declaration: " +
                 Field.columnName;
      return false;
    }
    if (Existing->hasInitializer()) {
      // Only the exact global a previous round created is reusable. A
      // constant or foreign-linkage definition is not a table this pass can
      // vouch for. In runtime-owned mode a definition is always foreign: the
      // runtime's shared resource is registered as an absolute symbol, and a
      // module definition would shadow it with per-compile storage of its own.
      if (Existing->isConstant() || RuntimeOwned ||
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
    if (Field.strategy == EJitSmallTableStrategy::Uniform)
      continue; // A uniform field has no table payload at all.

    Type *ScalarTy = scalarTypeForField(Ctx, Field);
    ArrayType *TableTy = columnTypeForField(Ctx, Field, Plan.storage);
    // Validated above: either the table a previous round created, or a
    // pre-declared slot this round defines.
    GlobalVariable *GV = M.getNamedGlobal(Field.columnName);

    if (RuntimeOwned) {
      // The runtime owns one shared, fixed-capacity data resource and binds it
      // to this symbol in every compile of this code generation (spec §8): the
      // module declares the column and never allocates or initializes storage.
      // Publishing validated rows is the runtime's cold-path job (B1).
      if (!GV)
        GV = new GlobalVariable(M, TableTy, /*isConstant=*/false,
                                GlobalValue::ExternalLinkage, /*Initializer=*/nullptr,
                                Field.columnName);
      GV->setAlignment(Align(1));
      SmallVector<Metadata *, 12> ColumnOps;
      ColumnOps.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.tableRows)));
      ColumnOps.push_back(ConstantAsMetadata::get(ConstantInt::get(I64, 0)));
      ColumnOps.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.sourceOffset)));
      ColumnOps.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.accessSize)));
      ColumnOps.push_back(ConstantAsMetadata::get(
          ConstantInt::get(I64, static_cast<uint64_t>(Field.kind))));
      ColumnOps.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.bitWidth)));
      ColumnOps.push_back(ConstantAsMetadata::get(
          ConstantInt::get(I64, Field.retainedAxes.size())));
      for (unsigned Dim : Field.retainedAxes)
        ColumnOps.push_back(ConstantAsMetadata::get(
            ConstantInt::get(I64, static_cast<uint64_t>(Dim))));
      GV->setMetadata(MD_SMALL_TABLE_COLUMN, MDNode::get(Ctx, ColumnOps));
      continue;
    }

    // Fill the field's own projected coordinates. A coordinate the proven
    // domain does not publish is refused rather than emitted as 0/undef or as a
    // representative row (spec §5).
    SmallVector<Constant *, 32> Elements(Field.tableRows,
                                         Constant::getNullValue(ScalarTy));
    std::vector<char> Published(static_cast<size_t>(Field.tableRows), 0);
    for (uint64_t Row = 0; Row < Plan.rows.size(); ++Row) {
      if (!Plan.rows[Row].ready)
        continue;
      const uint64_t Coord = Plan.projectRow(Field, Row);
      if (Coord >= Field.tableRows) {
        if (Error)
          *Error = "projected coordinate leaves the small-table column";
        return false;
      }
      Elements[static_cast<size_t>(Coord)] =
          constantFromBits(ScalarTy, Field, Plan.rows[Row].bits[I], Ctx);
      Published[static_cast<size_t>(Coord)] = 1;
    }
    if (llvm::any_of(Published, [](char P) { return P == 0; })) {
      if (Error)
        *Error = "small-table column has an unpublished projected coordinate: " +
                 Field.columnName;
      return false;
    }

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
    SmallVector<Metadata *, 12> ColumnOps;
    ColumnOps.push_back(
        ConstantAsMetadata::get(ConstantInt::get(I64, Field.tableRows)));
    ColumnOps.push_back(ConstantAsMetadata::get(ConstantInt::get(I64, ReadyRows)));
    ColumnOps.push_back(
        ConstantAsMetadata::get(ConstantInt::get(I64, Field.sourceOffset)));
    ColumnOps.push_back(
        ConstantAsMetadata::get(ConstantInt::get(I64, Field.accessSize)));
    ColumnOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(I64, static_cast<uint64_t>(Field.kind))));
    ColumnOps.push_back(
        ConstantAsMetadata::get(ConstantInt::get(I64, Field.bitWidth)));
    ColumnOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(I64, Field.retainedAxes.size())));
    for (unsigned Dim : Field.retainedAxes)
      ColumnOps.push_back(ConstantAsMetadata::get(
          ConstantInt::get(I64, static_cast<uint64_t>(Dim))));
    GV->setMetadata(MD_SMALL_TABLE_COLUMN, MDNode::get(Ctx, ColumnOps));
  }

  // Record the admission contract on the entry: the folded constants (§6.6
  // step 1) and, for every table field, the retained axes and resource shape
  // the runtime must validate a later member against.
  SmallVector<Metadata *, 4> ContractOps;
  ContractOps.push_back(
      MDString::get(Ctx, "ejit.smalltable.contract.v2"));
  ContractOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(I64, static_cast<uint64_t>(Plan.mode))));
  ContractOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(I64, Plan.readiness.domainEpoch)));
  ContractOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(I64, Plan.numRows())));
  ContractOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(I64, ReadyRows)));
  ContractOps.push_back(
      MDString::get(Ctx, Plan.readiness.providerLabel.empty()
                            ? StringRef("<none>")
                            : StringRef(Plan.readiness.providerLabel)));
  for (const EJitSmallTableField &Field : Plan.fields) {
    SmallVector<Metadata *, 12> Ops;
    if (Field.strategy == EJitSmallTableStrategy::Uniform) {
      Ops.push_back(MDString::get(Ctx, "uniform"));
      Ops.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.sourceOffset)));
      Ops.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.bitWidth)));
      Ops.push_back(ConstantAsMetadata::get(
          ConstantInt::get(I64, static_cast<uint64_t>(Field.kind))));
      Ops.push_back(ConstantAsMetadata::get(
          ConstantInt::get(I64, *Field.uniformValue)));
      Ops.push_back(ConstantAsMetadata::get(
          ConstantInt::get(I64, Field.uniformFromContract ? 1 : 0)));
    } else {
      Ops.push_back(MDString::get(Ctx, "table"));
      Ops.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.sourceOffset)));
      Ops.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.bitWidth)));
      Ops.push_back(ConstantAsMetadata::get(
          ConstantInt::get(I64, static_cast<uint64_t>(Field.kind))));
      Ops.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.tableRows)));
      Ops.push_back(
          ConstantAsMetadata::get(ConstantInt::get(I64, Field.tableBytes)));
      Ops.push_back(ConstantAsMetadata::get(
          ConstantInt::get(I64, Field.retainedAxes.size())));
      for (unsigned Dim : Field.retainedAxes)
        Ops.push_back(ConstantAsMetadata::get(
            ConstantInt::get(I64, static_cast<uint64_t>(Dim))));
    }
    ContractOps.push_back(MDNode::get(Ctx, Ops));
  }
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
  if (plan_.littleEndian != M->getDataLayout().isLittleEndian()) {
    EJIT_DIAG_VERBOSE("small-table run SKIP func=%s: plan byte order does not "
                      "match module data layout", F.getName().str().c_str());
    return PreservedAnalyses::all();
  }
  const bool RuntimeOwned =
      plan_.storage == EJitSmallTableStorage::RuntimeOwned;
  if (RuntimeOwned) {
    if (!plan_.runtimeRowAdmission) {
      EJIT_DIAG_VERBOSE("small-table run SKIP func=%s: runtime storage has no "
                        "row-admission gate", F.getName().str().c_str());
      return PreservedAnalyses::all();
    }
    // A direct pass caller can bypass materialize(). Preflight every runtime
    // column before any uniform fold or replacement, not midway through edits.
    for (const EJitSmallTableField &Field : plan_.fields) {
      if (Field.isUniform())
        continue;
      GlobalVariable *Table = M->getNamedGlobal(Field.columnName);
      if (!Table || Table->getValueType() !=
                        columnTypeForField(M->getContext(), Field, plan_.storage) ||
          Table->hasInitializer() || Table->isConstant() ||
          Table->getLinkage() != GlobalValue::ExternalLinkage) {
        EJIT_DIAG_VERBOSE("small-table run SKIP func=%s: invalid runtime column %s",
                          F.getName().str().c_str(), Field.columnName.c_str());
        return PreservedAnalyses::all();
      }
    }
  }
  // Executable lowering requires a plan that covers its whole declared domain;
  // see materialize(). This is the second entry point, so it enforces the same
  // rule: a caller that bypasses materialize() (or holds a stale table) must
  // not fold a uniform contract or index a table with holes. A runtime-owned
  // plan whose rows are gated per member by the runtime is the one exception:
  // its executable gate is the runtime admission, not the compile-time proof.
  if (!plan_.allRowsReady() && !plan_.runtimeRowAdmission) {
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
    if (R.Field->strategy == EJitSmallTableStrategy::Uniform) {
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
      // Only the field's own retained axes index its table: each eliminated
      // axis was proven irrelevant over the complete original domain, so it is
      // not part of this column's address at all.
      for (unsigned Pos = 0; Pos < R.Field->retainedAxes.size(); ++Pos) {
        const unsigned Dim = R.Field->retainedAxes[Pos];
        if (Dim >= R.DynValues.size()) {
          RowIndex = nullptr;
          break;
        }
        Value *Idx = R.DynValues[Dim];
        if (!Idx->getType()->isIntegerTy()) {
          RowIndex = nullptr;
          break;
        }
        if (Idx->getType() != I64)
          Idx = Builder.CreateZExtOrTrunc(Idx, I64);
        const uint64_t Stride = plan_.fieldRowStride(*R.Field, Pos);
        if (Stride != 1)
          Idx = Builder.CreateMul(Idx, Builder.getInt64(Stride));
        RowIndex = RowIndex ? Builder.CreateAdd(RowIndex, Idx) : Idx;
      }
      if (!RowIndex) {
        ++stats_.keptOriginal;
        continue;
      }
      Value *Ptr;
      if (RuntimeOwned) {
        // Match the runtime's exact packed row stride, not the LLVM scalar's
        // allocation size. The load keeps its original scalar type/bit width.
        Value *ByteOffset = RowIndex;
        if (R.Field->accessSize != 1)
          ByteOffset = Builder.CreateMul(RowIndex,
                                         Builder.getInt64(R.Field->accessSize));
        Ptr = Builder.CreateInBoundsGEP(Builder.getInt8Ty(), Table, ByteOffset);
      } else {
        Ptr = Builder.CreateInBoundsGEP(
            Table->getValueType(), Table, {Builder.getInt64(0), RowIndex});
      }
      Type *FieldTy = scalarTypeForField(M->getContext(), *R.Field);
      auto *NewLoad = Builder.CreateLoad(FieldTy, Ptr);
      NewLoad->setAlignment(RuntimeOwned ? Align(1) : DL.getABITypeAlign(FieldTy));
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

//===----------------------------------------------------------------------===//
// Exported admission contract (§6.6)
//===----------------------------------------------------------------------===//

namespace llvm {
namespace ejit {

namespace {

/// FNV-1a over the contract's obligations. It exists so a runtime can tell two
/// contracts apart: two compiles using the same symbol spelling are not the same
/// table resource, and two contracts with the same digest do impose the same
/// admission obligations.
uint64_t contractHashStep(uint64_t H, uint64_t Value) {
  for (unsigned I = 0; I < 8; ++I) {
    H ^= (Value >> (I * 8)) & 0xFFu;
    H *= 1099511628211ull;
  }
  return H;
}

uint64_t contractHashString(uint64_t H, StringRef S) {
  for (char C : S) {
    H ^= static_cast<unsigned char>(C);
    H *= 1099511628211ull;
  }
  return contractHashStep(H, S.size());
}

/// Table stride of the retained axis at position \p Pos of a contract field.
uint64_t contractAxisStride(const EJitSmallTableFieldContract &Field,
                            const EJitSmallTableContract &Contract,
                            unsigned Pos) {
  uint64_t Stride = 1;
  for (unsigned I = Pos + 1; I < Field.retainedAxes.size(); ++I)
    Stride *= Contract.dims[Field.retainedAxes[I]].extent;
  return Stride;
}

} // namespace

EJitSmallTableContract buildAdmissionContract(const EJitSmallTablePlan &Plan) {
  EJitSmallTableContract Contract;
  Contract.entryName = Plan.entryName;
  Contract.sourceVarName = Plan.sourceVarName;
  Contract.elementBytes = Plan.elementBytes;
  Contract.dims.append(Plan.dims.begin(), Plan.dims.end());
  Contract.sourceStrides.append(Plan.sourceStrides.begin(),
                                Plan.sourceStrides.end());
  Contract.declaredRows = Plan.numRows();
  Contract.provenRows = Plan.readyRowCount();
  Contract.domainComplete = Plan.domainComplete();
  Contract.domainEpoch = Plan.readiness.domainEpoch;
  Contract.readinessProvider = Plan.readiness.providerLabel;
  Contract.borrowedStable = Plan.readiness.borrowedStable;
  Contract.coverageAsserted = Plan.readiness.coversDeclaredDomain;
  Contract.littleEndian = Plan.littleEndian;
  Contract.mode = Plan.mode;

  for (unsigned I = 0; I < Plan.fields.size(); ++I) {
    const EJitSmallTableField &Field = Plan.fields[I];
    EJitSmallTableFieldContract FC;
    FC.sourceOffset = Field.sourceOffset;
    FC.accessSize = Field.accessSize;
    FC.bitWidth = Field.bitWidth;
    FC.kind = Field.kind;
    FC.strategy = Field.strategy;
    FC.retainedAxes = Field.retainedAxes;
    if (Field.strategy == EJitSmallTableStrategy::Uniform) {
      FC.requiredValue = Field.uniformValue;
    } else {
      FC.resource.symbolName = Field.columnName;
      FC.resource.elementBits = Field.bitWidth;
      FC.resource.rows = Field.tableRows;
      FC.resource.bytes = Field.tableBytes;
      // Access size is not alignment (3/5/6/7 bytes are not even powers of
      // two). A plan does not carry the module's ABI alignment, so export the
      // universally valid byte guarantee rather than guessing a stronger one.
      FC.resource.alignment = 1;
      FC.resource.fixedAddress = Plan.columnsFixedAddress;
      // The published projections: the coordinate of every proven row with the
      // bit-exact value the emitted column holds there. The plan already proved
      // that every proven row of one coordinate carries the same value.
      std::vector<char> Seen(static_cast<size_t>(Field.tableRows), 0);
      for (uint64_t Row = 0; Row < Plan.rows.size(); ++Row) {
        if (!Plan.rows[Row].ready)
          continue;
        const uint64_t Coord = Plan.projectRow(Field, Row);
        if (Coord >= Field.tableRows)
          continue;
        if (Seen[static_cast<size_t>(Coord)])
          continue;
        Seen[static_cast<size_t>(Coord)] = 1;
        FC.publishedValues.push_back({Coord, Plan.rows[Row].bits[I]});
      }
      llvm::sort(FC.publishedValues,
                 [](const std::pair<uint64_t, uint64_t> &L,
                    const std::pair<uint64_t, uint64_t> &R) {
                   return L.first < R.first;
                 });
    }
    Contract.fields.push_back(std::move(FC));
  }

  uint64_t H = 14695981039346656037ull;
  H = contractHashString(H, Contract.entryName);
  H = contractHashString(H, Contract.sourceVarName);
  H = contractHashStep(H, Contract.elementBytes);
  H = contractHashStep(H, Contract.declaredRows);
  H = contractHashStep(H, Contract.provenRows);
  H = contractHashStep(H, static_cast<uint64_t>(Contract.mode));
  H = contractHashStep(H, Contract.domainEpoch);
  H = contractHashStep(H, Contract.littleEndian ? 1 : 0);
  for (const EJitSmallTableDim &Dim : Contract.dims) {
    H = contractHashStep(H, static_cast<uint64_t>(Dim.kind));
    H = contractHashStep(H, Dim.argIndex);
    H = contractHashStep(H, Dim.modulus);
    H = contractHashStep(H, Dim.extent);
  }
  for (const EJitSmallTableFieldContract &FC : Contract.fields) {
    H = contractHashStep(H, FC.sourceOffset);
    H = contractHashStep(H, FC.accessSize);
    H = contractHashStep(H, FC.bitWidth);
    H = contractHashStep(H, static_cast<uint64_t>(FC.kind));
    H = contractHashStep(H, static_cast<uint64_t>(FC.strategy));
    H = contractHashStep(H, FC.requiredValue.value_or(0));
    H = contractHashStep(H, FC.requiredValue.has_value() ? 1 : 0);
    H = contractHashStep(H, FC.retainedAxes.size());
    for (unsigned Axis : FC.retainedAxes)
      H = contractHashStep(H, Axis);
    H = contractHashString(H, FC.resource.symbolName);
    H = contractHashStep(H, FC.resource.rows);
    H = contractHashStep(H, FC.resource.bytes);
    H = contractHashStep(H, FC.publishedValues.size());
    for (const std::pair<uint64_t, uint64_t> &P : FC.publishedValues) {
      H = contractHashStep(H, P.first);
      H = contractHashStep(H, P.second);
    }
  }
  Contract.identityHash = H;
  return Contract;
}

std::optional<EJitSmallTableMember>
readAdmissionMember(const EJitSmallTableContract &Contract,
                    const EJitSmallTableSource &Source,
                    ArrayRef<uint64_t> Indices, std::string &Error) {
  auto Fail = [&](const Twine &Msg) -> std::optional<EJitSmallTableMember> {
    Error = Msg.str();
    return std::nullopt;
  };

  if (!Source.baseAddr || Source.size == 0)
    return Fail("member region is null or empty");
  if (Indices.size() != Contract.dims.size())
    return Fail("member coordinate does not match the contract schema");
  if (Contract.sourceStrides.size() != Contract.dims.size())
    return Fail("contract source strides do not match its dimensions");

  uint64_t ElementOff = 0;
  for (unsigned I = 0; I < Indices.size(); ++I) {
    if (Indices[I] >= Contract.dims[I].extent)
      return Fail("member coordinate leaves the declared schema/capacity");
    uint64_t Add = 0;
    if (__builtin_mul_overflow(Indices[I], Contract.sourceStrides[I], &Add) ||
        __builtin_add_overflow(ElementOff, Add, &ElementOff))
      return Fail("member coordinate address overflow");
  }

  EJitSmallTableMember Member;
  Member.indices.append(Indices.begin(), Indices.end());
  for (const EJitSmallTableFieldContract &FC : Contract.fields) {
    EJitSmallTableField Field;
    Field.sourceOffset = FC.sourceOffset;
    Field.accessSize = FC.accessSize;
    Field.bitWidth = FC.bitWidth;
    Field.kind = FC.kind;
    uint64_t FieldOff = 0;
    if (__builtin_add_overflow(ElementOff, FC.sourceOffset, &FieldOff) ||
        FieldOff > Source.size || FC.accessSize > Source.size - FieldOff)
      return Fail("member field access leaves the member region");
    Member.bits.push_back(
        readScalarBitsOrdered(Source.baseAddr + FieldOff, Field,
                              Contract.littleEndian));
  }
  return Member;
}

const char *admissionName(EJitSmallTableAdmission Admission) {
  switch (Admission) {
  case EJitSmallTableAdmission::Compatible:
    return "compatible";
  case EJitSmallTableAdmission::Extendable:
    return "extendable";
  case EJitSmallTableAdmission::Conflict:
    return "conflict";
  case EJitSmallTableAdmission::Unusable:
    return "unusable";
  }
  llvm_unreachable("unknown admission classification");
}

EJitSmallTableAdmission validateAdmission(const EJitSmallTableContract &Contract,
                                          const EJitSmallTableMember &Member,
                                          std::string *Why) {
  auto Set = [&](const Twine &Msg) {
    if (Why)
      *Why = Msg.str();
  };

  if (Contract.dims.empty())
    return Set("contract declares no schema"), EJitSmallTableAdmission::Unusable;
  if (Contract.identityHash == 0)
    return Set("contract has no identity"), EJitSmallTableAdmission::Unusable;
  if (Member.indices.size() != Contract.dims.size())
    return Set("member coordinate does not match the declared schema"),
           EJitSmallTableAdmission::Unusable;
  if (Member.bits.size() != Contract.fields.size())
    return Set("member value count does not match the contract fields"),
           EJitSmallTableAdmission::Unusable;
  for (unsigned I = 0; I < Member.indices.size(); ++I)
    if (Member.indices[I] >= Contract.dims[I].extent)
      return Set("member coordinate leaves the declared schema/capacity"),
             EJitSmallTableAdmission::Unusable;

  bool Extendable = false;
  for (unsigned I = 0; I < Contract.fields.size(); ++I) {
    const EJitSmallTableFieldContract &FC = Contract.fields[I];
    const uint64_t Bits = Member.bits[I];
    // The member value is a typed bit value of exactly the field's width: a raw
    // wider storage byte is not that typed value.
    if (FC.bitWidth == 0 || FC.bitWidth > 64)
      return Set("contract field has no supported width"),
             EJitSmallTableAdmission::Unusable;
    if (FC.bitWidth < 64 && Bits >= (uint64_t{1} << FC.bitWidth))
      return Set("member value does not fit the field width"),
             EJitSmallTableAdmission::Unusable;

    if (FC.strategy == EJitSmallTableStrategy::Uniform) {
      if (!FC.requiredValue)
        return Set("contract uniform field has no required value"),
               EJitSmallTableAdmission::Unusable;
      if (Bits != *FC.requiredValue)
        return Set("a uniform constant differs from the contract"),
               EJitSmallTableAdmission::Conflict;
      continue;
    }

    // Project the member onto the retained axes and require an already
    // published coordinate to carry exactly this value. A compressed projected
    // value is shared, so "table fields may differ per row" does not apply here
    // (spec §6.6).
    uint64_t Coord = 0;
    for (unsigned Pos = 0; Pos < FC.retainedAxes.size(); ++Pos) {
      const unsigned Dim = FC.retainedAxes[Pos];
      if (Dim >= Contract.dims.size())
        return Set("contract retained axis is outside the schema"),
               EJitSmallTableAdmission::Unusable;
      Coord += Member.indices[Dim] * contractAxisStride(FC, Contract, Pos);
    }
    auto It = std::lower_bound(
        FC.publishedValues.begin(), FC.publishedValues.end(), Coord,
        [](const std::pair<uint64_t, uint64_t> &P, uint64_t C) {
          return P.first < C;
        });
    if (It != FC.publishedValues.end() && It->first == Coord) {
      if (It->second != Bits)
        return Set("a published projected value differs from the contract"),
               EJitSmallTableAdmission::Conflict;
      continue;
    }
    if (Coord >= FC.resource.rows)
      return Set("projected coordinate leaves the declared table capacity"),
             EJitSmallTableAdmission::Unusable;
    Extendable = true;
  }

  if (Extendable)
    return Set("member projects to a supported, not yet published coordinate"),
           EJitSmallTableAdmission::Extendable;
  return EJitSmallTableAdmission::Compatible;
}

} // namespace ejit
} // namespace llvm
