//===-- EJitSmallTable.h - Shared small-table specialization -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine-wide small-table specialization (EJIT_SMALL_TABLE_SPEC.md §6.5/§6.6).
//
// A plan is an immutable value snapshot describing, for one ejit_entry:
//   * the source period array and its index dimensions,
//   * the authorized may_const scalar fields (table columns),
//   * the per-row values copied out of the borrowed source region.
//
// The pass replaces an authorized may_const load whose address depends on the
// real dynamic dimension arguments with a load from a compiler-emitted table
// global indexed by those same dynamic values. No argument is replaced by a
// representative value and the source pointer is never retained.
//
// The plan is deliberately plain data: no IR pointers survive planning, so a
// later inline/cleanup round can never observe a dangling Value.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {

class Module;

namespace ejit {

/// Scalar kind of one table column. Only scalars that fit in 64 bits are
/// supported; pointers, aggregates, vectors and wider integers stay original.
enum class EJitSmallTableKind : uint8_t { Integer, Float, Double };

/// One index dimension of the source period array. The dimension value is
/// either the real function argument at \p argIndex or `urem(arg, modulus)`.
/// Nothing here ever carries a witness or representative argument value.
struct EJitSmallTableDim {
  enum class Kind : uint8_t { Argument, ModuloArgument };

  Kind kind = Kind::Argument;
  uint32_t argIndex = 0;
  /// Divisor for Kind::ModuloArgument. Ignored for Kind::Argument.
  uint32_t modulus = 0;
  /// Number of elements the source array declares for this dimension. The
  /// matcher requires this to equal the array type's extent, so the emitted
  /// table covers the same index space the source GEP can address.
  uint64_t extent = 0;
};

/// One authorized may_const scalar field, emitted as a table column.
struct EJitSmallTableField {
  /// Byte offset of the field inside the innermost source element.
  uint64_t sourceOffset = 0;
  /// Bytes the authorized load reads.
  uint64_t accessSize = 0;
  /// Scalar width in bits (integer width, or 32/64 for float/double).
  ///
  /// The width is part of the field's identity: an authorized load matches this
  /// field only when its own scalar width is equal, because an integer column
  /// preserves the declared width (spec §5). `load i1` and `load i8` at the
  /// same address therefore occupy two columns, and a column never serves a load
  /// that would need a cross-width zext/trunc. Values are copied and compared
  /// masked to this width (the storage padding above it is not part of the
  /// typed value), and a uniform contract must be a value of exactly this
  /// width.
  uint64_t bitWidth = 0;
  EJitSmallTableKind kind = EJitSmallTableKind::Integer;
  /// Emitted global name. Empty for a field folded through an explicit
  /// uniform admission contract: a uniform field has no table payload.
  std::string columnName;
  /// Explicit admission contract (§6.6). When set, every member later admitted
  /// to this code must hold exactly this bit pattern for this field or stay
  /// AOT. It is only ever set by the caller of the planner, never inferred
  /// from the rows that happen to be visible now.
  std::optional<uint64_t> uniformValue;
};

/// One row of copied scalar bits. `bits[i]` belongs to `fields[i]`.
struct EJitSmallTableRow {
  bool ready = false;
  std::vector<uint64_t> bits;
};

/// Immutable, self-contained plan for one entry function.
class EJitSmallTablePlan {
public:
  static constexpr const char *TableGlobalPrefix = "__ejit_stab_";
  /// Bounds on emitted table storage. A plan outside them is refused rather
  /// than silently truncated.
  static constexpr uint64_t MaxRows = 1u << 20;
  static constexpr uint64_t MaxTableBytes = 64u << 20;

  std::string entryName;
  std::string sourceVarName;
  /// Alloc size of the innermost source element the fields live in.
  uint64_t elementBytes = 0;
  SmallVector<EJitSmallTableDim, 4> dims;
  /// Source-array byte stride of each dimension, read from the IR type at plan
  /// time so the stored plan is self-contained.
  SmallVector<uint64_t, 4> sourceStrides;
  SmallVector<EJitSmallTableField, 8> fields;
  /// One entry per linear row index over the declared array extents.
  std::vector<EJitSmallTableRow> rows;

  /// Number of rows the emitted table covers (product of the source array
  /// extents). Zero when the plan is malformed.
  uint64_t numRows() const;

  /// Linear row stride of dimension \p dim in the emitted table.
  uint64_t tableRowStride(unsigned dim) const;

  /// Byte offset of \p row's element in the source array.
  uint64_t sourceElementOffset(uint64_t row) const;

  /// Structural self-check used before materialization.
  bool isConsistent(std::string *why = nullptr) const;

  /// Number of fields that carry an explicit uniform admission contract.
  uint64_t uniformFieldCount() const;
  /// Number of rows whose member configuration was validated and copied.
  uint64_t readyRowCount() const;
  /// True when every row in the declared domain was filled and validated.
  /// Executable lowering requires this: spec §5 says every row that can be
  /// read must be initialized, and a missing row must never be emitted as
  /// 0/undef or as a representative cell. A plan with holes is only usable by
  /// a runtime that gates dispatch per row (spec §6.5 `tableReady(row)`), which
  /// is not implemented in milestone A, so materialize() and run() refuse it.
  bool allRowsReady() const;

  /// True when \p ArgIndex is one of this plan's dynamic dimension arguments.
  /// The pipeline must keep that parameter live instead of substituting the
  /// compiled member's value into it.
  bool isDynamicDimArg(unsigned ArgIndex) const {
    return llvm::any_of(dims, [ArgIndex](const EJitSmallTableDim &D) {
      return D.argIndex == ArgIndex;
    });
  }
};

/// A borrowed source region. The pointer is used only while planning and is
/// never stored in the plan or in the module.
struct EJitSmallTableSource {
  const uint8_t *baseAddr = nullptr;
  uint64_t size = 0;
};

/// The row keys whose member configuration is confirmed ready. Each key lists
/// one index per plan dimension, outermost first.
struct EJitSmallTableRowKey {
  SmallVector<uint64_t, 4> indices;
};

/// Builds plans from a borrowed source region. Planning is the only place that
/// reads application memory: it copies the authorized scalar values and then
/// releases the pointer.
class EJitSmallTablePlanner {
public:
  /// Plan one entry. \p uniformContracts, when non-empty, must have one entry
  /// per discovered field; a value there is an explicit admission contract
  /// that every already-authorized row must satisfy, and at least one ready row
  /// must confirm it (an unobserved contract is not a validated observation).
  /// Returns std::nullopt and sets \p error on any refusal (unknown global,
  /// shape mismatch, out-of-region row, unsupported type, contract mismatch,
  /// unconfirmed contract, overflow).
  static std::optional<EJitSmallTablePlan>
  plan(const Module &M, StringRef entryName, StringRef sourceVarName,
       ArrayRef<EJitSmallTableDim> dims, const EJitSmallTableSource &source,
       ArrayRef<EJitSmallTableRowKey> authorizedRows,
       ArrayRef<std::optional<uint64_t>> uniformContracts,
       std::string &error);

  /// Derive the schema of \p sourceVarName and the authorized field columns
  /// without reading any application memory and without row values. Used by
  /// tests and by the artifact gate to build a plan for a foreign data layout.
  static std::optional<EJitSmallTablePlan>
  planShape(const Module &M, StringRef entryName, StringRef sourceVarName,
            ArrayRef<EJitSmallTableDim> dims, std::string &error);
};

/// Immutable set of plans keyed by entry function name.
class EJitSmallTablePlanSet {
public:
  void add(std::shared_ptr<const EJitSmallTablePlan> plan);
  const EJitSmallTablePlan *find(StringRef entryName) const;
  bool empty() const { return plans_.empty(); }
  size_t size() const { return plans_.size(); }

private:
  StringMap<std::shared_ptr<const EJitSmallTablePlan>> plans_;
};

/// Replace authorized may_const loads with indexed table loads. The emitted
/// table globals are mutable data with a fixed address: the runtime may publish
/// later row values into them, which is exactly why the pass never marks them
/// `constant` and never folds a load to a value.
///
/// Both entry points refuse a plan that does not cover its whole declared
/// domain (`!plan.allRowsReady()`), because a hole would otherwise have to be
/// emitted as a placeholder that an optimized read could reach. Incremental
/// row admission is spec §6.5/§6.6 and belongs to the runtime milestone.
class EJitSmallTablePass : public PassInfoMixin<EJitSmallTablePass> {
public:
  /// Replacement strategy counters (§13), per pass instance.
  struct Stats {
    uint64_t mayConstSites = 0;
    uint64_t tableReplaced = 0;
    uint64_t uniformFolded = 0;
    uint64_t keptOriginal = 0;
    /// may_const loads of the source global whose address shape the plan does
    /// not describe; they keep their original form.
    uint64_t refusedShape = 0;
    /// Times the pass refused to lower a plan because it declares unready rows
    /// (or was never materialized). Non-zero means the entry stayed original.
    uint64_t refusedNotReady = 0;
  };

  explicit EJitSmallTablePass(const EJitSmallTablePlan &plan) : plan_(plan) {}

  /// Create the table globals for \p plan in \p M. Idempotent: an existing
  /// global of the expected type is reused, so the three replace rounds share
  /// one table. Returns false (and leaves the module untouched) on refusal,
  /// including when \p plan has any unready row: without a runtime per-row
  /// dispatch gate the compiler must keep the original loads.
  static bool materialize(Module &M, const EJitSmallTablePlan &plan,
                          std::string *error = nullptr);

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  const Stats &getStats() const { return stats_; }

private:
  const EJitSmallTablePlan &plan_;
  Stats stats_;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLE_H
