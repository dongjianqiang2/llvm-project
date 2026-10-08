//===-- EJitSmallTableHost.cpp - normal-path small-table integration ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See EJitSmallTableHost.h for the contract. The rules this file enforces, in
// the order a call meets them:
//
//   1. A request without a fact source, without a domain epoch, without a
//      protected borrow, without a confirmed-ready member, or with a dimension
//      whose lifecycle name was never registered fails CLOSED: no plan, no
//      resource, no slot, and the entry stays AOT.
//   2. A coordinate is only ever derived from the plan's declared dimensions
//      resolved to the wrapper's real dimType slots. A call that does not carry
//      every declared dimension is unprovable and stays AOT - the host never
//      guesses which row a call belongs to.
//   3. A slot's callable entry appears only after tableReady AND codeReady AND
//      admission AND eligibility all hold, and disappears (drained) the moment
//      the session, epoch or generation stops being current. Published rows are
//      never rewritten.
//   4. Every real execution goes through enter/leave, so the protected read
//      borrow covers actual execution and an in-flight execution blocks freeze
//      and retirement instead of being silently dropped.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSmallTableHost.h"
#include "EJitOwnerWorkerContext.h"
#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "EJitWrapperRuntimeTestAccess.h"
#include "EJitSmallTableSreBridgeInternal.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#ifndef EJIT_FREESTANDING
#include <mutex>
#endif

namespace llvm {
namespace ejit {

//===----------------------------------------------------------------------===//
// Host fact source
//===----------------------------------------------------------------------===//

EJitSmallTableHostFactSource::EJitSmallTableHostFactSource(
    std::string SourceVarName, const void *Base, uint64_t Bytes, uint64_t Epoch,
    uint64_t Revision)
    : provider_(std::move(SourceVarName), Base, Bytes, Epoch),
      revision_(Revision) {}

void EJitSmallTableHostFactSource::invalidateGeneration() {
  provider_.invalidateGeneration();
  ++revision_;
}

//===----------------------------------------------------------------------===//
// Construction
//===----------------------------------------------------------------------===//

EJitSmallTableHost::~EJitSmallTableHost() {
  // A host is destroyed only when no real execution is inside it (the owner
  // teardown parks it otherwise), but a caller that deletes it directly must
  // still not leave a generation reader or a protected borrow behind: release
  // every remaining lease so a retained generation can be reclaimed.
  for (ExecutionRecord &Rec : executions_) {
    if (Rec.isSample && runtime_)
      runtime_->leaveAdmitted(Rec.ticket);
    if (Rec.borrow) {
      Rec.borrow->release();
      Rec.borrow.reset();
    }
    if (!Rec.isSample && Rec.resourceGeneration != 0 && runtime_) {
      runtime_->releaseReader(Rec.resourceGeneration);
      Rec.resourceGeneration = 0;
    }
  }
  executions_.clear();
  activeExecutions_ = 0;
}

Expected<std::unique_ptr<EJitSmallTableHost>> EJitSmallTableHost::create(
    const Config &Cfg, PeriodArrayRegistry &Registry, EJitRuntimeState &State,
    std::shared_ptr<EJitSmallTableFactSource> Facts, Options Opts) {
  if (!Facts) {
    return make_error<StringError>(
        "small-table host: no fact source installed, refusing to specialize an "
        "unproven domain",
        inconvertibleErrorCode());
  }
  std::shared_ptr<EJitSmallTableReadinessProvider> Provider = Facts;
  auto RTOrErr =
      EJitSmallTableRuntime::create(Cfg, Registry, State, std::move(Provider),
                                    Opts.runtime);
  if (!RTOrErr)
    return RTOrErr.takeError();
  auto Host = std::unique_ptr<EJitSmallTableHost>(new EJitSmallTableHost());
  Host->ownedRuntime_ = std::move(*RTOrErr);
  Host->runtime_ = Host->ownedRuntime_.get();
  Host->facts_ = std::move(Facts);
  Host->options_ = Opts;
  return std::move(Host);
}

Expected<std::unique_ptr<EJitSmallTableHost>> EJitSmallTableHost::create(
    EJitSmallTableRuntime &Runtime,
    std::shared_ptr<EJitSmallTableFactSource> Facts, Options Opts) {
  if (!Facts) {
    return make_error<StringError>(
        "small-table host: no fact source installed, refusing to specialize an "
        "unproven domain",
        inconvertibleErrorCode());
  }
  auto Host = std::unique_ptr<EJitSmallTableHost>(new EJitSmallTableHost());
  Host->runtime_ = &Runtime;
  Host->facts_ = std::move(Facts);
  Host->options_ = Opts;
  return std::move(Host);
}

namespace {
/// The process-global normal-path integration point. Only ever set while the
/// feature is explicitly enabled; null means OFF and every dispatch entry takes
/// its original AOT/compile path untouched.
std::atomic<EJitSmallTableHost *> gSmallTableHost{nullptr};
EJIT_SHARED_SECTION std::atomic<uint64_t> gSmallTablePolicyEpoch{1};
EJitWrapperRuntimeTestAccess::HostInstallationObserver
    gHostInstallationObserver = nullptr;
void *gHostInstallationObserverContext = nullptr;

/// The completion ABI carries only a token, so it must identify its owner
/// across host replacement. Never wrap into a token an older owner may hold.
std::atomic<uint64_t> gNextSmallTableExecutionToken{1};

uint64_t allocateExecutionToken() {
  uint64_t Next = gNextSmallTableExecutionToken.load(std::memory_order_relaxed);
  // The high bit names additive cross-core bridge tokens. Both namespaces
  // exhaust without wrap, so a stale/duplicate token cannot name another call.
  while (Next < (uint64_t{1} << 63)) {
    if (gNextSmallTableExecutionToken.compare_exchange_weak(
            Next, Next + 1, std::memory_order_relaxed))
      return Next;
  }
  return 0;
}

/// Owners that were uninstalled (disable / host replacement / instance
/// teardown) while a real execution was still inside their table. A late
/// `ejit_stab_leave` must still reach the host whose token it carries, so the
/// object - and with it the protected borrow and the resource generation the
/// running call reads - is retained until that execution returns.
#ifndef EJIT_FREESTANDING
std::mutex &retiredOwnerMutex() {
  static std::mutex M;
  return M;
}
#endif
std::vector<std::unique_ptr<EJitSmallTableHost>> &retiredOwners() {
  static std::vector<std::unique_ptr<EJitSmallTableHost>> Owners;
  return Owners;
}
} // namespace

EJitSmallTableHost *EJitSmallTableHost::installGlobal(EJitSmallTableHost *Host) {
  return installGlobalImpl(Host, nullptr);
}

EJitSmallTableHost *EJitSmallTableHost::installGlobal(
    const detail::OwnerWorkerContext &Worker, EJitSmallTableHost *Host) {
  return installGlobalImpl(Host, &Worker);
}

EJitSmallTableHost *EJitSmallTableHost::installGlobalImpl(
    EJitSmallTableHost *Host, const detail::OwnerWorkerContext *Worker) {
  if (Host && global() == Host && Host->wrapperAdmissionReady())
    return Host; // an already-effective same-owner install is a true no-op
  if (Host)
    Host->acquireOwnerControlPin();
  struct InstallationPin {
    EJitSmallTableHost *Host;
    ~InstallationPin() {
      if (Host)
        Host->releaseOwnerControlPin();
    }
  } Pin{Host};
  // Close this Host before its pointer becomes visible, even when replacing
  // another bound Host for the same function whose shared bit is already owned.
  if (Host)
    Host->setWrapperAdmissionReady(false);
  EJitSmallTableHost *Old =
      gSmallTableHost.exchange(Host, std::memory_order_acq_rel);
  notePolicyChange();
  if (Old && Old != Host &&
      (!Host || !Host->isBoundTo(Old->funcIndex())))
    releaseSmallTableOwnership(Old);
  if (Host && gHostInstallationObserver)
    gHostInstallationObserver(gHostInstallationObserverContext, Host);
  // A Host can have been prepared before installation. Installing that bound
  // policy must still join the ordinary-PGO handoff; merely moving the wrapper
  // hook would otherwise leave the earlier session occupying admission.
  if (Host && Host->isBoundTo(Host->funcIndex()) &&
      Host->belongsToRuntimeOwner(currentEJitRuntimeOwnerIdentity()) &&
      (Worker || !inSmallTableOwnerRequest(Host))) {
    SmallTableOwnerWorkerJob Job =
        [](const detail::OwnerWorkerContext &) { return Error::success(); };
    Error Handoff = Worker
                        ? runSmallTableOwnerRequest(*Worker, Host->funcIndex(),
                                                    Host, Job)
                        : runSmallTableOwnerRequest(Host->funcIndex(), Host,
                                                    Job);
    if (Error E = std::move(Handoff)) {
      const std::string Why = toString(std::move(E));
      if (Worker)
        Host->cancel(*Worker, Why);
      else
        Host->cancel(Why);
      EJIT_DIAG("small-table bound installation remains AOT: %s", Why.c_str());
    }
  }
  return Old;
}

void EJitWrapperRuntimeTestAccess::setHostInstallationObserver(
    HostInstallationObserver Observer, void *Context) {
  gHostInstallationObserver = Observer;
  gHostInstallationObserverContext = Context;
}

uint64_t EJitSmallTableHost::policyEpoch() {
  return gSmallTablePolicyEpoch.load(std::memory_order_acquire);
}

void EJitSmallTableHost::notePolicyChange() {
  smallTableSrePolicyChanged();
  uint64_t Epoch = gSmallTablePolicyEpoch.load(std::memory_order_acquire);
  while (Epoch != std::numeric_limits<uint64_t>::max() &&
         !gSmallTablePolicyEpoch.compare_exchange_weak(
             Epoch, Epoch + 1, std::memory_order_acq_rel)) {}
}

EJitSmallTableHost *EJitSmallTableHost::global() {
  return gSmallTableHost.load(std::memory_order_acquire);
}

bool EJitSmallTableHost::beginOwnerTeardown() {
  return beginOwnerTeardownImpl(nullptr);
}

bool EJitSmallTableHost::beginOwnerTeardown(
    const detail::OwnerWorkerContext &Worker) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  EJit *Runtime = acquireSmallTableSreRuntime(Worker);
  if (!Runtime)
    return false;
  struct RuntimePin {
    EJit *Runtime;
    const detail::OwnerWorkerContext &Worker;
    ~RuntimePin() { releaseSmallTableSreRuntime(Runtime, Worker); }
  } Pin{Runtime, Worker};
  EJitSharedTaskPool *Pool = Runtime->sharedTaskPool();
  if (!Pool || !Worker.activeFor(*Pool) || Runtime->smallTableHost() != this)
    return false;
#else
  (void)Worker;
  return false;
#endif
  return beginOwnerTeardownImpl(&Worker);
}

bool EJitSmallTableHost::beginOwnerTeardownImpl(
    const detail::OwnerWorkerContext *Worker) {
  setWrapperAdmissionReady(false);
  // Logical teardown FIRST: no new call can be admitted, every slot stops being
  // published and every entered execution is settled for sampling. The physical
  // leases stay until their own completions.
  if (global() == this) {
    if (Worker)
      installGlobal(*Worker, nullptr);
    else
      installGlobal(nullptr);
  }
  retractPublishedSlots();
  if (Worker)
    cancel(*Worker, "small-table normal path disabled");
  else
    cancel("small-table normal path disabled");
  if (executions_.empty() && ownerControlPins_.loadAcquire() == 0)
    return true; // destroy now: nothing is inside this table
  return false;  // retain: a real execution still holds this generation
}

void EJitSmallTableHost::adoptRetired(std::unique_ptr<EJitSmallTableHost> Host) {
  if (!Host)
    return;
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(retiredOwnerMutex());
#endif
  retiredOwners().push_back(std::move(Host));
}

bool EJitSmallTableHost::leaveRetainedExecution(uint64_t Ticket) {
  if (Ticket == 0)
    return false;
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(retiredOwnerMutex());
#endif
  std::vector<std::unique_ptr<EJitSmallTableHost>> &Owners = retiredOwners();
  for (size_t I = 0; I < Owners.size(); ++I) {
    EJitSmallTableHost *H = Owners[I].get();
    if (!H || !H->ownsExecution(Ticket))
      continue;
    H->leave(Ticket);
    // The completion emptied the last physical lease: the retained owner is
    // released here, still under the registry lock, so an entry can never be
    // looked up after its destruction. A host whose own `leave` reached this
    // point has already erased its registry entry, in which case the lookup
    // above would not have found it.
    if (H->physicalExecutions() == 0 && H->ownerControlPins_.loadAcquire() == 0)
      Owners.erase(Owners.begin() + static_cast<ptrdiff_t>(I));
    return true;
  }
  return false;
}

bool EJitSmallTableHost::leaveRetainedExecution(
    EJitSmallTableHost *ExpectedHost, uint64_t Ticket) {
  if (!ExpectedHost || Ticket == 0)
    return false;
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(retiredOwnerMutex());
#endif
  std::vector<std::unique_ptr<EJitSmallTableHost>> &Owners = retiredOwners();
  for (size_t I = 0; I < Owners.size(); ++I) {
    EJitSmallTableHost *Host = Owners[I].get();
    if (Host != ExpectedHost || !Host->ownsExecution(Ticket))
      continue;
    Host->leave(Ticket);
    if (Host->physicalExecutions() == 0 &&
        Host->ownerControlPins_.loadAcquire() == 0)
      Owners.erase(Owners.begin() + static_cast<ptrdiff_t>(I));
    return true;
  }
  return false;
}

void EJitSmallTableHost::releaseOwnerControlPin() {
  if (ownerControlPins_.fetchSub(1) != 1 || physicalExecutions() != 0)
    return;
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(retiredOwnerMutex());
#endif
  auto &Owners = retiredOwners();
  for (size_t I = 0; I < Owners.size(); ++I)
    if (Owners[I].get() == this) {
      Owners.erase(Owners.begin() + static_cast<ptrdiff_t>(I));
      return; // may have destroyed this: do not access any field afterward
    }
}

uint64_t EJitSmallTableHost::retainedOwnerCount() {
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(retiredOwnerMutex());
#endif
  return retiredOwners().size();
}

uint64_t EJitSmallTableHost::retainedOwnerOutstandingExecutions() {
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(retiredOwnerMutex());
#endif
  uint64_t N = 0;
  for (const std::unique_ptr<EJitSmallTableHost> &H : retiredOwners())
    if (H)
      N += H->physicalExecutions();
  return N;
}

bool EJitSmallTableHost::releaseRetainedOwner(EJitSmallTableHost *Host) {
  if (!Host)
    return false;
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(retiredOwnerMutex());
#endif
  std::vector<std::unique_ptr<EJitSmallTableHost>> &Owners = retiredOwners();
  for (size_t I = 0; I < Owners.size(); ++I) {
    if (Owners[I].get() != Host)
      continue;
    Owners[I].release(); // ownership returns to the caller; the object lives on
    Owners.erase(Owners.begin() + static_cast<ptrdiff_t>(I));
    return true;
  }
  return false;
}

void EJitSmallTableHost::abandonRetainedOwners() {
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(retiredOwnerMutex());
#endif
  retiredOwners().clear();
}

bool EJitSmallTableHost::ownsExecution(uint64_t Ticket) const {
  for (const ExecutionRecord &Rec : executions_)
    if (Rec.token == Ticket)
      return true;
  return false;
}

//===----------------------------------------------------------------------===//
// Dimension binding
//===----------------------------------------------------------------------===//

bool EJitSmallTableHost::resolveDimTypes(std::string &Why) {
  // Each declared dimension is tied to a lifecycle (period) name the
  // configuration server knows. The wrapper passes the dense dimType slot the
  // process-global registry assigned to that name, so resolving the name here
  // is what makes a real call's dim pairs comparable with the plan schema. A
  // name that was never registered means the dimension cannot be identified in
  // a real call: fail closed rather than invent an index.
  dimTypes_.clear();
  EJitLifecycleRegistry &Reg = EJitLifecycleRegistry::instance();
  for (const std::string &Name : dimPeriodNames_) {
    const uint32_t Slot = Reg.lookup(Name);
    if (Slot == kEJitInvalidDimType) {
      Why = "small-table host: dimension lifecycle '" + Name +
            "' was never registered by the AOT wrapper, so a real call's "
            "dimension cannot be identified";
      return false;
    }
    if (std::find(dimTypes_.begin(), dimTypes_.end(), Slot) != dimTypes_.end()) {
      Why = "small-table host: two declared dimensions resolve to the same "
            "dimType slot " +
            std::to_string(Slot) + " (lifecycle '" + Name + "')";
      return false;
    }
    dimTypes_.push_back(Slot);
  }
  return true;
}

bool EJitSmallTableHost::coordinateOf(ArrayRef<uint32_t> DimTypes,
                                      ArrayRef<uint32_t> InstanceIds,
                                      SmallVectorImpl<uint64_t> &Out,
                                      std::string *Why) const {
  Out.clear();
  if (DimTypes.size() != InstanceIds.size()) {
    if (Why)
      *Why = "dims and instances disagree in count";
    return false;
  }
  Out.reserve(dims_.size());
  for (unsigned D = 0; D < dims_.size(); ++D) {
    const uint32_t Want = dimTypes_[D];
    int Found = -1;
    for (unsigned I = 0; I < DimTypes.size(); ++I) {
      if (DimTypes[I] != Want)
        continue;
      if (Found >= 0) {
        if (Why)
          *Why = "dimension " + std::to_string(D) +
                 " appears more than once in the call's dim list";
        Out.clear();
        return false;
      }
      Found = static_cast<int>(I);
    }
    if (Found < 0) {
      if (Why)
        *Why = "the call does not carry declared dimension " +
               std::to_string(D) + " (dimType " + std::to_string(Want) + ")";
      Out.clear();
      return false;
    }
    const uint64_t Instance = InstanceIds[static_cast<unsigned>(Found)];
    // Modulo dimensions index the source array with `urem(arg, modulus)`, so
    // the plan's extent is the real bound of the reachable coordinate. A value
    // outside it is not a member of this schema at all.
    const uint64_t Extent = dims_[D].extent;
    const uint64_t Bound =
        dims_[D].kind == EJitSmallTableDim::Kind::ModuloArgument &&
                dims_[D].modulus != 0
            ? std::min<uint64_t>(Extent, dims_[D].modulus)
            : Extent;
    if (Instance >= Bound) {
      if (Why)
        *Why = "instance " + std::to_string(Instance) +
               " leaves the declared extent of dimension " +
               std::to_string(D);
      Out.clear();
      return false;
    }
    Out.push_back(Instance);
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Request path
//===----------------------------------------------------------------------===//

Expected<const EJitSmallTablePlan *>
EJitSmallTableHost::planEntry(const EntryRequest &Request, std::string &Why) {
  if (global() == this &&
      belongsToRuntimeOwner(currentEJitRuntimeOwnerIdentity())) {
    const EJitSmallTablePlan *Plan = nullptr;
    SmallTableOwnerWorkerJob Job = [this, &Request, &Why, &Plan](
        const detail::OwnerWorkerContext &) -> Error {
      auto P = planEntryImpl(Request, Why);
      if (!P)
        return P.takeError();
      Plan = *P;
      return Error::success();
    };
    if (Error E = runSmallTableOwnerRequest(Request.funcIndex, this,
                                            std::move(Job)))
      return std::move(E);
    return Plan;
  }
  return planEntryImpl(Request, Why);
}

Expected<const EJitSmallTablePlan *> EJitSmallTableHost::planEntry(
    const detail::OwnerWorkerContext &Worker, const EntryRequest &Request,
    std::string &Why) {
  const EJitSmallTablePlan *Plan = nullptr;
  SmallTableOwnerWorkerJob Job = [this, &Request, &Why, &Plan](
      const detail::OwnerWorkerContext &) -> Error {
    auto P = planEntryImpl(Request, Why);
    if (!P)
      return P.takeError();
    Plan = *P;
    return Error::success();
  };
  if (Error E = runSmallTableOwnerRequest(Worker, Request.funcIndex, this,
                                          std::move(Job)))
    return std::move(E);
  return Plan;
}

Expected<const EJitSmallTablePlan *>
EJitSmallTableHost::planEntryImpl(const EntryRequest &Request,
                                 std::string &Why) {
  if (global() == this &&
      belongsToRuntimeOwner(currentEJitRuntimeOwnerIdentity()) &&
      (Request.funcIndex >= EJitFuncRegistry::instance().count() ||
       EJitFuncRegistry::instance().lookup(Request.entryName) != Request.funcIndex)) {
    Why = "small-table request function index does not identify its registered entry";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (bound_) {
    Why = "small-table host: entry '" + entryName_ +
          "' is already bound; one host drives one entry";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (!Request.module) {
    Why = "small-table host: request without a module";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (!facts_) {
    Why = "small-table host: no fact source; the product configuration "
          "transaction is not bound, so readiness is unknown";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (Request.dims.size() != Request.dimPeriodNames.size()) {
    Why = "small-table host: every declared dimension needs its lifecycle name";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (Request.dims.empty()) {
    Why = "small-table host: an entry with no declared dimension has no member "
          "coordinate and cannot be admitted";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }

  entryName_ = Request.entryName;
  funcIndex_ = Request.funcIndex;
  sourceVarName_ = Request.sourceVarName;
  dims_.clear();
  dims_.append(Request.dims.begin(), Request.dims.end());
  dimPeriodNames_.assign(Request.dimPeriodNames.begin(),
                         Request.dimPeriodNames.end());
  codeGeneration_ = Request.codeGeneration ? Request.codeGeneration
                                           : facts_->domainEpoch();
  if (codeGeneration_ == 0) {
    Why = "small-table host: no code generation identity for this plan";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }

  // The legal domain, dependency and epoch facts all come from the runtime's
  // prepare: it refuses without a provider, without a borrow, without an epoch
  // and without a confirmed-ready member.
  auto PlanOrErr = runtime_->prepare(*Request.module, entryName_, sourceVarName_,
                                     dims_, Why);
  if (!PlanOrErr)
    return PlanOrErr.takeError();

  if (!resolveDimTypes(Why)) {
    // The plan exists but no real call can be mapped onto it: withdraw it so no
    // slot can ever be published from an unprovable binding.
    runtime_->cancel(Why);
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }

  // The instrumented tier's pass synthesizes its profile-runtime hook reference
  // while the compile pipeline runs, and the engine resolves a registered user
  // symbol from the symbol table it builds when the module is loaded. Register
  // it BEFORE the first load so the common T1 can materialize at all.
  ensureProfileRuntimeHook();

  bound_ = true;
  notePolicyChange();
  planReady_ = true;
  factRevision_ = facts_->configurationRevision();

  // Admit every ready coordinate the plan covers, so the slot table is exactly
  // the admitted member set (the published subset is chosen by the contract).
  const EJitSmallTablePlan *Plan = runtime_->plan();
  std::vector<std::vector<uint64_t>> Coordinates;
  for (const EJitSmallTableReadyMember &Member : facts_->readyMembers()) {
    if (!Member.fieldsInitialized || Member.indices.size() != dims_.size())
      continue;
    std::vector<uint64_t> Key(Member.indices.begin(), Member.indices.end());
    if (std::find(Coordinates.begin(), Coordinates.end(), Key) ==
        Coordinates.end())
      Coordinates.push_back(std::move(Key));
  }
  llvm::sort(Coordinates);
  for (const std::vector<uint64_t> &Key : Coordinates) {
    std::string AdmitWhy;
    (void)runtime_->admitMember(Key, &AdmitWhy);
    if (Plan && !Plan->isConsistent(nullptr)) {
      Why = "small-table host: the planned schema is internally inconsistent";
      return make_error<StringError>(Why, inconvertibleErrorCode());
    }
  }
  rebuildSlots();
  return Plan;
}

void EJitSmallTableHost::registerExtraSymbol(const std::string &Name,
                                             void *Address) {
  if (!runtime_ || Name.empty() || !Address)
    return;
  runtime_->engine().addUserSymbol(Name, Address);
  extraSymbols_.push_back(Name);
}

void EJitSmallTableHost::ensureProfileRuntimeHook() {
  static const char *const kHook = "__llvm_profile_runtime";
  // A caller-supplied address wins: the product image defines this hook.
  for (const std::string &Name : extraSymbols_)
    if (Name == kHook)
      return;
  // The instrumented tier's pass synthesizes the reference while the pipeline
  // runs, so the engine's own declaration scan cannot see it. The hook carries
  // no counter state: the counters the profile bundle reads are the real
  // transform-generated __profc_/__profd_ globals.
  static uint32_t ProfileRuntimeHook = 0;
  runtime_->engine().addUserSymbol(
      kHook, reinterpret_cast<void *>(&ProfileRuntimeHook));
  extraSymbols_.push_back(kHook);
}

Expected<void *> EJitSmallTableHost::compileT1(std::string &Why) {
  if (global() == this &&
      belongsToRuntimeOwner(currentEJitRuntimeOwnerIdentity())) {
    void *Entry = nullptr;
    SmallTableOwnerWorkerJob Job = [this, &Why, &Entry](
        const detail::OwnerWorkerContext &) -> Error {
      auto P = compileT1Impl(Why);
      if (!P)
        return P.takeError();
      Entry = *P;
      return Error::success();
    };
    if (Error E = runSmallTableOwnerRequest(
            funcIndex_, this, std::move(Job), /*InitialHandoff=*/false))
      return std::move(E);
    return Entry;
  }
  return compileT1Impl(Why);
}

Expected<void *> EJitSmallTableHost::compileT1(
    const detail::OwnerWorkerContext &Worker, std::string &Why) {
  void *Entry = nullptr;
  SmallTableOwnerWorkerJob Job = [this, &Why, &Entry](
      const detail::OwnerWorkerContext &) -> Error {
    auto P = compileT1Impl(Why);
    if (!P)
      return P.takeError();
    Entry = *P;
    return Error::success();
  };
  if (Error E = runSmallTableOwnerRequest(
          Worker, funcIndex_, this, std::move(Job), /*InitialHandoff=*/false))
    return std::move(E);
  return Entry;
}

Expected<void *> EJitSmallTableHost::compileT1Impl(std::string &Why) {
  if (!runtime_->plan()) {
    Why = "small-table host: compileT1 before a successful plan";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  ensureProfileRuntimeHook();
  auto FnOrErr = runtime_->compileCommonT1(codeGeneration_, Why);
  if (!FnOrErr)
    return FnOrErr.takeError();
  if (!*FnOrErr) {
    Why = "small-table host: the common T1 compile produced no callable entry";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  // The compiled instrumented entry reads the resource's columns through the
  // column addresses the engine resolved. Reading them back here is the
  // host-side identity probe: a T1 that bound another resource would have left
  // these at a different address, and compileCommonT1 already refused that.
  t1Entry_ = *FnOrErr;
  for (const EJitSmallTableTableResource::Column &C : runtime_->resource()->columns()) {
    auto ColOrErr = runtime_->engine().lookup(codeGeneration_, C.symbolName);
    if (!ColOrErr) {
      consumeError(ColOrErr.takeError());
      Why = "small-table host: column " + C.symbolName +
            " was not resolvable after T1";
      return make_error<StringError>(Why, inconvertibleErrorCode());
    }
    if (*ColOrErr != runtime_->resource()->columnAddress(C.fieldIndex)) {
      Why = "small-table host: T1 bound a different table resource for column " +
            C.symbolName;
      return make_error<StringError>(Why, inconvertibleErrorCode());
    }
  }
  t1Ready_ = true;
  return t1Entry_;
}

Error EJitSmallTableHost::requestEntry(const EntryRequest &Request,
                                       void *executableAot, std::string &Why) {
  if (global() == this &&
      belongsToRuntimeOwner(currentEJitRuntimeOwnerIdentity())) {
    SmallTableOwnerWorkerJob Job = [this, &Request, executableAot, &Why](
        const detail::OwnerWorkerContext &) -> Error {
      return requestEntryImpl(Request, executableAot, Why);
    };
    return runSmallTableOwnerRequest(Request.funcIndex, this, std::move(Job));
  }
  return requestEntryImpl(Request, executableAot, Why);
}

Error EJitSmallTableHost::requestEntry(
    const detail::OwnerWorkerContext &Worker, const EntryRequest &Request,
    void *executableAot, std::string &Why) {
  SmallTableOwnerWorkerJob Job = [this, &Request, executableAot, &Why](
      const detail::OwnerWorkerContext &) -> Error {
    return requestEntryImpl(Request, executableAot, Why);
  };
  return runSmallTableOwnerRequest(Worker, Request.funcIndex, this,
                                  std::move(Job));
}

Error EJitSmallTableHost::requestEntryImpl(const EntryRequest &Request,
                                           void *executableAot,
                                           std::string &Why) {
  aotEntry_ = executableAot;
  auto PlanOrErr = planEntryImpl(Request, Why);
  if (!PlanOrErr)
    return PlanOrErr.takeError();
  auto T1OrErr = compileT1Impl(Why);
  if (!T1OrErr)
    return T1OrErr.takeError();
  return Error::success();
}

//===----------------------------------------------------------------------===//
// Publication
//===----------------------------------------------------------------------===//

void EJitSmallTableHost::rebuildSlots() {
  slots_.clear();
  const EJitSmallTablePlan *Plan = runtime_->plan();
  if (!Plan || !runtime_->resource())
    return;

  const uint64_t ResourceGeneration = runtime_->resource()->generation();
  for (const auto &Entry : runtime_->admittedMembers()) {
    EJitSmallTableLogicalSlot Slot;
    Slot.coordinate = Entry.first;
    Slot.admission = Entry.second;
    Slot.tableReady = true;
    Slot.resourceGeneration = ResourceGeneration;
    const bool Deactivated =
        std::any_of(deactivated_.begin(), deactivated_.end(),
                    [&](const std::vector<uint64_t> &C) {
                      return Slot.coordinate.size() == C.size() &&
                             std::equal(Slot.coordinate.begin(),
                                        Slot.coordinate.end(), C.begin());
                    });
    if (codeReady_ && publishedResourceGeneration_ == ResourceGeneration &&
        !Deactivated) {
      Slot.codeReady = true;
      Slot.codeGeneration = publishedCodeGeneration_;
      Slot.entry = t2Entry_;
    }
    slots_.push_back(std::move(Slot));
  }
  if (codeReady_)
    publishedSlotsCache_ = 0;
}

Error EJitSmallTableHost::publishGeneration(std::string &Why) {
  if (global() == this &&
      belongsToRuntimeOwner(currentEJitRuntimeOwnerIdentity()) &&
      !inSmallTableOwnerRequest(this)) {
    SmallTableOwnerWorkerJob Job = [this, &Why](
        const detail::OwnerWorkerContext &) {
      return publishGenerationImpl(Why);
    };
    return runSmallTableOwnerRequest(funcIndex_, this, std::move(Job),
                                     /*InitialHandoff=*/false);
  }
  return publishGenerationImpl(Why);
}

Error EJitSmallTableHost::publishGeneration(
    const detail::OwnerWorkerContext &Worker, std::string &Why) {
  SmallTableOwnerWorkerJob Job = [this, &Why](
      const detail::OwnerWorkerContext &) {
    return publishGenerationImpl(Why);
  };
  return runSmallTableOwnerRequest(Worker, funcIndex_, this, std::move(Job),
                                   /*InitialHandoff=*/false);
}

Error EJitSmallTableHost::publishGenerationImpl(std::string &Why) {
  if (!planReady_ || !t1Ready_) {
    Why = "small-table host: publishGeneration before a successful T1";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (codeReady_) {
    Why = "small-table host: this generation is already published";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (!facts_->epochCurrent(runtime_->contract().domainEpoch)) {
    drainSlots("configuration generation moved before publication");
    Why = "small-table host: the configuration generation moved before "
          "publication";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (facts_->configurationRevision() != factRevision_) {
    drainSlots("configuration revision moved before publication");
    Why = "small-table host: the configuration revision moved before "
          "publication (expected " +
          std::to_string(factRevision_) + ", now " +
          std::to_string(facts_->configurationRevision()) + ")";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }

  // Freeze ONE immutable bundle for this entry/code generation. A grant is not
  // a completion: freeze drains in-flight admitted executions first, and
  // refuses rather than reading half a sample.
  auto BundleOrErr = runtime_->freeze(Why);
  if (!BundleOrErr)
    return BundleOrErr.takeError();

  // The common T2 must read the SAME resource generation as T1; the runtime
  // refuses a T2 that bound a different table.
  auto T2OrErr = runtime_->compileCommonT2(Why);
  if (!T2OrErr)
    return T2OrErr.takeError();
  if (!*T2OrErr) {
    Why = "small-table host: the common T2 compile produced no callable entry";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }

  t2Entry_ = *T2OrErr;
  publishedCodeGeneration_ = codeGeneration_;
  publishedResourceGeneration_ = runtime_->resource()->generation();
  codeReady_ = true;
  rebuildSlots();
  facts_->notePublishedRevision(factRevision_, slots_.size());
  return Error::success();
}

//===----------------------------------------------------------------------===//
// Slots and gating
//===----------------------------------------------------------------------===//

void EJitSmallTableHost::drainSlots(StringRef Reason) {
  (void)Reason;
  for (EJitSmallTableLogicalSlot &Slot : slots_) {
    if (Slot.entry || Slot.codeReady) {
      ++drainedSlotCount_;
    }
    Slot.entry = nullptr;
    Slot.codeReady = false;
    Slot.codeGeneration = 0;
  }
  codeReady_ = false;
  t2Entry_ = nullptr;
  publishedSlotsCache_ = 0;
}

uint64_t EJitSmallTableHost::publishedSlots() const {
  uint64_t N = 0;
  for (const EJitSmallTableLogicalSlot &Slot : slots_)
    if (Slot.published())
      ++N;
  return N;
}

const EJitSmallTableLogicalSlot *
EJitSmallTableHost::findSlot(ArrayRef<uint64_t> Coordinate) const {
  for (const EJitSmallTableLogicalSlot &Slot : slots_)
    if (Slot.coordinate.size() == Coordinate.size() &&
        std::equal(Slot.coordinate.begin(), Slot.coordinate.end(),
                   Coordinate.begin()))
      return &Slot;
  return nullptr;
}

EJitSmallTableDispatch
EJitSmallTableHost::gateFor(ArrayRef<uint64_t> Coordinate,
                            const EJitSmallTableLogicalSlot **SlotOut,
                            std::string &Why) const {
  if (SlotOut)
    *SlotOut = nullptr;
  if (!bound_ || !planReady_) {
    Why = emptyReason_.empty() ? "no small-table entry is bound to this "
                                "function index"
                              : emptyReason_;
    return EJitSmallTableDispatch::NotBound;
  }
  const EJitSmallTableLogicalSlot *Slot = findSlot(Coordinate);
  if (!Slot) {
    Why = "the coordinate has no slot: the member was never admitted";
    return EJitSmallTableDispatch::Aot;
  }
  if (SlotOut)
    *SlotOut = Slot;
  if (Slot->admission != EJitSmallTableAdmission::Compatible &&
      Slot->admission != EJitSmallTableAdmission::Extendable) {
    Why = std::string("the member admission is ") +
          admissionName(Slot->admission) + ", so it must stay AOT";
    return EJitSmallTableDispatch::Aot;
  }
  if (!Slot->tableReady) {
    Why = "tableReady is false for this coordinate";
    return EJitSmallTableDispatch::Aot;
  }
  if (options_.requireCodeReadyForDispatch && !Slot->codeReady) {
    Why = "codeReady is false: no published code generation for this "
          "coordinate yet";
    return EJitSmallTableDispatch::Aot;
  }
  if (!Slot->entry) {
    Why = "the slot has no callable entry";
    return EJitSmallTableDispatch::NoEntry;
  }
  Why.clear();
  return EJitSmallTableDispatch::Dispatched;
}

bool EJitSmallTableHost::wouldDispatch(ArrayRef<uint64_t> Coordinate,
                                       std::string *Why) const {
  std::string Local;
  const EJitSmallTableDispatch D = gateFor(Coordinate, nullptr, Local);
  if (Why)
    *Why = Local;
  return D == EJitSmallTableDispatch::Dispatched;
}

bool EJitSmallTableHost::wouldDispatchCall(ArrayRef<uint32_t> DimTypes,
                                           ArrayRef<uint32_t> InstanceIds,
                                           std::string *Why) const {
  SmallVector<uint64_t, 4> Coordinate;
  std::string Local;
  if (!coordinateOf(DimTypes, InstanceIds, Coordinate, &Local)) {
    if (Why)
      *Why = Local;
    return false;
  }
  if (wouldDispatch(Coordinate, Why))
    return true;
  // Pre-publication: the ONE common sampling session is filled by ACTUAL
  // admitted calls, so a call the host would admit to the instrumented tier must
  // be allowed through the runtime's resolve gate rather than being sent to the
  // AOT body. The authoritative admission decision is still `enter`'s; this only
  // stops the cheap pre-filter from blocking every real call while the window
  // fills. A coordinate that is not samplable keeps the published refusal.
  return samplingAdmissible(Coordinate, Why);
}

bool EJitSmallTableHost::samplingAdmissible(ArrayRef<uint64_t> Coordinate,
                                            std::string *Why) const {
  auto Refuse = [&](const char *Msg) {
    if (Why)
      *Why = Msg;
    return false;
  };
  if (!bound_ || !planReady_)
    return Refuse("no small-table entry is bound to this function index");
  if (!t1Ready_ || !runtime_->sessionOpen())
    return Refuse("no open sampling session for this entry");
  if (runtime_->samplingExhausted())
    return Refuse("aggregate sampling budget reached; use AOT until T2 publishes");
  const EJitSmallTableLogicalSlot *Slot = findSlot(Coordinate);
  if (!Slot)
    return Refuse("the coordinate has no slot: the member was never admitted");
  if (Slot->admission != EJitSmallTableAdmission::Compatible &&
      Slot->admission != EJitSmallTableAdmission::Extendable)
    return Refuse("the member admission does not allow the specialized code");
  if (!Slot->tableReady)
    return Refuse("tableReady is false for this coordinate");
  const bool Deactivated =
      std::any_of(deactivated_.begin(), deactivated_.end(),
                  [&](const std::vector<uint64_t> &C) {
                    return C.size() == Coordinate.size() &&
                           std::equal(C.begin(), C.end(), Coordinate.begin());
                  });
  if (Deactivated)
    return Refuse("the coordinate was deactivated: its slot stays drained");
  if (Why)
    Why->clear();
  return true;
}

//===----------------------------------------------------------------------===//
// Dispatch
//===----------------------------------------------------------------------===//

EJitSmallTableDispatchResult
EJitSmallTableHost::refuse(EJitSmallTableDispatch Status, std::string Why) {
  EJitSmallTableDispatchResult R;
  R.status = Status;
  R.why = std::move(Why);
  return R;
}

EJitSmallTableDispatchResult
EJitSmallTableHost::dispatch(ArrayRef<uint32_t> DimTypes,
                             ArrayRef<uint32_t> InstanceIds, int64_t Arg) {
  if (!bound_)
    return refuse(EJitSmallTableDispatch::NotBound,
                  "no small-table entry is bound to this function index");

  SmallVector<uint64_t, 4> Coordinate;
  std::string Why;
  if (!coordinateOf(DimTypes, InstanceIds, Coordinate, &Why)) {
    ++unprovableCoordinateCount_;
    return refuse(EJitSmallTableDispatch::CoordinateUnprovable, Why);
  }

  uint64_t Ticket = 0;
  const uint64_t Before = runtime_->currentSessionSamples();
  void *Entry = enter(DimTypes, InstanceIds, &Ticket, &Why);
  if (!Entry) {
    ++aotDispatchCount_;
    return refuse(EJitSmallTableDispatch::Aot, Why);
  }

  using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
  EntryFn Fn = reinterpret_cast<EntryFn>(Entry);
  const bool Counted = runtime_->currentSessionSamples() > Before;
  const int64_t Value =
      Fn(static_cast<uint64_t>(Coordinate[0]),
         dims_.size() > 1 ? static_cast<uint64_t>(Coordinate[1]) : 0ull, Arg);
  EJitSmallTableDispatchResult R;
  R.status = EJitSmallTableDispatch::Dispatched;
  R.counted = Counted;
  R.value = Value;
  leave(Ticket);
  return R;
}

bool EJitSmallTableHost::pinForExecutionPreparation(
    ArrayRef<uint32_t> DimTypes, ArrayRef<uint32_t> Instances, uint64_t &Pin,
    std::string &Why) {
  Pin = 0;
  SmallVector<uint64_t, 4> Coordinate;
  if (!coordinateOf(DimTypes, Instances, Coordinate, &Why))
    return false;
  if (!codeReady_) {
    if (!samplingAdmissible(Coordinate, &Why))
      return false;
  } else if (gateFor(Coordinate, nullptr, Why) !=
             EJitSmallTableDispatch::Dispatched) {
    return false;
  }
  if (!facts_ || !facts_->epochCurrent(runtime_->contract().domainEpoch)) {
    Why = "configuration is not current at execution preparation";
    return false;
  }
  // Pin physical ownership BEFORE a virtual borrow callback. That callback can
  // disable/replace this Host; a preparation pin must keep its owner and table
  // alive even on the failure return. The caller's guard leaves Pin in all cases.
  ExecutionRecord Rec;
  Rec.token = allocateExecutionToken();
  if (!Rec.token) {
    Why = "small-table preparation token space exhausted";
    return false;
  }
  Rec.resourceGeneration = runtime_->resourceGeneration();
  Rec.codeGeneration = codeGeneration_;
  Rec.logicalSession = logicalSessionId_;
  runtime_->acquireReader(Rec.resourceGeneration);
  Pin = Rec.token;
  executions_.push_back(std::move(Rec));
  ++activeExecutions_;
  auto Facts = facts_;
  const uint64_t DomainEpoch = runtime_->contract().domainEpoch;
  auto Borrow = Facts->borrow(runtime_->contract().sourceVarName, Why);
  if (!Borrow)
    return false;
  if (Borrow->stale() || !Facts->epochCurrent(DomainEpoch)) {
    Borrow->release();
    Why = "configuration changed at execution preparation";
    return false;
  }
  // Re-entrant callbacks can reallocate executions_; never keep a vector
  // element reference across borrow(). Match the globally unique real token.
  for (ExecutionRecord &Record : executions_)
    if (Record.token == Pin) {
      Record.borrow = std::move(Borrow);
      return true;
    }
  Borrow->release();
  Why = "preparation owner was completed during borrow";
  return false;
}

void *EJitSmallTableHost::enter(ArrayRef<uint32_t> DimTypes,
                                ArrayRef<uint32_t> InstanceIds,
                                uint64_t *OutTicket, std::string *Why) {
  if (OutTicket)
    *OutTicket = 0;
  auto Refuse = [&](const std::string &Msg) -> void * {
    if (Why)
      *Why = Msg;
    return nullptr;
  };
  if (!bound_ || !planReady_)
    return Refuse("no small-table entry is bound to this function index");

  SmallVector<uint64_t, 4> Coordinate;
  std::string Local;
  if (!coordinateOf(DimTypes, InstanceIds, Coordinate, &Local))
    return Refuse(Local);

  // PR231: ONE common T1 per entry/code generation is filled by the entry's OWN
  // admitted business calls, not by a host-side loop. While the sampling session
  // is open and no final generation is published, an admitted member's ordinary
  // call is a real sample and runs the instrumented tier; `leave` closes exactly
  // that execution. The published (T2) gate below is unchanged and still
  // requires codeReady + a published slot, so nothing here exposes final code
  // before publication.
  if (!codeReady_ && samplingAdmissible(Coordinate, nullptr)) {
    void *SampleEntry =
        enterInstrumented(DimTypes, InstanceIds, OutTicket, nullptr);
    if (SampleEntry) {
      if (Why)
        Why->clear();
      return SampleEntry;
    }
  }

  std::string GateWhy;
  const EJitSmallTableDispatch Gate =
      gateFor(Coordinate, nullptr, GateWhy);
  if (Gate != EJitSmallTableDispatch::Dispatched)
    return Refuse(GateWhy);

  // A published execution is not a sampling-session sample (the session is
  // frozen by now, and the bundle it produced is immutable). It still runs
  // under the configuration guarantee: the protected read is taken for this
  // execution and released by `leave`.
  uint64_t Token = 0;
  if (!enterPublished(Coordinate, &Token, Local))
    return Refuse(Local);
  if (OutTicket && Token != 0)
    *OutTicket = Token;
  if (Why)
    Why->clear();
  return t2Entry_;
}

bool EJitSmallTableHost::enterPublished(ArrayRef<uint64_t> Coordinate,
                                        uint64_t *OutToken,
                                        std::string &Why) {
  if (OutToken)
    *OutToken = 0;
  if (!planReady_ || !runtime_->plan()) {
    Why = "the entry has no plan";
    return false;
  }
  if (!facts_ || !facts_->epochCurrent(runtime_->contract().domainEpoch)) {
    Why = "the configuration generation that published this entry is no longer "
          "current";
    return false;
  }
  // Admission is re-checked on every published execution: a member the contract
  // no longer admits (or never admitted) must not reach the specialized code.
  const EJitSmallTableAdmission Admission =
      runtime_->admitMember(Coordinate, &Why);
  if (Admission != EJitSmallTableAdmission::Compatible &&
      Admission != EJitSmallTableAdmission::Extendable) {
    Why = std::string("the member admission is ") + admissionName(Admission) +
          ": " + Why;
    return false;
  }
  const EJitSmallTableLogicalSlot *Slot = findSlot(Coordinate);
  if (!Slot || !Slot->published()) {
    Why = "the coordinate has no published logical slot";
    return false;
  }
  // The protected read: the configuration must stay stable for as long as the
  // specialized code runs. Without a borrow the call takes the AOT path.
  std::string BorrowError;
  std::unique_ptr<EJitSmallTableReadBorrow> Borrow =
      facts_->borrow(runtime_->contract().sourceVarName, BorrowError);
  if (!Borrow) {
    Why = "no protected read borrow for this execution: " + BorrowError;
    return false;
  }
  if (!facts_->epochCurrent(runtime_->contract().domainEpoch) ||
      Borrow->stale()) {
    Borrow->invalidate("configuration generation moved at dispatch");
    Borrow->release();
    Why = "configuration generation moved at dispatch";
    return false;
  }
  ExecutionRecord Rec;
  Rec.token = allocateExecutionToken();
  if (Rec.token == 0) {
    Borrow->release();
    Why = "small-table execution token space exhausted";
    return false;
  }
  Rec.isSample = false;
  Rec.borrow = std::move(Borrow);
  Rec.resourceGeneration = runtime_->resourceGeneration();
  Rec.codeGeneration = codeGeneration_;
  Rec.logicalSession = logicalSessionId_;
  // The physical lease: the runtime counts a reader for this generation, so a
  // retirement that arrives while this call is running cannot free the column
  // storage its compiled address reads. Released only by this execution's own
  // `leave` (never by a cancel).
  if (Rec.resourceGeneration != 0)
    runtime_->acquireReader(Rec.resourceGeneration);
  executions_.push_back(std::move(Rec));
  ++activeExecutions_;
  if (OutToken)
    *OutToken = executions_.back().token;
  Why.clear();
  return true;
}

uint64_t EJitSmallTableHost::beginLogicalSession() {
  ++logicalSessionId_;
  return logicalSessionId_;
}

void EJitSmallTableHost::releasePhysicalLease(ExecutionRecord &Rec) {
  if (Rec.borrow) {
    // Release the execution's protected read: the configuration was protected
    // for exactly as long as the specialized code ran. A cancellation does NOT
    // release this - the call is still running and its compiled code may still
    // read the source region.
    Rec.borrow->release();
    Rec.borrow.reset();
  }
  if (Rec.resourceGeneration != 0) {
    // Drop the generation reader. This is what makes a deferred retirement
    // reclaimable once the last real execution of that generation returns.
    // Sampling tickets own their reader in the runtime and release it through
    // leaveAdmitted. Published calls own their reader directly in this host.
    if (!Rec.isSample)
      runtime_->releaseReader(Rec.resourceGeneration);
    Rec.resourceGeneration = 0;
  }
}

void EJitSmallTableHost::leavePublished(uint64_t Token) {
  if (Token == 0)
    return;
  // Find this execution's record. Executions may complete out of order (a
  // nested call, a preempted core), so the token, not the stack position, is
  // the identity. A record a cancel/config change already closed logically is
  // STILL here: the real call is running, and its physical lease must be
  // released by this completion, never by the cancellation.
  for (size_t I = executions_.size(); I > 0; --I) {
    ExecutionRecord &Rec = executions_[I - 1];
    if (Rec.token != Token)
      continue;
    // A completion that arrives after a cancel/configuration change is a stale
    // callback for SAMPLING: it is counted and never merged into the generation
    // that follows. Its own physical lease is released either way.
    const bool Stale = Rec.logicallyClosed ||
                       Rec.logicalSession != logicalSessionId_ ||
                       (Rec.isSample &&
                        (runtime_->sessionId() != Rec.runtimeSession ||
                         !runtime_->sessionOpen()));
    if (Stale)
      ++staleLeaveCount_;
    if (activeExecutions_ > 0)
      --activeExecutions_;
    if (Rec.isSample) {
      // The runtime's session identity is its own serial: a ticket whose session
      // has since changed is a stale callback, which the runtime counts and
      // never merges. It still decrements the runtime's physical in-flight count
      // for exactly this execution.
      runtime_->leaveAdmitted(Rec.ticket);
    }
    releasePhysicalLease(Rec);
    executions_.erase(executions_.begin() + static_cast<ptrdiff_t>(I - 1));
    reclaimDeferredRetirements();
    return;
  }
}
void EJitSmallTableHost::closeOutstandingExecutions() {
  // LOGICAL settlement only. The generated code that already entered is not
  // stopped by a cancel: it still holds the compiled address and still reads the
  // table's raw column storage. Releasing the protected borrow or erasing the
  // record here would let the resource be retired (and its storage freed) while
  // that call is still running, which is precisely the physical-lifetime
  // violation this protocol exists to prevent. So each in-flight execution is
  // marked closed for admission/publication/sampling purposes and KEEPS its
  // lease: the protected borrow, the resource-generation reader and the record
  // all survive until the real `leave` releases them.
  for (ExecutionRecord &Rec : executions_) {
    if (Rec.logicallyClosed)
      continue;
    Rec.logicallyClosed = true;
    // A sampling execution whose session was cancelled must still deliver its
    // OWN completion to the runtime at its real return: that is what decrements
    // the runtime's physical in-flight count for exactly this execution. Nothing
    // is delivered here, because the execution has not returned.
  }
}

void EJitSmallTableHost::reclaimDeferredRetirements() {
  retiredExecutions_ = 0;
  retiredExecutionGeneration_ = 0;
  for (const ExecutionRecord &Rec : executions_) {
    if (Rec.resourceGeneration == 0 ||
        Rec.resourceGeneration > retirementWatermark_)
      continue;
    ++retiredExecutions_;
    if (retiredExecutionGeneration_ == 0 ||
        Rec.resourceGeneration < retiredExecutionGeneration_)
      retiredExecutionGeneration_ = Rec.resourceGeneration;
  }
  if (retiredExecutions_ != 0)
    return;
  if (retirementWatermark_ != 0) {
    runtime_->reclaimRetiredGenerations();
    retirementWatermark_ = 0;
  }
}

uint64_t EJitSmallTableHost::reclaimRetiredNow() {
  // Explicit reclaim point. It never frees a generation a real execution is
  // still inside: the runtime only reclaims what its physical reader counts say
  // is unread.
  return runtime_->reclaimRetiredGenerations();
}

uint64_t EJitSmallTableHost::logicallyClosedExecutions() const {
  uint64_t N = 0;
  for (const ExecutionRecord &Rec : executions_)
    if (Rec.logicallyClosed)
      ++N;
  return N;
}

uint64_t EJitSmallTableHost::oldestExecutionGeneration() const {
  uint64_t Oldest = 0;
  for (const ExecutionRecord &Rec : executions_)
    if (Rec.resourceGeneration != 0 &&
        (Oldest == 0 || Rec.resourceGeneration < Oldest))
      Oldest = Rec.resourceGeneration;
  return Oldest;
}

std::vector<uint64_t> EJitSmallTableHost::executionGenerations() const {
  std::vector<uint64_t> Out;
  for (const ExecutionRecord &Rec : executions_)
    if (Rec.resourceGeneration != 0 &&
        std::find(Out.begin(), Out.end(), Rec.resourceGeneration) == Out.end())
      Out.push_back(Rec.resourceGeneration);
  llvm::sort(Out);
  return Out;
}

void EJitSmallTableHost::leave(uint64_t Ticket) {
  if (!runtime_)
    return;
  // Ticket0 is a refusal/no-policy path: no specialized execution was entered.
  if (Ticket == 0)
    return;
  leavePublished(Ticket);
}

void *EJitSmallTableHost::activeEntry() const {
  if (codeReady_ && t2Entry_)
    return t2Entry_;
  if (t1Ready_)
    return t1Entry_;
  return nullptr;
}

StringRef EJitSmallTableHost::activeTier() const {
  if (codeReady_ && t2Entry_)
    return "final";
  if (t1Ready_)
    return "instrumented";
  return "none";
}

void *EJitSmallTableHost::enterInstrumented(ArrayRef<uint32_t> DimTypes,
                                            ArrayRef<uint32_t> InstanceIds,
                                            uint64_t *OutTicket,
                                            std::string *Why) {
  if (OutTicket)
    *OutTicket = 0;
  if (!bound_ || !planReady_) {
    if (Why)
      *Why = "no small-table entry is bound to this function index";
    return nullptr;
  }
  SmallVector<uint64_t, 4> Coordinate;
  std::string Local;
  if (!coordinateOf(DimTypes, InstanceIds, Coordinate, &Local))
    return nullptr;
  // The sampling window is host-internal bookkeeping over real executions: the
  // runtime's own admission, epoch re-check, aggregate budget and protected read
  // borrow all apply, but the published-slot/code-ready gate does not, because
  // the window runs on the instrumented tier before any final code exists.
  EJitSmallTableSampleTicket Ticket;
  if (!runtime_->enterAdmitted(Coordinate, &Ticket, &Local)) {
    if (Why)
      *Why = Local;
    return nullptr;
  }
  // Every admitted T1 dispatch has a physical ticket through its real return.
  // The runtime owns its sampling borrow and generation reader; the
  // host token routes its actual completion back to that runtime after teardown.
  if (!Ticket.valid) {
    if (Why)
      *Why = "runtime admitted T1 without a physical execution ticket";
    return nullptr;
  }
  const uint64_t Token = allocateExecutionToken();
  if (Token == 0) {
    runtime_->leaveAdmitted(Ticket);
    if (Why)
      *Why = "small-table execution token space exhausted";
    return nullptr;
  }
  ++activeExecutions_;
  ExecutionRecord Rec;
  Rec.token = Token;
  Rec.isSample = true;
  Rec.ticket = Ticket;
  Rec.resourceGeneration = runtime_->resourceGeneration();
  Rec.codeGeneration = codeGeneration_;
  Rec.logicalSession = logicalSessionId_;
  Rec.runtimeSession = runtime_->sessionId();
  // enterAdmitted already acquired this sampling ticket's physical reader.
  if (OutTicket)
    *OutTicket = Rec.token;
  executions_.push_back(std::move(Rec));
  if (Why)
    Why->clear();
  return t1Entry_;
}

uint64_t EJitSmallTableHost::driveSampling(uint64_t MaxExecutions, int64_t Arg) {
  if (!bound_ || !t1Ready_ || slots_.empty())
    return 0;
  const uint64_t Budget = options_.samplingDriverLimit
                              ? options_.samplingDriverLimit
                              : runtime_->sampleBudget();
  uint64_t Done = 0;
  // Real executions through the ordinary enter/leave path, spread over the
  // members the contract admits. Only executions the runtime actually counts
  // consume the aggregate budget; once it is reached new calls use AOT until
  // the completed window is frozen and T2 published, so stop.
  size_t SlotCursor = 0;
  while (Done < MaxExecutions && runtime_->currentSessionSamples() < Budget &&
         !runtime_->samplingExhausted()) {
    const EJitSmallTableLogicalSlot *Pick = nullptr;
    const size_t N = slots_.size();
    for (size_t I = 0; I < N; ++I) {
      const size_t Index = (SlotCursor + I) % N;
      const EJitSmallTableLogicalSlot &Candidate = slots_[Index];
      if (Candidate.admission == EJitSmallTableAdmission::Compatible ||
          Candidate.admission == EJitSmallTableAdmission::Extendable) {
        Pick = &Candidate;
        SlotCursor = (Index + 1) % N;
        break;
      }
    }
    if (!Pick)
      break;
    SmallVector<uint32_t, 4> Types;
    SmallVector<uint32_t, 4> Instances;
    for (unsigned D = 0; D < dims_.size(); ++D) {
      Types.push_back(dimTypes_[D]);
      Instances.push_back(static_cast<uint32_t>(Pick->coordinate[D]));
    }
    std::string Why;
    uint64_t Ticket = 0;
    void *Entry = enterInstrumented(Types, Instances, &Ticket, &Why);
    if (!Entry)
      break;
    using EntryFn = int64_t (*)(uint64_t, uint64_t, int64_t);
    reinterpret_cast<EntryFn>(Entry)(
        static_cast<uint64_t>(Pick->coordinate[0]),
        dims_.size() > 1 ? static_cast<uint64_t>(Pick->coordinate[1]) : 0ull,
        Arg);
    leave(Ticket);
    ++Done;
  }
  return Done;
}

//===----------------------------------------------------------------------===//
// Configuration change and failure paths
//===----------------------------------------------------------------------===//

void EJitSmallTableHost::noteConfigurationChange(StringRef Reason) {
  notePolicyChange();
  const std::string R = Reason.empty() ? std::string("configuration changed")
                                       : Reason.str();
  retractPublishedSlots();
  beginLogicalSession();
  closeOutstandingExecutions();
  drainSlots(R);
  runtime_->noteGenerationChange(R);
  t1Ready_ = false;
  factRevision_ = facts_ ? facts_->configurationRevision() : 0;
  emptyReason_ = R;
}

void EJitSmallTableHost::cancel(StringRef Reason) {
  cancelImpl(Reason);
}

void EJitSmallTableHost::cancel(
    const detail::OwnerWorkerContext &Worker, StringRef Reason) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  EJit *Runtime = acquireSmallTableSreRuntime(Worker);
  if (!Runtime)
    return;
  struct RuntimePin {
    EJit *Runtime;
    const detail::OwnerWorkerContext &Worker;
    ~RuntimePin() { releaseSmallTableSreRuntime(Runtime, Worker); }
  } Pin{Runtime, Worker};
  EJitSharedTaskPool *Pool = Runtime->sharedTaskPool();
  if (!Pool || !Worker.activeFor(*Pool) || Runtime->smallTableHost() != this)
    return;
#else
  (void)Worker;
  return;
#endif
  cancelImpl(Reason);
}

void EJitSmallTableHost::cancelImpl(StringRef Reason) {
  notePolicyChange();
  const std::string R =
      Reason.empty() ? std::string("cancelled") : Reason.str();
  retractPublishedSlots();
  beginLogicalSession();
  closeOutstandingExecutions();
  drainSlots(R);
  runtime_->cancel(R);
  emptyReason_ = R;
}

uint64_t EJitSmallTableHost::retractPublishedSlots() {
  uint64_t Retracted = 0;
  for (const EJitSmallTableLogicalSlot &Slot : slots_)
    if (Slot.published())
      ++Retracted;
  if (Retracted == 0)
    return 0;
  // The real retraction path: the owner's inline-cache drain, which walks the
  // registered cells and writes each back to its empty value, so a call site
  // that already holds a specialized pointer stops reaching it.
  if (invalidationHook_)
    invalidationHook_();
  return Retracted;
}

Error EJitSmallTableHost::beginNextGeneration(
    ArrayRef<EJitSmallTableRowKey> ExtraMembers, std::string &Why) {
  if (!bound_ || !planReady_) {
    Why = "small-table host: beginNextGeneration before a successful plan";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  if (codeReady_ || publishedSlots() != 0) {
    // A published generation is not replaced under a reachable entry: the
    // caller drains first (noteConfigurationChange/cancel), which is what makes
    // the rebuild observable and keeps the lifetime rule.
    Why = "a published code generation is still reachable: drain it before "
          "preparing a new one";
    return make_error<StringError>(Why, inconvertibleErrorCode());
  }
  auto PlanOrErr = runtime_->beginNextGeneration(ExtraMembers, Why);
  if (!PlanOrErr)
    return PlanOrErr.takeError();
  // The new generation re-derives every member from the configuration facts, so
  // a previous lifecycle drain no longer applies.
  deactivated_.clear();
  t1Ready_ = false;
  codeGeneration_ = runtime_->resource() ? runtime_->resource()->generation()
                                         : codeGeneration_;
  factRevision_ = facts_ ? facts_->configurationRevision() : 0;
  return Error::success();
}

bool EJitSmallTableHost::retireGenerationsUpTo(uint64_t Generation,
                                               std::string &Why) {
  // Freeing the storage a published entry may still be executing against would
  // be a use-after-free, so refuse while any slot reads that generation. The
  // caller drains the slots first (noteConfigurationChange/cancel), which is
  // the same rule the runtime applies to its own current generation.
  if (codeReady_ && publishedResourceGeneration_ != 0 &&
      publishedResourceGeneration_ <= Generation) {
    Why = "a published slot still reads resource generation " +
          std::to_string(publishedResourceGeneration_);
    return false;
  }
  // A real execution still inside a generation of this range does NOT make the
  // retirement fail: it makes it DEFERRED. The runtime keeps the storage and its
  // bytes accounted until that execution's own `leave` drops the last reader;
  // `reclaimDeferredRetirements` then releases it. Logical cancellation is not
  // physical completion, so a cancel followed by a rebuild does not license the
  // free (spec §7 retained old generation, NO_RECLAIM without a proven safe
  // reclamation).
  if (!runtime_->retireGenerationsUpTo(Generation)) {
    Why = "the runtime refused to retire generation " +
          std::to_string(Generation) + " (above the current generation)";
    return false;
  }
  retirementWatermark_ = std::max(retirementWatermark_, Generation);
  reclaimDeferredRetirements();
  if (retiredExecutions_ != 0) {
    ++deferredRetirements_;
  }
  Why.clear();
  return true;
}

//===----------------------------------------------------------------------===//
// Product lifecycle boundary
//===----------------------------------------------------------------------===//

namespace {
/// Derive the member coordinates a lifecycle activation names.
///
/// An activation carries (lifecycle, instance), which fixes ONE dimension's
/// index and leaves the others open. The call is only unprovable when the
/// open dimensions have more than one admitted value among them, because then
/// the activation does not identify a single member. When the other dimensions
/// really do only take the single value 0 across every slot this host tracks,
/// the coordinate is unambiguous and the derivation succeeds.
///
/// This is deliberately a derivation over the host's ACTUAL admitted
/// coordinates, not over the declared extents: for the product case where one
/// lifecycle instance is activated per member (one dimension per lifecycle) the
/// other dimensions are single-valued by construction and the member is exact.
bool coordinatesFromActivation(ArrayRef<EJitSmallTableLogicalSlot> Slots,
                               ArrayRef<EJitSmallTableDim> Dims,
                               ArrayRef<std::string> PeriodNames,
                               StringRef PeriodName, uint32_t InstanceId,
                               std::vector<std::vector<uint64_t>> &Out,
                               std::string *Why) {
  Out.clear();
  int MatchDim = -1;
  for (unsigned D = 0; D < Dims.size(); ++D) {
    if (PeriodNames[D] != PeriodName)
      continue;
    if (MatchDim >= 0) {
      if (Why)
        *Why = "the lifecycle names more than one declared dimension";
      return false;
    }
    MatchDim = static_cast<int>(D);
  }
  if (MatchDim < 0) {
    if (Why)
      *Why = "lifecycle '" + PeriodName.str() +
             "' is not a declared dimension of this entry";
    return false;
  }
  // The instance index is NOT range-checked against the declared extent here:
  // the coordinate is validated against the exported admission contract below,
  // which is the gate that decides admission. A value outside the schema is
  // reported as an unusable member rather than as a derivation failure, so the
  // caller's reason names the real cause.

  // Every coordinate that carries this instance of the named dimension. When a
  // product binds only one lifecycle to an entry, an activation names the whole
  // member set of that instance rather than one coordinate; that is a real and
  // useful granularity, so the set is returned instead of the call being refused
  // as ambiguous. Admission and publication are still decided per coordinate.
  for (const EJitSmallTableLogicalSlot &Slot : Slots) {
    if (Slot.coordinate.size() != Dims.size())
      continue;
    if (Slot.coordinate[static_cast<unsigned>(MatchDim)] != InstanceId)
      continue;
    Out.push_back(Slot.coordinate);
  }
  if (Out.empty()) {
    if (Why)
      *Why = "no admitted member carries instance " + std::to_string(InstanceId) +
             " of lifecycle '" + PeriodName.str() + "'";
    return false;
  }
  return true;
}

/// The dimension a lifecycle activation names, or -1 when the name is not a
/// declared dimension of this entry.
int activationDimension(ArrayRef<EJitSmallTableDim> Dims,
                        ArrayRef<std::string> PeriodNames, StringRef PeriodName,
                        std::string *Why) {
  int MatchDim = -1;
  for (unsigned D = 0; D < Dims.size(); ++D) {
    if (PeriodNames[D] != PeriodName)
      continue;
    if (MatchDim >= 0) {
      if (Why)
        *Why = "the lifecycle names more than one declared dimension";
      return -1;
    }
    MatchDim = static_cast<int>(D);
  }
  if (MatchDim < 0 && Why)
    *Why = "lifecycle '" + PeriodName.str() +
           "' is not a declared dimension of this entry";
  return MatchDim;
}

/// Candidate coordinates an activation names. The named dimension takes the
/// activated instance; every other dimension takes each value it already has
/// among the admitted coordinates when the activation names a whole member set,
/// and additionally the synthetic all-zeros coordinate so that a member which
/// nothing has admitted yet is still offered to the contract. Admission decides
/// what happens to each candidate; nothing here publishes anything.
std::vector<std::vector<uint64_t>>
candidateCoordinates(ArrayRef<EJitSmallTableLogicalSlot> Slots,
                     ArrayRef<EJitSmallTableDim> Dims, int MatchDim,
                     uint32_t InstanceId) {
  std::vector<std::vector<uint64_t>> Out;
  const size_t NDims = Dims.size();
  for (const EJitSmallTableLogicalSlot &Slot : Slots) {
    if (Slot.coordinate.size() != NDims)
      continue;
    if (Slot.coordinate[static_cast<unsigned>(MatchDim)] != InstanceId)
      continue;
    if (std::find(Out.begin(), Out.end(), Slot.coordinate) == Out.end())
      Out.push_back(Slot.coordinate);
  }
  // The new member the activation really is about: the named dimension's
  // instance with every other dimension at the value the schema's own first
  // member carries. This candidate is only ever used to ask the contract; a
  // coordinate the contract refuses is not admitted and not published.
  std::vector<uint64_t> Fresh(NDims, 0);
  Fresh[static_cast<unsigned>(MatchDim)] = InstanceId;
  for (unsigned D = 0; D < NDims; ++D) {
    if (static_cast<int>(D) == MatchDim)
      continue;
    for (const EJitSmallTableLogicalSlot &Slot : Slots) {
      if (Slot.coordinate.size() != NDims)
        continue;
      Fresh[D] = Slot.coordinate[D];
      break;
    }
  }
  if (std::find(Out.begin(), Out.end(), Fresh) == Out.end())
    Out.push_back(std::move(Fresh));
  return Out;
}
} // namespace

bool EJitSmallTableHost::onProductActivated(StringRef PeriodName,
                                            uint32_t InstanceId,
                                            std::string *Why) {
  auto Refuse = [&](const std::string &Msg) {
    if (Why)
      *Why = Msg;
    return false;
  };
  if (!bound_ || !planReady_)
    return Refuse("no small-table entry is bound");

  std::string Local;
  const int MatchDim =
      activationDimension(dims_, dimPeriodNames_, PeriodName, &Local);
  if (MatchDim < 0)
    return Refuse(Local);
  std::vector<std::vector<uint64_t>> Coordinates =
      candidateCoordinates(slots_, dims_, MatchDim, InstanceId);

  // Activation is keyed by the lifecycle the product activated, so every
  // candidate member is validated against the configuration-commit facts'
  // exported contract. A candidate the contract refuses is not made usable by
  // an activation call.
  bool AnyAdmitted = false;
  for (const std::vector<uint64_t> &Coordinate : Coordinates) {
    std::string AdmitWhy;
    const EJitSmallTableAdmission Admission =
        runtime_->admitMember(Coordinate, &AdmitWhy);
    if (Admission == EJitSmallTableAdmission::Compatible ||
        Admission == EJitSmallTableAdmission::Extendable) {
      AnyAdmitted = true;
      // A product activation of an instance whose member this host already
      // admitted re-publishes it: the drain was a publication decision, not a
      // statement about the member's data.
      deactivated_.erase(
          std::remove(deactivated_.begin(), deactivated_.end(), Coordinate),
          deactivated_.end());
      continue;
    }
    if (Coordinate.size() > static_cast<size_t>(MatchDim) &&
        Coordinate[static_cast<unsigned>(MatchDim)] == InstanceId &&
        Admission == EJitSmallTableAdmission::Conflict)
      return Refuse(std::string("the member admission is ") +
                    admissionName(Admission) + ": " + AdmitWhy);
  }
  if (!AnyAdmitted) {
    if (Why)
      *Why = "no member of this activation is admissible under the exported "
             "contract";
    return false;
  }
  rebuildSlots();
  bool Published = false;
  for (const std::vector<uint64_t> &Coordinate : Coordinates) {
    const EJitSmallTableLogicalSlot *Slot = findSlot(Coordinate);
    if (Slot && Slot->published())
      Published = true;
  }
  if (!Published) {
    if (Why)
      *Why = "the slot is not published yet (no ready code generation for this "
             "coordinate); the entry stays AOT until it is";
    return false;
  }
  if (Why)
    Why->clear();
  return true;
}

bool EJitSmallTableHost::onProductDeactivated(StringRef PeriodName,
                                             uint32_t InstanceId) {
  lastRefusal_.clear();
  if (!bound_ || !planReady_)
    return false;
  std::vector<std::vector<uint64_t>> Coordinates;
  std::string Local;
  if (!coordinatesFromActivation(slots_, dims_, dimPeriodNames_, PeriodName,
                                 InstanceId, Coordinates, &Local)) {
    lastRefusal_ = Local.empty() ? "activation named no coordinate" : Local;
    return false;
  }
  bool Drained = false;
  for (EJitSmallTableLogicalSlot &Slot : slots_) {
    const bool Named = std::any_of(
        Coordinates.begin(), Coordinates.end(),
        [&](const std::vector<uint64_t> &C) {
          return Slot.coordinate.size() == C.size() &&
                 std::equal(Slot.coordinate.begin(), Slot.coordinate.end(),
                            C.begin());
        });
    if (!Named)
      continue;
    if (!Slot.published())
      continue;
    Slot.entry = nullptr;
    Slot.codeReady = false;
    Slot.codeGeneration = 0;
    const std::vector<uint64_t> Key = Slot.coordinate;
    if (std::find(deactivated_.begin(), deactivated_.end(), Key) ==
        deactivated_.end())
      deactivated_.push_back(Key);
    ++drainedSlotCount_;
    Drained = true;
  }
  if (!Drained && lastRefusal_.empty())
    lastRefusal_ = "no published slot matched the activation";
  return Drained;
}

//===----------------------------------------------------------------------===//
// Dispatch by coordinate
//===----------------------------------------------------------------------===//

EJitSmallTableDispatchResult
EJitSmallTableHost::dispatchMember(ArrayRef<uint64_t> Coordinate, int64_t Arg) {
  if (!bound_)
    return refuse(EJitSmallTableDispatch::NotBound,
                  "no small-table entry is bound to this function index");
  SmallVector<uint32_t, 4> Types;
  SmallVector<uint32_t, 4> Instances;
  for (unsigned D = 0; D < dims_.size(); ++D) {
    if (D >= Coordinate.size())
      return refuse(EJitSmallTableDispatch::CoordinateUnprovable,
                    "the coordinate is shorter than the declared schema");
    Types.push_back(dimTypes_[D]);
    Instances.push_back(static_cast<uint32_t>(Coordinate[D]));
  }
  return dispatch(Types, Instances, Arg);
}

std::vector<std::vector<uint64_t>>
EJitSmallTableHost::admittedCoordinates() const {
  std::vector<std::vector<uint64_t>> Out;
  Out.reserve(slots_.size());
  for (const EJitSmallTableLogicalSlot &Slot : slots_)
    if (Slot.admission == EJitSmallTableAdmission::Compatible ||
        Slot.admission == EJitSmallTableAdmission::Extendable)
      Out.push_back(Slot.coordinate);
  return Out;
}

//===----------------------------------------------------------------------===//
// Accounting
//===----------------------------------------------------------------------===//

EJitSmallTableHost::ByteAccounting EJitSmallTableHost::accounting() const {
  ByteAccounting A;
  if (EJitSmallTableTableResource *R = runtime_->resource()) {
    const EJitSmallTableTableResource::Accounting RA = R->accounting();
    A.tablePayloadBytes = RA.payloadBytes;
    A.tableAllocatedBytes = RA.allocatedBytes;
    A.tablePublishedBytes = RA.publishedBytes;
  }
  A.retainedBytes = runtime_->retainedBytes();
  A.retainedGenerations = runtime_->retainedGenerationCount();
#if defined(EJIT_SRE_CODE_POOL)
  // Code bytes are only knowable when the active engine can resolve the
  // published entry to a finalized code range. Reported separately from table
  // bytes; 0 means "not measurable through this engine", never "no code".
  if (void *E = const_cast<void *>(activeEntry())) {
    EJitCompiledCodeInfo Info;
    if (runtime_->engine().findCodeRange(E, Info))
      A.publishedCodeBytes = Info.codeSize;
  }
#endif
  return A;
}

} // namespace ejit
} // namespace llvm
