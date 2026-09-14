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

/// Per-field strategy chosen by the planner (spec §4.1).
enum class EJitSmallTableStrategy : uint8_t {
  /// Bit-exact constant over the whole proven dependency domain: the field gets
  /// no table column, no payload and no table load, and the branches the
  /// constant decides fold. The value is an admission obligation for later
  /// members (§6.6 step 1): a member that does not hold it stays AOT.
  Uniform,
  /// Retained-axis projection table: only the axes the field provably varies
  /// along are indexed; every eliminated axis is proven irrelevant on the
  /// complete original domain.
  Table,
};

/// How the planner decides a field's strategy. The default is the automatic
/// solver required by §4.1/§6.6; a caller-supplied uniform value is a distinctly
/// selected comparison primitive, never the default contract.
enum class EJitSmallTablePlanMode : uint8_t {
  /// Required default: derive every field's strategy from the proven dependency
  /// domain. `uniformContracts` must be empty in this mode.
  Automatic,
  /// Comparison mode: the caller supplies a uniform value per field (or
  /// `std::nullopt`), each supplied value is validated against every proven row,
  /// and every field without one keeps a table over ALL declared axes. Nothing
  /// is inferred from observed equality, so this mode preserves the historical
  /// explicit-contract and pure-table behavior exactly.
  ExplicitContracts,
};

/// How a table field's column storage is owned (spec §8, milestone B1).
enum class EJitSmallTableStorage : uint8_t {
  /// Compiler-emitted immutable table: the pass defines the column global with
  /// the proven values baked in. Correct only when the plan covers its whole
  /// declared domain, so the executable gate refuses a plan with holes.
  CompilerEmitted = 0,
  /// Runtime-owned shared data resource: the pass emits the column as an
  /// external declaration and never allocates or initializes storage. The
  /// runtime owns one fixed-capacity region at a stable address, binds the same
  /// region into every compile of this code generation, and publishes only
  /// validated, not-yet-published rows into it. Because the executable gate then
  /// moves to the runtime's per-member admission (no un-admitted member may
  /// dispatch to this code), a partial proven domain is allowed here.
  RuntimeOwned = 1,
};

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

/// One authorized may_const scalar field, emitted as a table column or folded to
/// a constant.
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
  /// Uniform or table (spec §4.1).
  EJitSmallTableStrategy strategy = EJitSmallTableStrategy::Table;
  /// Ascending positions into EJitSmallTablePlan::dims that this field's table
  /// retains. Empty exactly when the field is uniform: an eliminated axis is one
  /// the complete original domain proved irrelevant for this field.
  SmallVector<unsigned, 2> retainedAxes;
  /// Table elements for a table field (product of the retained extents), 0 for
  /// a uniform field.
  uint64_t tableRows = 0;
  /// Allocated payload bytes for this field (tableRows * accessSize), 0 for a
  /// uniform field: a uniform field has no payload at all.
  uint64_t tableBytes = 0;
  /// Emitted global name. Empty for a field folded to a constant: a folded field
  /// has no table payload.
  std::string columnName;
  /// Exact typed bit pattern of a uniform field. In the default automatic mode
  /// it is discovered by whole-domain equality over the proven dependency
  /// domain; in the comparison mode it is the caller's admission contract. It
  /// is never inferred from a single row or from an unproven range.
  std::optional<uint64_t> uniformValue;
  /// True when uniformValue came from an explicit caller contract, false when
  /// the automatic solver proved it over the domain.
  bool uniformFromContract = false;

  bool isUniform() const {
    return strategy == EJitSmallTableStrategy::Uniform;
  }
};

/// One row of copied scalar bits. `bits[i]` belongs to `fields[i]`.
struct EJitSmallTableRow {
  bool ready = false;
  std::vector<uint64_t> bits;
};

/// The readiness facts a plan was built from (spec §6.1.1). These are inputs,
/// never inferences: milestone A has no production configuration provider, so a
/// caller passes them explicitly and the plan records exactly what was claimed.
/// `providerLabel` is a label of the source of the fact — compiler tests use a
/// clearly labeled test provider; the production configuration completion point
/// is B0 and is not wired here.
struct EJitSmallTableReadiness {
  /// Caller-supplied configuration generation/epoch of the readiness fact. 0
  /// means the caller supplied no identity, which the contract records rather
  /// than inventing one.
  uint64_t domainEpoch = 0;
  /// Label of the provider that asserted the rows are initialized, readable and
  /// stable. Empty means no provider was named.
  std::string providerLabel;
  /// True when the provider asserts the authorized rows cover every reachable
  /// dependency of the declared extents, not just the member that happens to be
  /// compiled. Without it a plan may be planned but never lowered executably.
  bool coversDeclaredDomain = false;
  /// True when the values were copied under a live, stable borrow of the source
  /// region (§6.1.1). Unstable means "read without a synchronisation proof",
  /// which §5 forbids as a basis for folding.
  bool borrowedStable = false;
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
  /// Readiness/generation identity of the values this plan was built from.
  EJitSmallTableReadiness readiness;
  /// Mode the plan was produced in; recorded so the exported contract never
  /// presents a comparison-mode plan as the automatic default.
  EJitSmallTablePlanMode mode = EJitSmallTablePlanMode::Automatic;
  /// Target facts read from the module at plan time, so the stored plan and its
  /// exported contract stay self-contained: byte order of the copied scalars and
  /// whether the emitted columns bind non-preemptibly at a fixed product address
  /// (AArch64 direct binding) or stay preemptible on the host.
  bool littleEndian = true;
  bool columnsFixedAddress = false;
  /// True when this plan's executable lowering is gated by the runtime's
  /// per-member admission instead of by a compile-time whole-domain proof (spec
  /// §6.5/§6.6, milestone B1). Only a runtime that owns the table resource and
  /// refuses to dispatch an un-validated member to this code may set it; the
  /// compiler-side gate stays in force for every plan that does not.
  bool runtimeRowAdmission = false;
  /// Storage ownership the pass materializes this plan with. Recorded so a plan
  /// can never be lowered with storage the runtime did not actually provide.
  EJitSmallTableStorage storage = EJitSmallTableStorage::CompilerEmitted;

  /// Number of rows the emitted table covers (product of the source array
  /// extents). Zero when the plan is malformed.
  uint64_t numRows() const;

  /// Linear row stride of dimension \p dim in the emitted table.
  uint64_t tableRowStride(unsigned dim) const;

  /// Byte offset of \p row's element in the source array.
  uint64_t sourceElementOffset(uint64_t row) const;

  /// Table row index of full-domain row \p Row under \p Field's retained axes.
  /// Returns 0 for a uniform field (which has no table).
  uint64_t projectRow(const EJitSmallTableField &Field, uint64_t Row) const;

  /// Table stride of the retained axis at position \p Pos of \p Field.
  uint64_t fieldRowStride(const EJitSmallTableField &Field, unsigned Pos) const;

  /// True when \p Field's table retains declared dimension \p Dim.
  bool fieldRetainsDim(const EJitSmallTableField &Field, unsigned Dim) const;

  /// Sum of the fields' payload bytes (spec §13 capacity accounting).
  uint64_t tableBytes() const;

  /// Structural self-check used before materialization.
  bool isConsistent(std::string *why = nullptr) const;

  /// Re-verify every field's final joint projection against the complete
  /// original proven domain (spec §4.1 step 4): a uniform field must be
  /// bit-exactly equal on every proven row, and a table field's retained axes
  /// must group the proven rows into equal-valued projected coordinates. This
  /// is the gate `materialize()` and the pass consult, so a plan whose retained
  /// axes were not re-proven can never reach code generation.
  bool verifyProjections(std::string *why = nullptr) const;

  /// Number of fields that carry a uniform admission contract (caller-supplied
  /// or automatically proven).
  uint64_t uniformFieldCount() const;
  /// Number of fields lowered as retained-axis projection tables.
  uint64_t tableFieldCount() const;
  /// Number of rows whose member configuration was validated and copied.
  uint64_t readyRowCount() const;
  /// True when every row in the declared domain was filled and validated.
  /// Executable lowering requires this: spec §5 says every row that can be
  /// read must be initialized, and a missing row must never be emitted as
  /// 0/undef or as a representative cell. A plan with holes is only usable by
  /// a runtime that gates dispatch per row (spec §6.5 `tableReady(row)`), which
  /// is not implemented in milestone A, so materialize() and run() refuse it.
  bool allRowsReady() const;

  /// True when the proven dependency domain covers the whole declared schema.
  bool domainComplete() const { return allRowsReady(); }

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

/// One planning request. All the inputs the solver needs are here, including
/// the readiness identity the plan must record; nothing is inferred from the
/// values themselves.
struct EJitSmallTableRequest {
  /// Module that declares the entry and the source global. Required.
  const Module *module = nullptr;
  StringRef entryName;
  StringRef sourceVarName;
  ArrayRef<EJitSmallTableDim> dims;
  EJitSmallTableSource source;
  ArrayRef<EJitSmallTableRowKey> authorizedRows;
  /// Explicit caller contracts, comparison mode only: one entry per discovered
  /// field, a value meaning "this field is a contracted constant". Empty selects
  /// no contract at all (and, with Automatic, no caller value either).
  ArrayRef<std::optional<uint64_t>> uniformContracts;
  EJitSmallTablePlanMode mode = EJitSmallTablePlanMode::Automatic;
  EJitSmallTableReadiness readiness;
};

/// Builds plans from a borrowed source region. Planning is the only place that
/// reads application memory: it copies the authorized scalar values and then
/// releases the pointer.
class EJitSmallTablePlanner {
public:
  /// Plan one entry through the full request (mode, readiness identity and
  /// contracts). Automatic mode refuses a non-empty contract list: the default
  /// contract is the proven domain, not a caller assertion.
  static std::optional<EJitSmallTablePlan> plan(const EJitSmallTableRequest &Req,
                                                std::string &error);

  /// Historical 8-argument entry point. It selects the comparison mode when the
  /// caller supplies contracts and the required automatic default otherwise, so
  /// the default can never be silently redefined back to the manual contract.
  static std::optional<EJitSmallTablePlan>
  plan(const Module &M, StringRef entryName, StringRef sourceVarName,
       ArrayRef<EJitSmallTableDim> dims, const EJitSmallTableSource &source,
       ArrayRef<EJitSmallTableRowKey> authorizedRows,
       ArrayRef<std::optional<uint64_t>> uniformContracts, std::string &error);

  /// Derive the schema of \p sourceVarName and the authorized field columns
  /// without reading any application memory and without row values. Used by
  /// tests and by the artifact gate to build a plan for a foreign data layout.
  static std::optional<EJitSmallTablePlan>
  planShape(const Module &M, StringRef entryName, StringRef sourceVarName,
            ArrayRef<EJitSmallTableDim> dims, std::string &error);
};

/// Identity of one emitted table resource (§6.6): a symbol name is not shared
/// storage, so the resource carries the shape and byte count it was emitted
/// with in exactly the compile that materialized it.
struct EJitSmallTableResource {
  std::string symbolName;
  uint64_t elementBits = 0;
  uint64_t rows = 0;
  uint64_t bytes = 0;
  uint64_t alignment = 1;
  /// True when the emitted global is non-preemptible at a fixed product address
  /// (AArch64 direct binding); false is the host's preemptible column.
  bool fixedAddress = false;
};

/// One field's admission obligation (§6.6 step 1-2), self-contained.
struct EJitSmallTableFieldContract {
  uint64_t sourceOffset = 0;
  uint64_t accessSize = 0;
  uint64_t bitWidth = 0;
  EJitSmallTableKind kind = EJitSmallTableKind::Integer;
  EJitSmallTableStrategy strategy = EJitSmallTableStrategy::Table;
  /// Positions into EJitSmallTableContract::dims retained by a table field.
  SmallVector<unsigned, 2> retainedAxes;
  /// Required typed value of a uniform field; a later member must hold it
  /// bit-exactly or stay AOT.
  std::optional<uint64_t> requiredValue;
  /// Table resource identity for a table field.
  EJitSmallTableResource resource;
  /// Every projected coordinate this table already publishes with its bit-exact
  /// value, ascending by coordinate. A later member whose projection lands on one
  /// of these must match it exactly; the value may never be overwritten.
  std::vector<std::pair<uint64_t, uint64_t>> publishedValues;
};

/// Self-contained admission contract of one plan (spec §6.6). It carries the
/// source/row/dependency identity, the readiness/generation fact it was built
/// from, and the per-field constants and projections a later member must
/// satisfy. It never carries an IR pointer, so the runtime can validate a member
/// without the module.
struct EJitSmallTableContract {
  std::string entryName;
  std::string sourceVarName;
  uint64_t elementBytes = 0;
  SmallVector<EJitSmallTableDim, 4> dims;
  SmallVector<uint64_t, 4> sourceStrides;
  uint64_t declaredRows = 0;
  uint64_t provenRows = 0;
  bool domainComplete = false;
  uint64_t domainEpoch = 0;
  std::string readinessProvider;
  bool borrowedStable = false;
  bool coverageAsserted = false;
  /// Target byte order the member scalars must be read with, so a cold-path
  /// validation reads the same typed value the planner copied.
  bool littleEndian = true;
  EJitSmallTablePlanMode mode = EJitSmallTablePlanMode::Automatic;
  SmallVector<EJitSmallTableFieldContract, 8> fields;
  /// Stable digest of the obligations above (source + dims + per-field strategy,
  /// values and resources). Two contracts with the same digest impose the same
  /// admission obligations; two compiles with the same symbol spelling do not.
  uint64_t identityHash = 0;
};

/// One candidate member's observed configuration: the coordinate it occupies in
/// the declared schema and the bit-exact typed values its own storage holds for
/// the contract's fields, in contract field order.
struct EJitSmallTableMember {
  SmallVector<uint64_t, 4> indices;
  SmallVector<uint64_t, 8> bits;
};

/// Derive the exported admission contract of \p Plan.
EJitSmallTableContract buildAdmissionContract(const EJitSmallTablePlan &Plan);

/// Read \p Indices' field values out of a borrowed member region using the
/// contract's own offsets, widths and source strides. Refuses (nullopt + error)
/// when the coordinate leaves the declared schema or the region cannot hold the
/// access, so a cold path can never read past the member it validated.
std::optional<EJitSmallTableMember>
readAdmissionMember(const EJitSmallTableContract &Contract,
                    const EJitSmallTableSource &Source,
                    ArrayRef<uint64_t> Indices, std::string &Error);

/// Classification of a candidate member against a contract (§6.6 steps 2-4).
enum class EJitSmallTableAdmission : uint8_t {
  /// Every obligation already holds: constants match and every projected
  /// coordinate is published with the same value. The member may reuse the code.
  Compatible,
  /// Every existing obligation holds and at least one table field projects to a
  /// supported, not-yet-published coordinate. Publishing it is the runtime's job
  /// (B1) and is not done here.
  Extendable,
  /// A uniform constant differs, or a published projected value differs. The
  /// member must stay AOT; existing specialized code must never run for it.
  Conflict,
  /// The member cannot be validated at all: its coordinate leaves the declared
  /// schema/capacity, its shape is wrong, or no contract identity exists.
  Unusable,
};

const char *admissionName(EJitSmallTableAdmission Admission);

/// Validate \p Member against \p Contract. \p Why, when given, receives the
/// first concrete reason.
EJitSmallTableAdmission validateAdmission(const EJitSmallTableContract &Contract,
                                          const EJitSmallTableMember &Member,
                                          std::string *Why = nullptr);

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

  /// Create the table storage \p plan declares in \p M. Idempotent: an existing
  /// global of the expected type is reused, so the three replace rounds share
  /// one table. The storage form comes from `plan.storage`:
  ///   * CompilerEmitted: the column is defined with the proven values baked in,
  ///     and the plan must cover its whole declared domain.
  ///   * RuntimeOwned: the column is emitted as an external declaration; the
  ///     runtime binds its own fixed-capacity data resource to that symbol and
  ///     publishes validated rows. Such a plan must also set
  ///     `plan.runtimeRowAdmission`, because the executable gate has moved from
  ///     the compile-time domain proof to the runtime's per-member admission.
  /// Returns false (and leaves the module untouched) on refusal.
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
