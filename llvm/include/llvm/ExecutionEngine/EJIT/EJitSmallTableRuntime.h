//===-- EJitSmallTableRuntime.h - online small-table runtime (B0/B1/B2) ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The runtime half of the cell/TRP small-table design (EJIT_SMALL_TABLE_SPEC.md
// §6.1.1, §6.5, §6.6, §10): readiness facts and a protected read borrow (B0), one
// shared fixed-capacity table resource with generation identity and incremental
// row publication (B1), and ONE common sampling session per entry/code
// generation with an aggregate, configurable entry budget (B2).
//
// What is real here and what is not:
//   * real: the planner, the module lowering, the ORC engine, the code
//     generation of the common instrumented T1 and common T2, the counter
//     capture and profile synthesis, the shared data resource and every
//     admission/publication/identity check.
//   * NOT wired: the product configuration transaction. Readiness, the member
//     set and the read borrow come from an `EJitSmallTableReadinessProvider`.
//     No provider installed means the runtime fails closed and nothing is
//     specialized. `EJitSmallTableHostProvider` below is an explicitly labeled
//     HOST integration adapter for driving the path without the product; it is
//     never product readiness and must not be presented as such.
//
// The runtime is single-owner: the owner core performs admission, publication,
// sampling bookkeeping and freeze. Published values are never rewritten.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLERUNTIME_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLERUNTIME_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTable.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef EJIT_FREESTANDING
#include <condition_variable>
#include <mutex>
#endif

namespace llvm {

class LLVMContext;
class Module;

namespace ejit {

class EJitOrcEngine;
class EJitSmallTableReadBorrow;
class EJitSmallTableReadinessProvider;
class EJitSmallTableTableResource;
class EJitSmallTableHost;
class PeriodArrayRegistry;
class EJitRuntimeState;
struct SpecializationContext;

//===----------------------------------------------------------------------===//
// B0: readiness facts, protected borrow, provider interface
//===----------------------------------------------------------------------===//

/// One member's readiness fact from the configuration side (spec §6.1.1). It is
/// an input fact, never an inference: a member is ready only when the provider
/// says its own configuration completed and the candidate may_const fields are
/// initialized and stable at this generation.
struct EJitSmallTableReadyMember {
  /// One index per declared plan dimension, outermost first.
  SmallVector<uint64_t, 4> indices;
  /// The member's own completed configuration version. 0 means the provider
  /// supplied no version, which the admission records rather than inventing.
  uint64_t configGeneration = 0;
  /// True when every candidate field of this member is initialized and stable.
  /// A member whose fields are not all ready is not admitted at all.
  bool fieldsInitialized = false;
};

/// A protected read borrow over the declared source region (spec §6.1.1).
/// The runtime reads only through `source()`; every read refuses once the
/// borrow was released, invalidated (configuration generation changed, cancel,
/// timeout) or never granted. Taking a borrow is the only way the runtime
/// touches application memory, and it is released on every path.
class EJitSmallTableReadBorrow {
public:
  EJitSmallTableReadBorrow(EJitSmallTableReadinessProvider *Owner,
                           uint64_t Epoch, EJitSmallTableSource Source);
  ~EJitSmallTableReadBorrow();

  EJitSmallTableReadBorrow(const EJitSmallTableReadBorrow &) = delete;
  EJitSmallTableReadBorrow &operator=(const EJitSmallTableReadBorrow &) = delete;

  uint64_t epoch() const { return epoch_; }
  bool released() const { return released_; }
  bool stale() const { return !invalidation_.empty(); }
  StringRef invalidationReason() const { return invalidation_; }

  /// The borrowed region. Empty once released or invalidated, so a caller that
  /// ignores the state still cannot read through a dead borrow.
  EJitSmallTableSource source() const {
    if (released_ || stale())
      return EJitSmallTableSource{};
    return source_;
  }

  /// Provider callback: the configuration generation moved, the session was
  /// cancelled, or the borrow timed out. Every later read and publication
  /// refuses; this is a refusal, not a silent re-read.
  void invalidate(StringRef Reason);

  /// Release exactly once. Idempotent.
  void release();

private:
  EJitSmallTableReadinessProvider *owner_ = nullptr;
  uint64_t epoch_ = 0;
  EJitSmallTableSource source_;
  bool released_ = false;
  std::string invalidation_;
};

/// The B0 provider interface. The product configuration transaction is the only
/// legitimate implementation; until it is bound, the runtime is fail-closed:
/// `EJitSmallTableRuntime::create` without a provider, or a provider that cannot
/// take a borrow, refuses to specialize and the entry stays AOT.
class EJitSmallTableReadinessProvider {
public:
  virtual ~EJitSmallTableReadinessProvider() = default;

  /// Label of the fact source, recorded in the plan/contract. A test or host
  /// adapter must label itself as such (see `EJitSmallTableHostProvider`).
  virtual StringRef label() const = 0;

  /// Configuration domain epoch these facts describe. 0 means no identity.
  virtual uint64_t domainEpoch() const = 0;

  /// True while \p Epoch is still the current configuration generation. The
  /// runtime re-checks this after the last read and immediately before
  /// publication (spec §6.1.1/§6.3).
  virtual bool epochCurrent(uint64_t Epoch) const = 0;

  /// True when the ready member set covers every reachable dependency of the
  /// declared extents, not only the member being compiled.
  virtual bool coversDeclaredDomain() const = 0;

  /// The confirmed-ready member set of the current epoch.
  virtual ArrayRef<EJitSmallTableReadyMember> readyMembers() const = 0;

  /// Take a protected read borrow over \p SourceVarName's declared region.
  /// Returns nullptr and fills \p Error when no borrow is available.
  virtual std::unique_ptr<EJitSmallTableReadBorrow>
  borrow(StringRef SourceVarName, std::string &Error) = 0;

  /// Called when the runtime releases a borrow it took.
  virtual void onBorrowReleased(EJitSmallTableReadBorrow *Borrow) {
    (void)Borrow;
  }
};

/// Explicitly labeled HOST integration adapter (NOT product readiness).
///
/// This implements the provider interface over a host-registered region and an
/// explicitly supplied member list, so the real runtime path (plan, resource,
/// publication, common T1/T2, sampling session) can be driven on a host where
/// the product configuration transaction is unavailable. It observes no
/// configuration commit, it never reads the product's lifecycle state, and its
/// label says so; the runtime records the label in the plan and the contract.
/// The remaining product binding is exactly: replace this adapter with an
/// implementation over the product's configuration completion point, without
/// changing the interface.
class EJitSmallTableHostProvider final : public EJitSmallTableReadinessProvider {
public:
  /// The label every artifact produced through this adapter carries.
  static constexpr const char *Label = "host-adapter.pr231-not-product";

  EJitSmallTableHostProvider(std::string SourceVarName, const void *Base,
                             uint64_t Bytes, uint64_t Epoch);

  /// Append one confirmed-ready member (the caller asserts the fact; this
  /// adapter has nothing to verify it against, which is exactly why it is
  /// labeled a host adapter).
  void addReadyMember(ArrayRef<uint64_t> Indices, uint64_t ConfigGeneration,
                      bool FieldsInitialized = true);

  /// Simulate the product's configuration-generation change: an in-flight
  /// borrow is invalidated and `epochCurrent` turns false for the old epoch.
  void invalidateGeneration();

  /// Refuse the next borrow with \p Reason (product provider missing/locked).
  void refuseBorrow(std::string Reason);

  /// Allow borrows again: the product configuration transaction became
  /// available after a transient refusal (a delayed borrow).
  void allowBorrow() { borrowRefusal_.clear(); }

  void setCoverage(bool Covers) { covers_ = Covers; }
  size_t outstandingBorrows() const { return outstandingBorrows_; }
  uint64_t borrowCount() const { return borrowCount_; }

  StringRef label() const override { return Label; }
  uint64_t domainEpoch() const override { return epoch_; }
  bool epochCurrent(uint64_t Epoch) const override {
    return Epoch == epoch_ && !generationInvalidated_;
  }
  bool coversDeclaredDomain() const override { return covers_; }
  ArrayRef<EJitSmallTableReadyMember> readyMembers() const override {
    return members_;
  }
  std::unique_ptr<EJitSmallTableReadBorrow>
  borrow(StringRef SourceVarName, std::string &Error) override;
  void onBorrowReleased(EJitSmallTableReadBorrow *Borrow) override;

private:
  std::string sourceVarName_;
  const void *base_ = nullptr;
  uint64_t bytes_ = 0;
  uint64_t epoch_ = 0;
  bool covers_ = true;
  bool generationInvalidated_ = false;
  std::string borrowRefusal_;
  SmallVector<EJitSmallTableReadyMember, 8> members_;
  size_t outstandingBorrows_ = 0;
  uint64_t borrowCount_ = 0;
};

//===----------------------------------------------------------------------===//
// B1: shared table resource with generation identity and row publication
//===----------------------------------------------------------------------===//

/// One shared, fixed-capacity data resource for one plan generation (spec §8):
/// a stable address outside the code pool, a real generation identity, one
/// contiguous column region per table field, and incremental publication of
/// validated rows. The same resource is bound into every compile of the code
/// generation (T1 and T2), so a symbol name alone is never taken as proof that
/// two compiles see the same storage.
class EJitSmallTableTableResource {
public:
  /// One field's column inside the resource.
  struct Column {
    std::string symbolName;
    uint64_t offset = 0;
    uint64_t rows = 0;
    uint64_t elementBytes = 0;
    uint64_t payloadBytes = 0;
    SmallVector<unsigned, 2> retainedAxes;
    /// Index into the plan's fields; lets publication map a field to its column.
    unsigned fieldIndex = 0;
  };

  /// Capacity/retention accounting (spec §8/§13): reserved is the capacity this
  /// generation was sized for, allocated what was actually taken, payload the
  /// column bytes, published the bytes whose values are immutable from now on.
  struct Accounting {
    uint64_t reservedBytes = 0;
    uint64_t allocatedBytes = 0;
    uint64_t payloadBytes = 0;
    uint64_t publishedBytes = 0;
    uint64_t publishedCells = 0;
  };

  enum class PublishResult : uint8_t {
    Stored,      ///< first publication of this coordinate
    AlreadySame, ///< the coordinate was already published with this value
    Conflict,    ///< the coordinate is published with a different value: refuse
    OutOfRange,  ///< the coordinate leaves the column capacity
    NotATable,   ///< the field is uniform (no column exists)
  };

  ~EJitSmallTableTableResource();

  EJitSmallTableTableResource(const EJitSmallTableTableResource &) = delete;
  EJitSmallTableTableResource &operator=(const EJitSmallTableTableResource &) =
      delete;

  /// Allocate the resource for \p Plan. \p CapacityLimit bounds the payload; a
  /// plan above it is refused rather than truncated. Returns nullptr + \p Error
  /// on refusal.
  static std::unique_ptr<EJitSmallTableTableResource>
  create(const EJitSmallTablePlan &Plan, uint64_t Generation,
         uint64_t CapacityLimit, std::string &Error);

  uint64_t generation() const { return generation_; }
  const uint8_t *base() const { return base_; }
  uint64_t capacityBytes() const { return capacityBytes_; }
  ArrayRef<Column> columns() const { return columns_; }
  const Column *findColumn(StringRef Symbol) const;
  /// Address of \p FieldIndex's column, or nullptr for a uniform field.
  void *columnAddress(unsigned FieldIndex) const;
  const Column *columnForField(unsigned FieldIndex) const;

  /// Publish one projected coordinate. Never overwrites: a repeated coordinate
  /// with the same value is a no-op, with a different value a `Conflict`.
  PublishResult publish(unsigned FieldIndex, uint64_t Coordinate, uint64_t Bits);

  /// Read back a published coordinate (for admission of later members).
  bool published(unsigned FieldIndex, uint64_t Coordinate, uint64_t *Bits) const;

  /// Every published coordinate of \p FieldIndex, ascending (contract export).
  std::vector<std::pair<uint64_t, uint64_t>>
  publishedValues(unsigned FieldIndex) const;

  Accounting accounting() const;

private:
  explicit EJitSmallTableTableResource(bool LittleEndian)
      : littleEndian_(LittleEndian) {}
  const bool littleEndian_;

  uint8_t *base_ = nullptr;
  uint64_t capacityBytes_ = 0;
  uint64_t generation_ = 0;
  SmallVector<Column, 8> columns_;
  /// Per column: published[coordinate] = {known, value}.
  std::vector<std::vector<std::pair<bool, uint64_t>>> published_;
};

//===----------------------------------------------------------------------===//
// B2: ONE common sampling session per entry / code generation
//===----------------------------------------------------------------------===//

/// Confirmed policy (spec §10, user-confirmed 2026-09-14): ONE common T1 per
/// entry/code generation, an aggregate budget of real admitted sample
/// executions shared by every admitted ready member (default 64, configurable),
/// not 64 per cell and not a representative-cell quota.
struct EJitSmallTableSamplingPolicy {
  uint64_t aggregateLimit = 64;
  /// Optional bound on the freeze wait; 0 means the caller decides.
  uint64_t freezeWaitMillis = 0;
  /// When true, `freeze` waits for admitted executions that are still in
  /// flight; a granted dispatch is not a completed sample (spec §10).
  bool waitForInFlightOnFreeze = true;
};

/// One admitted execution's ticket. Completing a ticket after the session was
/// frozen/cancelled is a stale callback: it is rejected and counted, never
/// merged into another generation's data.
struct EJitSmallTableSampleTicket {
  uint64_t sessionId = 0;
  uint64_t serial = 0;
  /// Every admitted T1 has a valid physical ticket through its actual leave.
  /// Quota-exhausted calls take AOT and receive no sampling ticket.
  bool valid = false;
  bool counted = false;
};

/// The immutable frozen profile bundle of one common session (spec §10):
/// entry/epoch/session identity, the contract hash and the ACTUAL table
/// resource address/generation the instrumented code was bound to, the number
/// of real admitted samples, and the synthesized profile buffer that the common
/// T2 consumes.
struct EJitSmallTableProfileBundle {
  std::string entryName;
  uint64_t codeGeneration = 0;
  uint64_t domainEpoch = 0;
  uint64_t sessionId = 0;
  uint64_t contractHash = 0;
  uintptr_t resourceAddress = 0;
  uint64_t resourceGeneration = 0;
  uint64_t sampleCount = 0;
  uint64_t participatingMembers = 0;
  std::string readinessProvider;
  std::vector<PgoCounterRef> counters;
  /// Synthesized from the real captured Tier-1 counters, not from a model.
  std::string profileData;
};

//===----------------------------------------------------------------------===//
// The runtime
//===----------------------------------------------------------------------===//

class EJitSmallTableRuntime {
public:
  struct Options {
    EJitSmallTableSamplingPolicy sampling;
    /// Bound on this generation's table payload (spec §8 capacity budget).
    uint64_t resourceCapacityLimit = 4u << 20;
    /// Bound on the bytes of generations kept alive for code that may still
    /// dispatch to them (spec §8 retention budget). A generation that would
    /// exceed it is refused instead of silently dropping an older one.
    uint64_t retentionCapacityLimit = 16u << 20;
    OptimizationLevel optLevel = OptimizationLevel::L2;
  };

  struct Stats {
    uint64_t plannedRows = 0;
    uint64_t readyRows = 0;
    uint64_t admittedMembers = 0;
    uint64_t compatibleMembers = 0;
    uint64_t extendableMembers = 0;
    uint64_t conflictMembers = 0;
    uint64_t unusableMembers = 0;
    uint64_t publishedRows = 0;
    uint64_t aotRefusals = 0;
    uint64_t acceptedSamples = 0;
    uint64_t rejectedSamples = 0;
    uint64_t staleCallbacks = 0;
    uint64_t generationChanges = 0;
    uint64_t generationsPrepared = 0;
    uint64_t migratedRows = 0;
    uint64_t retiredGenerations = 0;
    /// Retirements that had to wait for a physical reader of the generation to
    /// leave (spec §7 retained-old-generation rule). A deferred retirement is
    /// NOT a freed resource: the bytes stay accounted until the last reader
    /// returns, which is when the reclamation happens.
    uint64_t deferredRetirements = 0;
    /// Generations whose storage was really released after their last physical
    /// reader left (the safe-reclamation half of `deferredRetirements`).
    uint64_t reclaimedAfterReaders = 0;
  };

  ~EJitSmallTableRuntime();
  EJitSmallTableRuntime(const EJitSmallTableRuntime &) = delete;
  EJitSmallTableRuntime &operator=(const EJitSmallTableRuntime &) = delete;

  /// Create the runtime. \p Provider may be null: the runtime is then
  /// fail-closed and `prepare` refuses with "no readiness provider", so an
  /// entry can never be specialized on an unproven domain.
  /// Default-options convenience overload (defined out of line: the default
  /// argument cannot be written in-class because `Options` has default member
  /// initializers).
  static Expected<std::unique_ptr<EJitSmallTableRuntime>>
  create(const Config &Cfg, PeriodArrayRegistry &Registry, EJitRuntimeState &State,
         std::shared_ptr<EJitSmallTableReadinessProvider> Provider);

  static Expected<std::unique_ptr<EJitSmallTableRuntime>>
  create(const Config &Cfg, PeriodArrayRegistry &Registry, EJitRuntimeState &State,
         std::shared_ptr<EJitSmallTableReadinessProvider> Provider, Options Opts);

  /// Plan the entry from the provider's confirmed-ready members under a
  /// protected borrow, allocate the shared resource and declare its columns in
  /// \p M (runtime-owned storage). The module is serialized as this code
  /// generation's source for the T1/T2 compiles. Refuses on: no provider, no
  /// borrow, empty/invalid member set, an unproven projection, or a resource
  /// above the capacity budget.
  Expected<const EJitSmallTablePlan *>
  prepare(Module &M, StringRef EntryName, StringRef SourceVarName,
          ArrayRef<EJitSmallTableDim> Dims, std::string &Error);

  /// ONE coalesced new generation for a member set the current generation cannot
  /// serve (spec §6.6/§8): re-plan over the union of every member this runtime
  /// already admitted (their dependency identity is preserved even when an axis
  /// or column vanishes) plus \p ExtraMembers, allocate a new resource with
  /// generation+1, migrate every still-valid member by re-reading the protected
  /// source and publishing it into the new resource, and start a fresh common
  /// T1/T2 cycle on a freshly parsed copy of this entry's source module. Only the
  /// fields whose values actually changed widen: the solver runs per field.
  ///
  /// The previous resource is retained, with its bytes accounted against the
  /// retention budget, so code already compiled against it stays valid until
  /// `retireGenerationsUpTo`. Refuses when a member execution is still in
  /// flight, when a prepared generation has not been compiled yet, when the
  /// provider moved or no longer confirms a member of the union, or when the
  /// resource/retention budget would be exceeded.
  Expected<const EJitSmallTablePlan *>
  beginNextGeneration(ArrayRef<EJitSmallTableRowKey> ExtraMembers,
                      std::string &Error);

  /// Release the storage of generations `<= Generation` for code that can no
  /// longer dispatch to them. A generation above the current one is refused
  /// (returns false) rather than freeing live storage.
  ///
  /// PHYSICAL READER PROTECTION. A generation whose columns a real execution is
  /// still reading is NOT freed here: the retirement is recorded as deferred and
  /// the storage is reclaimed by `reclaimRetiredGenerations` when the last
  /// reader of that generation leaves. Logical cancellation is not proof of
  /// physical completion, so an execution that was cancelled mid-flight still
  /// holds its generation until its own leave arrives. Returns false only for a
  /// generation above the current one.
  bool retireGenerationsUpTo(uint64_t Generation);

  /// Free every generation whose retirement was deferred and whose last physical
  /// reader has since left. Idempotent and safe to call from any bookkeeping
  /// point (a leave, a cancel, a teardown). Returns the number of generations
  /// released.
  uint64_t reclaimRetiredGenerations();

  /// Generations <= some retired generation whose storage is still held only
  /// because a physical reader is inside them. Empty when nothing is deferred.
  uint64_t pendingRetireGenerationCount() const { return retired_.size(); }
  /// Bytes held for retired generations that a reader is still inside.
  uint64_t pendingRetireBytes() const;
  /// Physical readers currently inside \p Generation (real executions that took
  /// a lease and have not left). 0 means the generation may be reclaimed.
  uint64_t physicalReaders(uint64_t Generation) const;
  /// Every generation with at least one physical reader (ascending).
  std::vector<uint64_t> physicallyReadGenerations() const;

  /// Compile the common instrumented T1 through the real engine, verify that the
  /// compiled code bound this resource (not a same-named foreign table), publish
  /// every admitted ready member's rows and capture the real counter addresses.
  Expected<void *> compileCommonT1(uint64_t CodeGeneration, std::string &Error);

  /// Validate one member against the exported contract (B1 §6.6 steps 2-4) and
  /// publish its rows when the projection is new. `Compatible`/`Extendable` mean
  /// the member may dispatch to the specialized code; `Conflict` and `Unusable`
  /// leave it AOT.
  EJitSmallTableAdmission admitMember(ArrayRef<uint64_t> Indices,
                                      std::string *Why = nullptr);

  /// Record one REAL execution of the admitted specialized entry for \p Indices.
  /// Returns false when the call must take the AOT path instead: the member was
  /// never admitted, is not ready, the quota is exhausted, the session is
  /// closed/stale, or the code
  /// has no admitted rows. A call that is not admitted never consumes budget.
  ///
  /// The first physical execution of a session takes the session's protected read
  /// borrow and holds it until the session freezes or is cancelled, so the
  /// instrumented entry is always called while the configuration generation is
  /// read-protected (spec §6.1.1/§10). If no borrow can be taken the call is
  /// refused and takes the AOT path: no borrow, no specialization consumption.
  bool enterAdmitted(ArrayRef<uint64_t> Indices, EJitSmallTableSampleTicket *Ticket,
                     std::string *Why = nullptr);

  /// Complete a previously admitted execution. A ticket whose session is no
  /// longer current is a stale callback: it is rejected and counted.
  void leaveAdmitted(const EJitSmallTableSampleTicket &Ticket);

  uint64_t sampleCount() const { return stats_.acceptedSamples; }
  /// Real admitted samples of the CURRENT entry/code generation. The aggregate
  /// budget is per generation: a compatible late member does not restart it,
  /// while a replacement generation gets its own quota.
  uint64_t currentSessionSamples() const { return sessionSamples_; }
  uint64_t sampleBudget() const { return options_.sampling.aggregateLimit; }
  bool samplingExhausted() const {
    return sessionSamples_ >= options_.sampling.aggregateLimit;
  }
  /// Real executions that were entered through the sampling session and have not
  /// returned (the freezing barrier). A cancel does not zero this: the calls are
  /// still running.
  uint64_t inFlight() const { return inFlight_; }
  /// The CURRENT (open) session's granted-and-not-completed executions. This is
  /// what blocks replacing the generation being sampled (`beginNextGeneration`)
  /// and what a freeze has to drain. Executions a cancel already gave up on are
  /// not part of it: they read the retained old resource and may keep running
  /// while a new generation is prepared.
  uint64_t sessionInFlight() const { return sessionInFlight_; }
  /// Physical readers of the table storage, across every session and tier. A
  /// generation with a non-zero `physicalReaders` count may not be freed.
  uint64_t physicalInFlight() const { return inFlight_; }
  bool sessionOpen() const { return sessionOpen_; }
  uint64_t sessionId() const { return sessionId_; }
  /// True while the session holds the protected read borrow that guards the
  /// source region for the whole sampling window. A cancelled session keeps the
  /// borrow until its last real execution returns (physical protection), so
  /// `samplingProtected()` can stay true after `cancel` while `inFlight() != 0`.
  bool samplingProtected() const {
    return sessionBorrow_ != nullptr || !retiredSessionBorrows_.empty();
  }

  /// Freeze ONE immutable profile bundle for the common session. Requires the
  /// aggregate budget to be reached (or \p Force) and, when the policy asks for
  /// it, waits for admitted executions that are still in flight: a grant is not
  /// a completed sample. The bundle carries the real synthesized profile.
  Expected<const EJitSmallTableProfileBundle *> freeze(std::string &Error,
                                                       bool Force = false);

  /// Compile the common T2 from the frozen bundle and return its entry pointer.
  /// Verifies that T2 read the SAME resource address/generation as T1; a T2 that
  /// bound a different table is refused instead of published.
  Expected<void *> compileCommonT2(std::string &Error);

  /// Cancel/invalidate the session (timeout, configuration change, failure):
  /// the borrow is invalidated, later admissions take AOT and callbacks that
  /// arrive afterwards are rejected. Nothing already published is rewritten.
  void cancel(StringRef Reason);

  /// A configuration-generation change observed by the provider: the current
  /// session is terminated and the next generation must be prepared afresh.
  void noteGenerationChange(StringRef Reason);

  // Accessors (plan/contract/resource/identity/statistics).
  const EJitSmallTablePlan *plan() const { return plan_.get(); }
  const EJitSmallTableContract &contract() const { return contract_; }
  EJitSmallTableTableResource *resource() const { return resource_.get(); }
  const EJitSmallTableProfileBundle *bundle() const { return bundle_.get(); }
  const Stats &stats() const { return stats_; }
  StringRef providerLabel() const { return providerLabel_; }
  StringRef cancellationReason() const { return cancellationReason_; }
  EJitOrcEngine &engine() const { return *engine_; }
  uint64_t resourceGeneration() const {
    return resource_ ? resource_->generation() : 0;
  }
  size_t retainedGenerationCount() const { return retained_.size(); }
  uint64_t retainedBytes() const { return retainedBytes_; }
  /// Every (indices, admission) pair this runtime currently tracks, so a caller
  /// can see which members must stay AOT.
  ArrayRef<std::pair<std::vector<uint64_t>, EJitSmallTableAdmission>>
  admittedMembers() const {
    return admitted_;
  }

private:
  /// The runtime owns sampling readers through exact execution tickets; the
  /// host owns published T2 readers through its execution records. These
  /// primitives stay private so arbitrary callers cannot drop another call's
  /// protection without delivering that execution's actual completion.
  friend class EJitSmallTableHost;
  friend struct EJitSmallTableCounterCaptureTestAccess;

  EJitSmallTableRuntime() = default;

  /// Publish \p Indices' rows if the contract admits them; returns the
  /// admission and records publication.
  EJitSmallTableAdmission admitAndPublish(ArrayRef<uint64_t> Indices,
                                          StringRef BoundReason,
                                          std::string *Why);

  /// Plan \p Rows over \p M, allocate the generation's shared resource, export
  /// the contract and declare the resource columns in \p M. Shared by
  /// `prepare` (generation 1) and `beginNextGeneration`.
  Expected<const EJitSmallTablePlan *>
  buildGeneration(Module &M, ArrayRef<EJitSmallTableRowKey> Rows,
                  uint64_t Generation, std::string &Error);

  /// Take one physical read lease on \p Generation for a real execution whose
  /// code reads that generation's column storage.
  void acquireReader(uint64_t Generation);
  /// Drop one physical read lease and reclaim anything it was holding back.
  void releaseReader(uint64_t Generation);
  /// Erase every retained resource `<= Generation` immediately (no reader).
  void freeRetainedUpTo(uint64_t Generation);
  /// Free \p Generation's retained resource when its last reader left.
  void freeIfUnread(uint64_t Generation);
  /// Release the sampling window's protected read borrow once no real execution
  /// is still inside the window (cancel/failure path).
  void releaseSessionBorrowIfDrained();

  Config config_;
  PeriodArrayRegistry *registry_ = nullptr;
  EJitRuntimeState *state_ = nullptr;
  std::shared_ptr<EJitSmallTableReadinessProvider> provider_;
  Options options_;

  std::unique_ptr<EJitOrcEngine> engine_;
  /// The specialization context must outlive every materialization the engine
  /// may still trigger for this code generation, so the runtime owns it.
  std::unique_ptr<SpecializationContext> activeCtx_;
  std::shared_ptr<const EJitSmallTablePlanSet> planSet_;
  std::shared_ptr<const EJitSmallTablePlan> plan_;
  EJitSmallTableContract contract_;
  std::unique_ptr<EJitSmallTableTableResource> resource_;
  /// The entry's source module bitcode as it was BEFORE this pass declared the
  /// columns: `beginNextGeneration` re-parses it, so a new generation never
  /// inherits a previous generation's declarations.
  std::string sourceBitcode_;
  std::string bitcode_;
  /// Storage for the re-parsed module of the generation being prepared.
  std::unique_ptr<LLVMContext> generationContext_;
  std::unique_ptr<Module> generationModule_;
  /// Generations kept alive for code that may still dispatch to them, with the
  /// bytes they account for (spec §8 retention budget).
  std::vector<std::unique_ptr<EJitSmallTableTableResource>> retained_;
  uint64_t retainedBytes_ = 0;
  /// Generations `<= some retired generation` that could not be freed because a
  /// real execution was still reading them, with the number of such readers.
  /// The resource stays in `retained_` (and its bytes stay accounted) until the
  /// last reader leaves; `reclaimRetiredGenerations` then releases it.
  std::map<uint64_t, uint64_t> retired_;
  /// Physical readers per generation, across both T1 and T2. This remains
  /// independent of the current sampling-session barrier after cancellation.
  /// Absent key means zero readers.
  std::map<uint64_t, uint64_t> readers_;
  /// True between preparing generation N+1 and compiling it.
  bool pendingGeneration_ = false;
  uint64_t codeGeneration_ = 0;
  uint64_t domainEpoch_ = 0;
  std::string providerLabel_;
  uintptr_t boundResourceAddress_ = 0;

  /// Real Tier-1 counter identity captured from the common T1 compile: the PGO
  /// function names (owned, because PgoCounterRef borrows the char pointer) and
  /// the resolved __profc_/__profd_ addresses.
  std::vector<std::string> counterNames_;
  std::vector<std::pair<uintptr_t, uintptr_t>> counterAddrs_;
  /// Private, one-shot fault-injection seam for the actual ORC capture tests.
  /// Empty in production; it cannot replace counters or the lookup result.
  std::function<void(EJitOrcEngine &, uint64_t, ArrayRef<std::string>)>
      beforeCounterCaptureForTesting_;

  /// Members already admitted for dispatch, keyed by their coordinate.
  std::vector<std::pair<std::vector<uint64_t>, EJitSmallTableAdmission>>
      admitted_;
  /// Coordinates that actually produced an admitted sample (coverage report).
  std::vector<std::vector<uint64_t>> sampleKeys_;

  uint64_t sessionId_ = 0;
  uint64_t nextTicketSerial_ = 1;
  struct SamplingExecution {
    uint64_t sessionId = 0;
    uint64_t resourceGeneration = 0;
  };
  /// Exact physical executions. A duplicate/stale completion must not close
  /// another execution or decrement a replacement session's barrier.
  std::map<uint64_t, SamplingExecution> samplingExecutions_;
  /// PHYSICAL in-flight count: every real execution entered through the sampling
  /// session that has not returned. `cancel` never zeroes it; only the
  /// executions' own `leaveAdmitted` decrements it.
  uint64_t inFlight_ = 0;
  /// The CURRENT open session's share of `inFlight_`: executions whose completing
  /// callback still belongs to it. A cancel lowers it to zero (the session that
  /// granted them is over) while `inFlight_` keeps counting them physically, so a
  /// cancelled session's late executions neither block a rebuild nor block a
  /// freeze of the NEXT session.
  uint64_t sessionInFlight_ = 0;
  /// Protected read borrow held for the whole sampling window (taken by the
  /// first counted execution, released by freeze/cancel). `samplingProtected()`
  /// reports it; a null value means the session is not reading the source under
  /// a provider guarantee, which is why a sample then refuses.
  std::unique_ptr<EJitSmallTableReadBorrow> sessionBorrow_;
  /// Cancelled sessions may still execute while a replacement session opens.
  /// Each old session keeps its OWN protected read until its last real leave.
  std::map<uint64_t, std::unique_ptr<EJitSmallTableReadBorrow>>
      retiredSessionBorrows_;
  /// Admitted samples of the current generation only (the aggregate budget is
  /// per entry/code generation; `stats_.acceptedSamples` stays cumulative).
  uint64_t sessionSamples_ = 0;
  bool sessionOpen_ = false;
  bool frozen_ = false;
  std::string cancellationReason_;
  std::unique_ptr<EJitSmallTableProfileBundle> bundle_;
  Stats stats_;

#ifndef EJIT_FREESTANDING
  mutable std::mutex mutex_;
  std::condition_variable inFlightCV_;
#endif
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLERUNTIME_H
