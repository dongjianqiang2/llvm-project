//===-- EJitSmallTableHost.h - normal-path small-table integration --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// THE NORMAL PATH. `EJitSmallTableRuntime` is the mechanism: it plans, owns the
// shared table resource, publishes validated rows, samples one aggregate budget
// and compiles the common T1/T2. This class is the integration layer that the
// running application actually talks to, and which the ordinary dispatch path
// consults:
//
//   application request  -> requestEntry()          (server facts + dim binding)
//   application dispatch -> dispatch() / enter()    (admission + slot + tier)
//   application return   -> leave()                 (in-flight + protected read)
//
// Everything here is driven by the product's own request/dispatch calls, never
// by a test-only entry: `dispatch` performs the real JIT call and `enter`/`leave`
// are the two hooks the AOT wrapper calls around that call
// (`ejit_stab_enter` / `ejit_stab_leave`, emitted by EJitWrapperGen). The
// taskpool compile_or_get entries consult `gate()` so a call site can never be
// handed a callable specialized pointer without an admitted, published slot.
//
// Readiness binding. `EJitSmallTableFactSource` is the seam for the product
// configuration transaction. The commit/ready boundary is explicit and is
// carried here as five separate facts, each of which must hold:
//   * legal domain  - `coversDeclaredDomain()`: the confirmed set covers every
//                     reachable dependency of the declared extents;
//   * dependency    - the runtime re-evaluates every candidate field of every
//                     reachable row against the contract before publication;
//   * epoch         - `domainEpoch()` != 0 and `epochCurrent(epoch)`;
//   * lifetime      - `borrow()` grants a protected read; without it nothing is
//                     read, published or dispatched;
//   * revision      - compareRevision()/notePublishedRevision(): the product's
//                     own commit counter, so a new generation is noticed
//                     without inventing one.
// A missing, stale or partial fact fails CLOSED: the entry stays AOT.
//
// `EJitSmallTableHostFactSource` below is the explicitly labeled HOST adapter
// (it derives from the existing `EJitSmallTableHostProvider`), never product
// readiness. The exact remaining external binding is recorded in RESULT.md:
// implement `EJitSmallTableFactSource` over the product's configuration
// completion point and install it with `setFactSource` - without changing any
// other code in this file.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLEHOST_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLEHOST_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTable.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableRuntime.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace llvm {

class Module;

namespace ejit {

class EJitOrcEngine;
class EJitRuntimeState;
class PeriodArrayRegistry;

//===----------------------------------------------------------------------===//
// The product fact-source seam
//===----------------------------------------------------------------------===//

/// The product-side readiness facts, as the host consumes them.
///
/// A product binding implements this over the configuration transaction's
/// completion point. `EJitSmallTableReadinessProvider` remains the mechanism the
/// runtime was written against (so the component contract is unchanged); this
/// interface adds exactly the two facts a product commit boundary has and a
/// borrow-based provider cannot express on its own: the commit revision and the
/// "this generation's data is now published" callback.
class EJitSmallTableFactSource : public EJitSmallTableReadinessProvider {
public:
  /// Monotonic revision of the configuration commit this fact source currently
  /// describes. 0 means "no revision information", which the host records and
  /// treats as unverifiable.
  virtual uint64_t configurationRevision() const = 0;
  /// Called after the host published validated rows for \p Revision. The
  /// product can use it to record that its data reached the shared resource.
  virtual void notePublishedRevision(uint64_t Revision, uint64_t RowCount) {
    (void)Revision;
    (void)RowCount;
  }
};

/// Explicitly labeled HOST fact source (NOT product readiness).
///
/// It is the existing labeled host provider plus the revision fact, so the whole
/// normal path can be driven on a host where the product configuration
/// transaction is unavailable. Its label
/// (`host-adapter.pr231-not-product`) is recorded in the plan and the contract,
/// so no artifact of this run can be mistaken for a product readiness claim.
class EJitSmallTableHostFactSource final : public EJitSmallTableFactSource {
public:
  static constexpr const char *Label = EJitSmallTableHostProvider::Label;

  EJitSmallTableHostFactSource(std::string SourceVarName, const void *Base,
                               uint64_t Bytes, uint64_t Epoch,
                               uint64_t Revision = 1);

  // The host provider surface is reused unchanged.
  void addReadyMember(ArrayRef<uint64_t> Indices, uint64_t ConfigGeneration,
                      bool FieldsInitialized = true) {
    provider_.addReadyMember(Indices, ConfigGeneration, FieldsInitialized);
  }
  void invalidateGeneration();
  void refuseBorrow(std::string Reason) { provider_.refuseBorrow(std::move(Reason)); }
  void allowBorrow() { provider_.allowBorrow(); }
  void setCoverage(bool Covers) { provider_.setCoverage(Covers); }
  size_t outstandingBorrows() const { return provider_.outstandingBorrows(); }
  uint64_t borrowCount() const { return provider_.borrowCount(); }
  /// Move to a new configuration commit: the old facts stop being current.
  uint64_t bumpRevision() { return ++revision_; }

  StringRef label() const override { return Label; }
  uint64_t domainEpoch() const override { return provider_.domainEpoch(); }
  bool epochCurrent(uint64_t Epoch) const override {
    return provider_.epochCurrent(Epoch);
  }
  bool coversDeclaredDomain() const override {
    return provider_.coversDeclaredDomain();
  }
  ArrayRef<EJitSmallTableReadyMember> readyMembers() const override {
    return provider_.readyMembers();
  }
  std::unique_ptr<EJitSmallTableReadBorrow>
  borrow(StringRef SourceVarName, std::string &Error) override {
    return provider_.borrow(SourceVarName, Error);
  }
  void onBorrowReleased(EJitSmallTableReadBorrow *Borrow) override {
    provider_.onBorrowReleased(Borrow);
  }

  uint64_t configurationRevision() const override { return revision_; }
  void notePublishedRevision(uint64_t Revision, uint64_t RowCount) override {
    lastPublishedRevision_ = Revision;
    lastPublishedRows_ = RowCount;
  }
  uint64_t lastPublishedRevision() const { return lastPublishedRevision_; }
  uint64_t lastPublishedRows() const { return lastPublishedRows_; }

private:
  EJitSmallTableHostProvider provider_;
  uint64_t revision_ = 1;
  uint64_t lastPublishedRevision_ = 0;
  uint64_t lastPublishedRows_ = 0;
};

//===----------------------------------------------------------------------===//
// One logical slot per admitted member coordinate
//===----------------------------------------------------------------------===//

/// The logical publication slot of one admitted coordinate (spec §6.5):
/// `tableReady(row)` and `codeReady(code_id)` are tracked separately, and the
/// callable entry is written only when both hold AND the member is admitted AND
/// the runtime currently considers its execution eligible. Until then `entry`
/// is null, which is exactly what every publish path must refuse to expose.
struct EJitSmallTableLogicalSlot {
  /// The member's coordinate in the plan's declared dimension order.
  std::vector<uint64_t> coordinate;
  /// The runtime admission this slot was published with.
  EJitSmallTableAdmission admission = EJitSmallTableAdmission::Unusable;
  /// tableReady(row): this coordinate's rows were validated and published into
  /// the resource of `generation`.
  bool tableReady = false;
  /// codeReady(code_id): the code generation bound to `generation` was compiled
  /// from the frozen bundle, passed its resource-identity check and is
  /// dispatchable.
  bool codeReady = false;
  /// The code generation whose entry this slot exposes (0 while unpublished).
  uint64_t codeGeneration = 0;
  /// The resource generation the published rows belong to.
  uint64_t resourceGeneration = 0;
  /// The callable entry for this coordinate, or null when not published.
  void *entry = nullptr;

  bool published() const { return entry != nullptr; }
};

/// Why a dispatch did or did not take the specialized path. Every value except
/// `Dispatched` means the caller takes the ORIGINAL AOT path and nothing is
/// consumed from the sampling budget.
enum class EJitSmallTableDispatch : uint8_t {
  /// The real specialized entry was called (through `enter`/`leave`).
  Dispatched,
  /// No small-table entry is bound to this function index.
  NotBound,
  /// The entry is bound but not ready to run: no plan, no resource, no admitted
  /// member, no protected borrow, or no published slot. `why` says which.
  Aot,
  /// The member is admitted, but the function's dimensions are not an
  /// unambiguous coordinate of the plan's declared schema (e.g. a call that
  /// only supplies part of the dims), so dispatching would guess. Fail closed.
  CoordinateUnprovable,
  /// The callable entry itself is missing or null: an internal inconsistency
  /// that must stay AOT rather than call a dangling pointer.
  NoEntry,
};

/// One dispatch outcome.
struct EJitSmallTableDispatchResult {
  EJitSmallTableDispatch status = EJitSmallTableDispatch::NotBound;
  /// The real call's return value (only when `status == Dispatched`).
  int64_t value = 0;
  /// True when this execution was counted as one of the session's admitted
  /// samples (a published dispatch can still be uncounted once the aggregate
  /// budget is reached).
  bool counted = false;
  std::string why;
};

//===----------------------------------------------------------------------===//
// The normal-path integration object
//===----------------------------------------------------------------------===//

/// Application-facing registration, request and dispatch for one entry.
///
/// Lifecycle of one entry:
///   registerEntry (server dim/period binding + module)
///     -> driveSampling (or real application dispatch once published)
///     -> freeze + publishGeneration (T2 on the same resource)
///     -> dispatch (the ordinary application call; enters through the slot)
///     -> noteConfigurationChange / cancel / retireSlot on the failure paths.
class EJitSmallTableHost {
public:
  struct Options {
    EJitSmallTableRuntime::Options runtime;
    /// When true, `dispatch` refuses before `publishGeneration` succeeded: the
    /// slot is published only when BOTH tableReady and codeReady hold, which is
    /// the §6.5 rule. Tests may lower it to exercise the T1 window explicitly
    /// through `enter`, but a product configuration must leave it true.
    bool requireCodeReadyForDispatch = true;
    /// Aggregate budget for a sampling-only driver (the initial T1 window).
    /// The runtime owns the real (configurable) budget; this only bounds the
    /// host's own sampling driver. 0 means "use the runtime's budget".
    uint64_t samplingDriverLimit = 0;
  };

  /// The application request: bind an entry to a server-provided fact source
  /// and the module that declares it. Performs the whole normal request path
  /// (plan -> resource -> class binding -> T1), so a call site can immediately
  /// ask the entry to run.
  struct EntryRequest {
    /// Module that declares the entry and its source global. Serialized by the
    /// runtime as this code generation's source.
    Module *module = nullptr;
    std::string entryName;
    /// Dense funcIndex the AOT wrapper uses for this entry (from
    /// `ejit_register_funcindex`). The dispatch gate is keyed by it.
    uint32_t funcIndex = 0;
    std::string sourceVarName;
    ArrayRef<EJitSmallTableDim> dims;
    /// Period/lifecycle name the configuration server knows for dimension
    /// position \p i, outermost first. The host resolves each name to the
    /// wrapper's dimType through the process-global lifecycle registry and
    /// refuses if the mapping is missing: a coordinate that cannot be proven is
    /// never used to publish a slot.
    ArrayRef<std::string> dimPeriodNames;
    /// Configuration generation the plan/code belongs to. 0 lets the host use
    /// the fact source's epoch.
    uint64_t codeGeneration = 0;
  };

  ~EJitSmallTableHost();
  EJitSmallTableHost(const EJitSmallTableHost &) = delete;
  EJitSmallTableHost &operator=(const EJitSmallTableHost &) = delete;

  /// Production constructor: the host owns the runtime it drives.
  static Expected<std::unique_ptr<EJitSmallTableHost>>
  create(const Config &Cfg, PeriodArrayRegistry &Registry,
         EJitRuntimeState &State, std::shared_ptr<EJitSmallTableFactSource> Facts,
         Options Opts);

  /// Binding constructor for an already-constructed runtime. `*Runtime` must
  /// outlive this host. Used where the same runtime instance is shared with a
  /// caller that already owns it (and by the focused integration tests, which
  /// must exercise THIS host, not a test-only reimplementation of it).
  static Expected<std::unique_ptr<EJitSmallTableHost>>
  create(EJitSmallTableRuntime &Runtime,
         std::shared_ptr<EJitSmallTableFactSource> Facts, Options Opts);

  /// Install THIS host as the runtime's normal-path integration point and
  /// install \p Host as the process-global one the taskpool dispatch entries
  /// consult. Returns the previously installed host (usually null). Owned by
  /// the caller; the global registration holds no ownership.
  static EJitSmallTableHost *installGlobal(EJitSmallTableHost *Host);
  /// The process-global host, or null when the feature is OFF.
  static EJitSmallTableHost *global();

  /// Retract every published logical slot through the real invalidation path the
  /// owner installed (see `setInvalidationHook`), so a call site that already
  /// cached a specialized pointer cannot reach a drained generation. This is the
  /// publication side of `drainSlots`: without it the per-call-site cell would
  /// keep dispatching to code this host no longer publishes. Returns the number
  /// of slots retracted.
  uint64_t retractPublishedSlots();

  /// Install the owner's real invalidation path (the runtime drains the
  /// registered inline-cache cells through the shared taskpool). The normal
  /// product wiring installs it from `EJit`; a caller that does not install one
  /// gets the conservative fallback, which retracts nothing and says so through
  /// `retractionAvailable()`.
  void setInvalidationHook(std::function<void()> Hook) {
    invalidationHook_ = std::move(Hook);
  }
  bool retractionAvailable() const { return static_cast<bool>(invalidationHook_); }

  /// Register one extra external symbol the engine must resolve for this entry's
  /// compiles (a product-image runtime hook). The normal path already provides
  /// the instrumented-tier profile-runtime hook itself (see `compileT1`); this is
  /// for a product binding that has its own address for such a hook.
  void registerExtraSymbol(const std::string &Name, void *Address);

  /// Provide the instrumented tier's profile-runtime hook if it is not already
  /// registered: the tier-1 instrumentation pass lowers to a reference to
  /// `__llvm_profile_runtime`, which the product image defines, so a host
  /// without that image needs a stand-in before the common T1 compile. Public
  /// and idempotent so a caller that drives `compileT1`'s steps by hand (a
  /// scheduler, or a focused test of the rebuild path) can do the same thing the
  /// normal entry path does.
  void ensureProfileRuntimeHook();

  /// The sampling-window entry: admission, table readiness, the epoch re-check
  /// and the protected read borrow, but NOT code readiness, because the initial
  /// window runs on the instrumented tier before any final code exists. This is
  /// deliberately a separate, non-application entry point: the ordinary
  /// `dispatch`/`enter` (and the AOT wrapper hook) still require a published
  /// code generation, so no call site is ever exposed to instrumented code.
  /// Public because a product scheduler with no natural call traffic yet drives
  /// its own sampling through exactly this protocol.
  void *enterInstrumented(ArrayRef<uint32_t> DimTypes,
                          ArrayRef<uint32_t> InstanceIds, uint64_t *OutTicket,
                          std::string *Why);

  /// The instrumented-tier entry, for a caller that must drive its own sampling
  /// traffic (a scheduler with no natural call flow yet) through the same
  /// entered/left protocol the application uses. Application dispatch never
  /// uses this: `dispatch`/`enter` require a published code generation.
  void *instrumentedEntry() const { return t1Entry_; }

  //--- application request --------------------------------------------------

  /// Run the normal request path for one entry. `executableAot` is the
  /// original (baseline) entry the application already has; it is recorded as
  /// the AOT side of every decision, never called by the host itself.
  Error requestEntry(const EntryRequest &Request, void *executableAot,
                     std::string &Why);

  /// The request path in two observable steps, for callers that drive their own
  /// scheduler: plan + resource from the confirmed facts, then the common T1.
  Expected<const EJitSmallTablePlan *> planEntry(const EntryRequest &Request,
                                                 std::string &Why);
  Expected<void *> compileT1(std::string &Why);

  /// The application's publish step: freeze the ONE common session, compile the
  /// common T2 from the immutable bundle and publish one logical slot per
  /// admitted eligible coordinate. Refuses (leaving every slot un-published) if
  /// the session is not ready, the bundle is stale, T2 does not bind the SAME
  /// resource/generation, or the configuration revision moved.
  Error publishGeneration(std::string &Why);

  //--- application dispatch -------------------------------------------------

  /// The ordinary application call. `dimTypes`/`instanceIds` are the wrapper's
  /// real dimension arguments and `arg` is the entry's dynamic argument. The
  /// call derives the member coordinate, gates on admission + published slot,
  /// calls the real specialized entry through `enter`/`leave`, and reports what
  /// happened. Never guesses a coordinate: a call whose dims cannot be mapped
  /// unambiguously to the plan's schema stays AOT.
  EJitSmallTableDispatchResult
  dispatch(ArrayRef<uint32_t> dimTypes, ArrayRef<uint32_t> instanceIds,
           int64_t arg);

  /// The two hooks the AOT wrapper emits around the specialized dispatch
  /// (`ejit_stab_enter` / `ejit_stab_leave`). `enter` returns the callable entry
  /// when THIS execution may run specialized code - admission, table readiness,
  /// code readiness, eligibility and the published slot must all hold - and null
  /// otherwise, in which case the wrapper takes its AOT body. It records the
  /// execution as in flight and holds the session's protected read across it.
  void *enter(ArrayRef<uint32_t> dimTypes, ArrayRef<uint32_t> instanceIds,
              uint64_t *outTicket, std::string *Why);
  /// Complete an execution started by `enter`. A ticket from a session that has
  /// since been frozen/cancelled is a stale callback: it is rejected and
  /// counted, never merged into another generation.
  void leave(uint64_t ticket);

  //--- sampling driver (initial T1 window) ---------------------------------

  /// Drive real executions of the admitted members until the runtime's
  /// aggregate budget is reached or \p MaxExecutions is reached, using the same
  /// enter/leave path the application uses. Returns the number of real
  /// executions performed. This is a convenience for a product that has no
  /// natural call traffic yet; normal traffic does the same accounting.
  uint64_t driveSampling(uint64_t MaxExecutions, int64_t Arg);

  //--- configuration change / failure paths --------------------------------

  /// The product's configuration moved. Every published slot is drained
  /// immediately (no later call can reach the old code), the current session is
  /// cancelled, and the new revision is recorded so the next request rebuilds.
  void noteConfigurationChange(StringRef Reason);
  /// Cancel the current session and drain every slot (timeout/queue failure).
  void cancel(StringRef Reason);
  /// Asynchronously prepare the coalesced next generation over the union of the
  /// members this host already admitted plus \p ExtraMembers: new resource
  /// generation, migrated rows, a fresh common session. Publishing it stays
  /// `publishGeneration`'s job, so no slot is exposed before the new code is
  /// ready.
  Error beginNextGeneration(ArrayRef<EJitSmallTableRowKey> ExtraMembers,
                            std::string &Why);
  /// Release the resources of every generation `<= Generation`. Refuses while a
  /// slot published from such a generation is still reachable, or while an
  /// execution is in flight.
  bool retireGenerationsUpTo(uint64_t Generation, std::string &Why);

  //--- product lifecycle boundary ------------------------------------------

  /// The product activated lifecycle \p PeriodName at instance \p InstanceId.
  /// When this entry declares that lifecycle as a dimension, the member's
  /// coordinate is derived, validated against the exported contract and its
  /// logical slot is published if the code generation is ready. Returns false
  /// (leaving the entry AOT) when activation does not prove this member ready:
  /// the period is not a declared dimension, the member was never admitted, the
  /// contract classifies it Unusable/Conflict, or no code is published.
  ///
  /// This is the product's own activation call - never a readiness inference:
  /// the admission still comes from the configuration-commit fact source.
  bool onProductActivated(StringRef PeriodName, uint32_t InstanceId,
                          std::string *Why = nullptr);
  /// The product deactivated a lifecycle instance whose coordinate owns a slot:
  /// the slot stops being published. Published ROWS are never rewritten, and
  /// unrelated members/entries are untouched.
  bool onProductDeactivated(StringRef PeriodName, uint32_t InstanceId);

  //--- dispatch by member coordinate ---------------------------------------

  /// Dispatch \p Coordinate through the ordinary request path (admission +
  /// published slot + enter/leave + real call). Used where the caller has the
  /// coordinate (a scheduler iterating members) rather than a wrapper's dim
  /// arguments.
  EJitSmallTableDispatchResult dispatchMember(ArrayRef<uint64_t> Coordinate,
                                              int64_t Arg);
  /// The configuration's own admitted member set, in slot-table order. Lets a
  /// caller or a sampling driver drive the real dispatched calls instead of
  /// inventing coordinates.
  std::vector<std::vector<uint64_t>> admittedCoordinates() const;

  //--- observation ----------------------------------------------------------

  EJitSmallTableRuntime &runtime() const { return *runtime_; }
  bool bound() const { return bound_; }
  const std::shared_ptr<EJitSmallTableFactSource> &facts() const {
    return facts_;
  }
  StringRef entryName() const { return entryName_; }
  uint32_t funcIndex() const { return funcIndex_; }
  /// The declared dimensions of the bound entry, outermost first.
  ArrayRef<EJitSmallTableDim> dims() const { return dims_; }
  /// The dimType slot each declared dimension was resolved to.
  ArrayRef<uint32_t> dimTypes() const { return dimTypes_; }
  /// The lifecycle name each declared dimension was bound to.
  ArrayRef<std::string> dimPeriodNames() const { return dimPeriodNames_; }
  StringRef emptyReason() const { return emptyReason_; }
  /// The reason the most recent lifecycle/refusal path recorded. Diagnostic
  /// only; it carries no state a decision depends on.
  StringRef lastRefusal() const { return lastRefusal_; }
  bool codeReady() const { return codeReady_; }
  uint64_t publishedCodeGeneration() const { return publishedCodeGeneration_; }
  uint64_t publishedResourceGeneration() const { return publishedResourceGeneration_; }
  uint64_t sampleCount() const { return runtime_->sampleCount(); }
  uint64_t inFlight() const { return runtime_->inFlight(); }
  /// Published executions that are currently running (entered, not left). A
  /// generation may not be retired while this is non-zero.
  uint64_t activeExecutions() const { return activeExecutions_; }
  bool samplingProtected() const { return runtime_->samplingProtected(); }
  const EJitSmallTableProfileBundle *bundle() const { return runtime_->bundle(); }
  /// The plan this entry is bound to, or null.
  const EJitSmallTablePlan *plan() const { return runtime_->plan(); }
  /// Every logical slot this host tracks (eligible and not).
  ArrayRef<EJitSmallTableLogicalSlot> slots() const { return slots_; }
  /// Number of slots with a callable entry.
  uint64_t publishedSlots() const;
  /// The slot for \p Coordinate, or null when the coordinate has no slot.
  const EJitSmallTableLogicalSlot *findSlot(ArrayRef<uint64_t> Coordinate) const;
  /// Whether `dispatch` would take the specialized path for \p Coordinate
  /// without calling anything. Read-only; does not enter the session.
  bool wouldDispatch(ArrayRef<uint64_t> Coordinate, std::string *Why = nullptr) const;
  /// The dispatch gate the ordinary taskpool compile_or_get entries consult.
  /// `false` means: do not hand this call site a specialized pointer, do not
  /// fill its inline-cache cell, take the AOT path. It can only ever refuse, and
  /// a function index this host does not own is never refused (so one host
  /// bound to one entry leaves every other entry's dispatch untouched).
  bool isBoundTo(uint32_t FuncIndex) const {
    return bound_ && FuncIndex == funcIndex_;
  }
  bool wouldDispatchCall(ArrayRef<uint32_t> DimTypes,
                         ArrayRef<uint32_t> InstanceIds,
                         std::string *Why = nullptr) const;
  /// The real AOT baseline entry recorded by `requestEntry` (never called by
  /// the host; callers use it for the AOT path and for equivalence checks).
  void *aotEntry() const { return aotEntry_; }
  /// Count of dispatches that took the AOT path, by reason class.
  uint64_t aotDispatchCount() const { return aotDispatchCount_; }
  uint64_t unprovableCoordinateCount() const { return unprovableCoordinateCount_; }
  uint64_t staleLeaveCount() const { return staleLeaveCount_; }
  /// Times a slot was drained because its session/generation stopped being
  /// current (never because a value changed).
  uint64_t drainedSlotCount() const { return drainedSlotCount_; }
  /// The tier the currently exposed entry belongs to: "final" after a
  /// successful `publishGeneration`, "instrumented" during the initial sampling
  /// window, "none" before T1. Both tiers are bound to the SAME table resource
  /// generation; the published bundle carries that identity.
  StringRef activeTier() const;
  /// The real callable entry currently exposed, or null.
  void *activeEntry() const;

  //--- measurable byte accounting (recorded separately by the caller) -------

  struct ByteAccounting {
    /// Column payload of the current resource generation.
    uint64_t tablePayloadBytes = 0;
    /// Capacity actually reserved for the current resource generation.
    uint64_t tableAllocatedBytes = 0;
    /// Bytes whose values are immutable from now on.
    uint64_t tablePublishedBytes = 0;
    /// Bytes of resources retained for code that may still dispatch to them.
    uint64_t retainedBytes = 0;
    /// Number of retained generations.
    uint64_t retainedGenerations = 0;
    /// Bytes of JIT code this host published entries for, when the engine can
    /// report a code range for the entry (code-pool builds); 0 otherwise.
    uint64_t publishedCodeBytes = 0;
  };
  ByteAccounting accounting() const;

private:
  EJitSmallTableHost() = default;

  /// Rebuild the slot table from the runtime's admissions for the current
  /// resource generation. Called after every successful publication.
  void rebuildSlots();
  /// Null the callable entry of every slot without touching published rows or
  /// the resource (a drain is a publication decision, not a data change).
  void drainSlots(StringRef Reason);
  /// Resolve every declared dimension to the wrapper's dimType slot by
  /// lifecycle name. False (with \p Why) when a name was never registered.
  bool resolveDimTypes(std::string &Why);
  /// Map one real call's dim pairs onto the plan's declared coordinate, in plan
  /// order. False when a declared dimension is absent, duplicated or ambiguous.
  bool coordinateOf(ArrayRef<uint32_t> DimTypes, ArrayRef<uint32_t> InstanceIds,
                    SmallVectorImpl<uint64_t> &Out, std::string *Why) const;
  /// Shared gate used by `dispatch` and `enter`.
  EJitSmallTableDispatch
  gateFor(ArrayRef<uint64_t> Coordinate, const EJitSmallTableLogicalSlot **SlotOut,
          std::string &Why) const;
  /// Enter one PUBLISHED execution: the admitted, published coordinate's code is
  /// already compiled, so this neither needs nor consumes the sampling session
  /// (which is frozen by then) and never affects the immutable bundle. What it
  /// does keep is the protected read: the borrow is taken for the execution and
  /// released by `leavePublished`, so every published call runs under the
  /// configuration guarantee even if the product's configuration changes while
  /// the call is running. `false` means the caller must take the AOT path.
  bool enterPublished(ArrayRef<uint64_t> Coordinate, uint64_t *OutToken,
                      std::string &Why);
  void leavePublished(uint64_t Token);
  /// Close every execution still entered (cancel / configuration change /
  /// generation replacement): their protected reads are released and their
  /// tokens become stale, so a completion that arrives later is counted and
  /// never applied to the generation that follows.
  void closeOutstandingExecutions();
  /// Build one dispatch result without calling anything.
  EJitSmallTableDispatchResult refuse(EJitSmallTableDispatch Status,
                                      std::string Why);

  EJitSmallTableRuntime *runtime_ = nullptr;
  std::unique_ptr<EJitSmallTableRuntime> ownedRuntime_;
  std::shared_ptr<EJitSmallTableFactSource> facts_;
  Options options_;

  bool bound_ = false;
  std::string entryName_;
  uint32_t funcIndex_ = 0;
  std::string sourceVarName_;
  SmallVector<EJitSmallTableDim, 4> dims_;
  SmallVector<std::string, 4> dimPeriodNames_;
  SmallVector<uint32_t, 4> dimTypes_;
  uint64_t codeGeneration_ = 0;
  void *aotEntry_ = nullptr;

  bool planReady_ = false;
  bool t1Ready_ = false;
  bool codeReady_ = false;
  void *t1Entry_ = nullptr;
  void *t2Entry_ = nullptr;
  uint64_t publishedCodeGeneration_ = 0;
  uint64_t publishedResourceGeneration_ = 0;
  uint64_t factRevision_ = 0;
  std::string emptyReason_;
  /// Diagnostic record of the last refusal reason on a lifecycle path.
  std::string lastRefusal_;

  std::vector<EJitSmallTableLogicalSlot> slots_;
  /// Coordinates the product's lifecycle boundary drained (a deactivated
  /// instance). They stay unpublished until the coordinate is re-admitted or a
  /// new generation is prepared; a rebuild of the slot table must not silently
  /// re-publish them.
  std::vector<std::vector<uint64_t>> deactivated_;
  uint64_t publishedSlotsCache_ = 0;

  uint64_t aotDispatchCount_ = 0;
  uint64_t unprovableCoordinateCount_ = 0;
  uint64_t staleLeaveCount_ = 0;
  uint64_t drainedSlotCount_ = 0;
  /// Published executions currently entered but not left, and the token serial
  /// handed to each. Kept by the host because a published execution is not a
  /// sampling session sample.
  uint64_t activeExecutions_ = 0;
  uint64_t nextExecutionToken_ = 1;
  /// One entered execution: the ticket that must be closed and the protected
  /// read that must be released, in entry order.
  struct ExecutionRecord {
    uint64_t token = 0;
    bool isSample = false;
    EJitSmallTableSampleTicket ticket;
    std::unique_ptr<EJitSmallTableReadBorrow> borrow;
  };
  std::vector<ExecutionRecord> executions_;
  /// Tokens of executions that a cancel/config-change closed while they were
  /// still entered. A later completion with one of these tokens is a stale
  /// callback: it is counted and never applied to a newer generation.
  std::vector<uint64_t> cancelledTokens_;
  /// Serial of the sampling session the sampling tickets belong to, captured
  /// when they are handed out so a completion after a session change is
  /// recognized as stale.
  uint64_t samplingSessionAtEnter_ = 0;
  /// The owner's real invalidation path (inline-cache cell drain). Empty when
  /// no owner installed one; `retractionAvailable()` reports which.
  std::function<void()> invalidationHook_;
  /// Names this host already registered as extra engine symbols, so a
  /// product-supplied address is never replaced by the host stand-in.
  std::vector<std::string> extraSymbols_;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLEHOST_H