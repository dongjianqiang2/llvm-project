//===-- EJitSmallTableRuntime.cpp - online small-table runtime (B0/B1/B2) --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// B0: readiness facts + protected read borrow + fail-closed provider.
// B1: one shared fixed-capacity table resource per generation, bound into every
//     compile of that generation, with incremental publication of validated
//     rows and an explicit contract check for late members.
// B2: ONE common sampling session per entry/code generation with an aggregate,
//     configurable budget of real admitted entries, an immutable frozen bundle
//     synthesized from the real Tier-1 counters, and a common T2 compiled and
//     executed from that bundle.
//
// Everything here drives the real planner/pass/ORC engine. The only stand-in is
// the readiness provider: when the product configuration transaction is not
// available, `EJitSmallTableHostProvider` (labeled
// "host-adapter.pr231-not-product") supplies the facts, and the runtime refuses
// to specialize when no provider is installed at all.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSmallTableRuntime.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#endif
#include "llvm/IR/Module.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace llvm {
namespace ejit {

//===----------------------------------------------------------------------===//
// B0: borrow and host adapter
//===----------------------------------------------------------------------===//

EJitSmallTableReadBorrow::EJitSmallTableReadBorrow(
    EJitSmallTableReadinessProvider *Owner, uint64_t Epoch,
    EJitSmallTableSource Source)
    : owner_(Owner), epoch_(Epoch), source_(Source) {}

EJitSmallTableReadBorrow::~EJitSmallTableReadBorrow() { release(); }

void EJitSmallTableReadBorrow::invalidate(StringRef Reason) {
  if (invalidation_.empty())
    invalidation_ = Reason.empty() ? std::string("invalidated") : Reason.str();
}

void EJitSmallTableReadBorrow::release() {
  if (released_)
    return;
  released_ = true;
  source_ = EJitSmallTableSource{};
  if (owner_)
    owner_->onBorrowReleased(this);
}

EJitSmallTableHostProvider::EJitSmallTableHostProvider(std::string SourceVarName,
                                                       const void *Base,
                                                       uint64_t Bytes,
                                                       uint64_t Epoch)
    : sourceVarName_(std::move(SourceVarName)), base_(Base), bytes_(Bytes),
      epoch_(Epoch) {}

void EJitSmallTableHostProvider::addReadyMember(ArrayRef<uint64_t> Indices,
                                                uint64_t ConfigGeneration,
                                                bool FieldsInitialized) {
  EJitSmallTableReadyMember M;
  M.indices.append(Indices.begin(), Indices.end());
  M.configGeneration = ConfigGeneration;
  M.fieldsInitialized = FieldsInitialized;
  members_.push_back(std::move(M));
}

void EJitSmallTableHostProvider::invalidateGeneration() {
  generationInvalidated_ = true;
}

void EJitSmallTableHostProvider::refuseBorrow(std::string Reason) {
  borrowRefusal_ = std::move(Reason);
}

std::unique_ptr<EJitSmallTableReadBorrow>
EJitSmallTableHostProvider::borrow(StringRef SourceVarName, std::string &Error) {
  // The adapter stands in for the product's configuration/completion point, so
  // it refuses exactly the cases that point would refuse: a missing fact source,
  // a moved generation, or a region that is not the one the epoch describes.
  if (!borrowRefusal_.empty()) {
    Error = borrowRefusal_;
    return nullptr;
  }
  if (generationInvalidated_) {
    Error = "configuration generation moved; the old borrow is not valid";
    return nullptr;
  }
  if (SourceVarName != sourceVarName_) {
    Error = "provider covers a different source region";
    return nullptr;
  }
  if (!base_ || bytes_ == 0) {
    Error = "provider has no readable source region";
    return nullptr;
  }
  ++borrowCount_;
  ++outstandingBorrows_;
  return std::make_unique<EJitSmallTableReadBorrow>(
      this, epoch_, EJitSmallTableSource{static_cast<const uint8_t *>(base_),
                                         bytes_});
}

void EJitSmallTableHostProvider::onBorrowReleased(
    EJitSmallTableReadBorrow *Borrow) {
  (void)Borrow;
  if (outstandingBorrows_ > 0)
    --outstandingBorrows_;
}

//===----------------------------------------------------------------------===//
// B1: shared table resource
//===----------------------------------------------------------------------===//

namespace {

/// Projected coordinate of \p Indices under \p Field's retained axes: exactly
/// the index the lowered IR computes for this column.
uint64_t projectCoordinate(ArrayRef<EJitSmallTableDim> Dims,
                           const EJitSmallTableFieldContract &Field,
                           ArrayRef<uint64_t> Indices) {
  uint64_t Coord = 0;
  for (unsigned Pos = 0; Pos < Field.retainedAxes.size(); ++Pos) {
    const unsigned Axis = Field.retainedAxes[Pos];
    if (Axis >= Dims.size() || Axis >= Indices.size())
      return std::numeric_limits<uint64_t>::max();
    uint64_t Stride = 1;
    for (unsigned Later = Pos + 1; Later < Field.retainedAxes.size(); ++Later) {
      const unsigned LaterAxis = Field.retainedAxes[Later];
      if (LaterAxis >= Dims.size())
        return std::numeric_limits<uint64_t>::max();
      Stride *= Dims[LaterAxis].extent;
    }
    Coord += Indices[Axis] * Stride;
  }
  return Coord;
}

} // namespace

EJitSmallTableTableResource::~EJitSmallTableTableResource() {
  if (sharedAllocation_.identity) {
#ifdef EJIT_SRE_CODE_POOL
    // Logical retirement occurs only after the existing real-reader drain.
    // The DataOnly backend never recycles the physical address, even then.
    if (!releaseSreSmallTableStorage(sharedAllocation_))
      EJIT_DIAG("small-table DataOnly retirement identity refused; physical storage retained");
#endif
  } else {
    delete[] base_;
  }
  base_ = nullptr;
}

std::unique_ptr<EJitSmallTableTableResource>
EJitSmallTableTableResource::create(const EJitSmallTablePlan &Plan,
                                    uint64_t Generation,
                                    uint64_t CapacityLimit,
                                    std::string &Error) {
  return create(Plan, Generation, CapacityLimit, Error, nullptr, false);
}

std::unique_ptr<EJitSmallTableTableResource>
EJitSmallTableTableResource::create(const EJitSmallTablePlan &Plan,
                                   uint64_t Generation,
                                   uint64_t CapacityLimit,
                                   std::string &Error,
                                   EJitSharedTaskPool *SharedDataPool,
                                   bool RequireSharedData) {
  auto Refuse = [&](const Twine &Msg)
      -> std::unique_ptr<EJitSmallTableTableResource> {
    Error = Msg.str();
    return nullptr;
  };

  auto Res = std::unique_ptr<EJitSmallTableTableResource>(
      new EJitSmallTableTableResource(Plan.littleEndian));
  Res->generation_ = Generation;

  uint64_t Offset = 0;
  for (unsigned I = 0; I < Plan.fields.size(); ++I) {
    const EJitSmallTableField &Field = Plan.fields[I];
    if (Field.strategy == EJitSmallTableStrategy::Uniform)
      continue;
    if (Field.columnName.empty() || Field.accessSize == 0 ||
        Field.accessSize > 8 || Field.tableRows == 0)
      return Refuse("table field has no usable column shape");
    const uint64_t Align = 8;
    Offset = (Offset + (Align - 1)) & ~(Align - 1);
    uint64_t Payload = 0;
    if (__builtin_mul_overflow(Field.tableRows, Field.accessSize, &Payload) ||
        __builtin_add_overflow(Offset, Payload, &Offset))
      return Refuse("table resource size overflow");

    Column C;
    C.symbolName = Field.columnName;
    C.offset = Offset - Payload;
    C.rows = Field.tableRows;
    C.elementBytes = Field.accessSize;
    C.payloadBytes = Payload;
    C.retainedAxes = Field.retainedAxes;
    C.fieldIndex = I;
    Res->columns_.push_back(std::move(C));
    Res->published_.emplace_back(
        static_cast<size_t>(Field.tableRows), std::make_pair(false, uint64_t{0}));
  }

  // Capacity budget (spec §8/§13): a plan above the declared capacity is
  // refused rather than silently truncated or re-mapped onto existing rows.
  if (Offset > CapacityLimit)
    return Refuse("table resource needs " + Twine(Offset) +
                  " bytes, above the capacity budget of " +
                  Twine(CapacityLimit));
  if (Offset == 0) {
    // A uniform-only plan needs no storage at all; that is legal and costs zero
    // bytes, so keep an empty resource rather than failing.
    Res->capacityBytes_ = 0;
    return Res;
  }

  if (RequireSharedData) {
#if defined(EJIT_SRE_CODE_POOL) && defined(EJIT_SRE_SHARED_TASKPOOL)
    if (!SharedDataPool)
      return Refuse("small-table DataOnly storage has no owner preparation pool");
    EJitSreDataAllocation Allocation{};
    if (llvm::Error E = allocateSreSmallTableStorage(Offset, Generation, Allocation))
      return Refuse(toString(std::move(E)));
    if (!SharedDataPool->prepareExternalSharedData(Allocation)) {
      if (!releaseSreSmallTableStorage(Allocation, true))
        EJIT_DIAG("small-table DataOnly failed preparation could not close its exact metadata record; storage remains charged and retained");
      return Refuse("small-table DataOnly permissions refused before initialization");
    }
    Res->sharedAllocation_ = Allocation;
    Res->base_ = reinterpret_cast<uint8_t *>(Allocation.address);
    Res->allocatedBytes_ = Allocation.bytes;
    std::memset(Res->base_, 0, static_cast<size_t>(Allocation.bytes));
#else
    return Refuse("small-table DataOnly storage requires the shared SRE code-pool adapter");
#endif
  } else {
    Res->base_ = new uint8_t[static_cast<size_t>(Offset)]();
    Res->allocatedBytes_ = Offset;
  }
  Res->capacityBytes_ = Offset;
  return Res;
}

const EJitSmallTableTableResource::Column *
EJitSmallTableTableResource::findColumn(StringRef Symbol) const {
  for (const Column &C : columns_)
    if (C.symbolName == Symbol)
      return &C;
  return nullptr;
}

const EJitSmallTableTableResource::Column *
EJitSmallTableTableResource::columnForField(unsigned FieldIndex) const {
  for (const Column &C : columns_)
    if (C.fieldIndex == FieldIndex)
      return &C;
  return nullptr;
}

void *EJitSmallTableTableResource::columnAddress(unsigned FieldIndex) const {
  const Column *C = columnForField(FieldIndex);
  if (!C || !base_)
    return nullptr;
  return base_ + C->offset;
}

EJitSmallTableTableResource::PublishResult
EJitSmallTableTableResource::publish(unsigned FieldIndex, uint64_t Coordinate,
                                     uint64_t Bits) {
  const Column *C = columnForField(FieldIndex);
  if (!C)
    return PublishResult::NotATable;
  if (Coordinate >= C->rows)
    return PublishResult::OutOfRange;
  std::vector<std::pair<bool, uint64_t>> &State =
      published_[static_cast<size_t>(C - columns_.data())];
  std::pair<bool, uint64_t> &Slot = State[static_cast<size_t>(Coordinate)];
  if (Slot.first)
    return Slot.second == Bits ? PublishResult::AlreadySame
                               : PublishResult::Conflict;
  if (!base_)
    return PublishResult::Conflict;
  // The stored value is the typed bit pattern masked to the field width; the
  // element bytes are the field's own access size, serialized in the plan's
  // target byte order. Neither the compiling host nor later plan mutations
  // determine the representation of this resource.
  uint8_t *Dst = base_ + C->offset + Coordinate * C->elementBytes;
  std::memset(Dst, 0, static_cast<size_t>(C->elementBytes));
  const unsigned Bytes = static_cast<unsigned>(C->elementBytes);
  for (unsigned B = 0; B < Bytes; ++B) {
    const unsigned BitOffset = 8 * (littleEndian_ ? B : Bytes - 1 - B);
    Dst[B] = static_cast<uint8_t>((Bits >> BitOffset) & 0xff);
  }
  Slot.first = true;
  Slot.second = Bits;
  return PublishResult::Stored;
}

bool EJitSmallTableTableResource::published(unsigned FieldIndex,
                                            uint64_t Coordinate,
                                            uint64_t *Bits) const {
  const Column *C = columnForField(FieldIndex);
  if (!C || Coordinate >= C->rows)
    return false;
  const std::vector<std::pair<bool, uint64_t>> &State =
      published_[static_cast<size_t>(C - columns_.data())];
  const std::pair<bool, uint64_t> &Slot = State[static_cast<size_t>(Coordinate)];
  if (!Slot.first)
    return false;
  if (Bits)
    *Bits = Slot.second;
  return true;
}

std::vector<std::pair<uint64_t, uint64_t>>
EJitSmallTableTableResource::publishedValues(unsigned FieldIndex) const {
  std::vector<std::pair<uint64_t, uint64_t>> Out;
  const Column *C = columnForField(FieldIndex);
  if (!C)
    return Out;
  const std::vector<std::pair<bool, uint64_t>> &State =
      published_[static_cast<size_t>(C - columns_.data())];
  for (uint64_t P = 0; P < State.size(); ++P)
    if (State[static_cast<size_t>(P)].first)
      Out.push_back({P, State[static_cast<size_t>(P)].second});
  return Out;
}

EJitSmallTableTableResource::Accounting
EJitSmallTableTableResource::accounting() const {
  Accounting A;
  A.allocatedBytes = allocatedBytes_;
  for (const Column &C : columns_) {
    A.payloadBytes += C.payloadBytes;
    const std::vector<std::pair<bool, uint64_t>> &State =
        published_[static_cast<size_t>(&C - columns_.data())];
    for (const std::pair<bool, uint64_t> &Slot : State)
      if (Slot.first) {
        A.publishedCells += 1;
        A.publishedBytes += C.elementBytes;
      }
  }
  A.reservedBytes = capacityBytes_;
  return A;
}

//===----------------------------------------------------------------------===//
// Runtime
//===----------------------------------------------------------------------===//

EJitSmallTableRuntime::~EJitSmallTableRuntime() = default;

Expected<std::unique_ptr<EJitSmallTableRuntime>>
EJitSmallTableRuntime::create(
    const Config &Cfg, PeriodArrayRegistry &Registry, EJitRuntimeState &State,
    std::shared_ptr<EJitSmallTableReadinessProvider> Provider) {
  return create(Cfg, Registry, State, std::move(Provider), Options());
}

Expected<std::unique_ptr<EJitSmallTableRuntime>>
EJitSmallTableRuntime::create(
    const Config &Cfg, PeriodArrayRegistry &Registry, EJitRuntimeState &State,
    std::shared_ptr<EJitSmallTableReadinessProvider> Provider, Options Opts) {
#if defined(EJIT_FREESTANDING) && defined(EJIT_FIXED_CODE_POOL)
  // Historical per-engine private cursors start at the SAME linker reservation
  // and could overwrite ordinary code still executing. Only the explicit,
  // validated, monotonic shared domain enables a second common Engine. This
  // proves allocation separation, not product cross-core board readiness.
  if (!sreSharedFixedCodePoolDomainActive())
    return make_error<StringError>(
        "small-table fixed code pool requires a non-overlapping allocation domain",
        inconvertibleErrorCode());
  Opts.requireSharedDataStorage = true;
#endif
  auto EngineOrErr = EJitOrcEngine::Create(Cfg, Registry, State);
  if (!EngineOrErr)
    return EngineOrErr.takeError();

  auto RT = std::unique_ptr<EJitSmallTableRuntime>(new EJitSmallTableRuntime());
  RT->config_ = Cfg;
  RT->registry_ = &Registry;
  RT->state_ = &State;
  RT->provider_ = std::move(Provider);
  RT->options_ = Opts;
  RT->engine_ = std::move(*EngineOrErr);
  RT->providerLabel_ = RT->provider_ ? RT->provider_->label().str() : std::string();
  return std::move(RT);
}

Expected<const EJitSmallTablePlan *>
EJitSmallTableRuntime::prepare(Module &M, StringRef EntryName,
                               StringRef SourceVarName,
                               ArrayRef<EJitSmallTableDim> Dims,
                               std::string &Error) {
  // Fail closed: with no provider there is no readiness fact, no legal member
  // set and no borrow, so nothing may be specialized (spec §6.1.1).
  if (!provider_) {
    Error = "no readiness provider: refusing to specialize an unproven domain";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  if (plan_) {
    Error = "this runtime already prepared an entry";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }

  std::unique_ptr<EJitSmallTableReadBorrow> Borrow =
      provider_->borrow(SourceVarName, Error);
  if (!Borrow)
    return make_error<StringError>(Error, inconvertibleErrorCode());

  const uint64_t Epoch = provider_->domainEpoch();
  if (Epoch == 0) {
    Error = "readiness provider supplied no domain epoch: the generation could "
            "not be re-checked before publication";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  domainEpoch_ = Epoch;

  // Only members the provider confirmed are ready, and only those whose
  // coordinate matches the declared schema, become the proven domain.
  SmallVector<EJitSmallTableRowKey, 64> Rows;
  for (const EJitSmallTableReadyMember &Member : provider_->readyMembers()) {
    if (!Member.fieldsInitialized)
      continue;
    if (Member.indices.size() != Dims.size())
      continue;
    if (llvm::any_of(llvm::enumerate(Dims), [&](const auto &Pair) {
          return Member.indices[Pair.index()] >= Pair.value().extent;
        }))
      continue;
    EJitSmallTableRowKey Key;
    Key.indices.append(Member.indices.begin(), Member.indices.end());
    Rows.push_back(std::move(Key));
  }
  if (Rows.empty()) {
    Error = "readiness provider confirms no ready member of the declared "
            "schema: an empty domain is not a proof";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }

  EJitSmallTableRequest Req;
  Req.module = &M;
  Req.entryName = EntryName;
  Req.sourceVarName = SourceVarName;
  Req.dims = Dims;
  Req.source = Borrow->source();
  Req.authorizedRows = Rows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness.domainEpoch = Epoch;
  Req.readiness.providerLabel = provider_->label().str();
  Req.readiness.coversDeclaredDomain = provider_->coversDeclaredDomain();
  Req.readiness.borrowedStable = true;

  auto Planned = EJitSmallTablePlanner::plan(Req, Error);
  if (!Planned)
    return make_error<StringError>(Error, inconvertibleErrorCode());
  if (!Borrow->source().baseAddr) {
    Error = "the read borrow was invalidated during planning";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  Borrow->release();

  // The executable gate for this plan is the runtime's per-member admission, so
  // the pass emits declarations for a runtime-owned resource instead of baking
  // the proven rows into the module.
  Planned->runtimeRowAdmission = true;
  Planned->storage = EJitSmallTableStorage::RuntimeOwned;
  if (!Planned->verifyProjections(&Error))
    return make_error<StringError>(Error, inconvertibleErrorCode());

  auto Res = EJitSmallTableTableResource::create(
      *Planned, Epoch, options_.resourceCapacityLimit, Error,
      options_.sharedDataPool, options_.requireSharedDataStorage);
  if (!Res)
    return make_error<StringError>(Error, inconvertibleErrorCode());
  resource_ = std::move(Res);

  plan_ = std::make_shared<const EJitSmallTablePlan>(std::move(*Planned));
  {
    auto MutableSet = std::make_shared<EJitSmallTablePlanSet>();
    MutableSet->add(plan_);
    planSet_ = MutableSet;
  }

  // The published-projection set starts empty: nothing is published until a
  // validated member is admitted. `buildAdmissionContract` pre-fills the plan's
  // proven rows, which is right for the compiler-emitted form and wrong for a
  // runtime-owned resource, so the runtime maintains it explicitly.
  contract_ = buildAdmissionContract(*plan_);
  for (EJitSmallTableFieldContract &FC : contract_.fields)
    if (FC.strategy == EJitSmallTableStrategy::Table)
      FC.publishedValues.clear();

  // Keep the PRE-declaration source: a later generation re-parses this instead of
  // inheriting the previous generation's column declarations.
  raw_string_ostream SrcOS(sourceBitcode_);
  WriteBitcodeToFile(M, SrcOS);
  SrcOS.flush();

  // Declare the resource columns in the module and serialize this generation's
  // source for the T1/T2 compiles.
  std::string MaterializeError;
  if (!EJitSmallTablePass::materialize(M, *plan_, &MaterializeError)) {
    Error = MaterializeError;
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  raw_string_ostream OS(bitcode_);
  WriteBitcodeToFile(M, OS);
  OS.flush();

  engine_->setSmallTablePlans(planSet_);
  stats_.plannedRows = plan_->numRows();
  stats_.readyRows = plan_->readyRowCount();
  return plan_.get();
}

Expected<const EJitSmallTablePlan *>
EJitSmallTableRuntime::beginNextGeneration(
    ArrayRef<EJitSmallTableRowKey> ExtraMembers, std::string &Error) {
  auto Refuse = [&](const Twine &Msg) -> Expected<const EJitSmallTablePlan *> {
    Error = Msg.str();
    return make_error<StringError>(Error, inconvertibleErrorCode());
  };

  if (!plan_ || !resource_)
    return Refuse("beginNextGeneration before prepare");
  if (pendingGeneration_)
    return Refuse("a prepared generation has not been compiled yet");
  if (sessionInFlight_ != 0)
    return Refuse("an admitted sampling execution is still in flight: refusing "
                  "to replace the table generation it may still read");
  if (!provider_ || !provider_->epochCurrent(domainEpoch_))
    return Refuse("configuration generation moved: prepare a fresh runtime");

  // Union of every member this runtime already tracks plus the new members, so a
  // member's dependency identity survives even when an axis or column vanishes
  // (spec §6.6/§8).
  std::vector<std::vector<uint64_t>> Union;
  auto AddKey = [&Union](ArrayRef<uint64_t> Indices) {
    for (const std::vector<uint64_t> &K : Union)
      if (K.size() == Indices.size() &&
          std::equal(K.begin(), K.end(), Indices.begin()))
        return;
    Union.emplace_back(Indices.begin(), Indices.end());
  };
  for (const auto &Entry : admitted_)
    AddKey(Entry.first);
  for (const EJitSmallTableRowKey &Key : ExtraMembers)
    AddKey(Key.indices);
  if (Union.empty())
    return Refuse("no member to migrate into a new generation");

  // Every union member must still hold a confirmed-ready fact: a member that
  // lost its readiness is never baked into a new generation.
  SmallVector<EJitSmallTableRowKey, 64> Rows;
  for (const std::vector<uint64_t> &K : Union) {
    if (K.size() != contract_.dims.size())
      return Refuse("member coordinate does not match the declared schema");
    for (unsigned D = 0; D < K.size(); ++D)
      if (K[D] >= contract_.dims[D].extent)
        return Refuse("member coordinate leaves the declared extents");
    bool Ready = false;
    for (const EJitSmallTableReadyMember &M : provider_->readyMembers())
      if (M.fieldsInitialized && M.indices.size() == K.size() &&
          std::equal(M.indices.begin(), M.indices.end(), K.begin())) {
        Ready = true;
        break;
      }
    if (!Ready)
      return Refuse("the readiness provider no longer confirms a member of the "
                    "union as ready");
    EJitSmallTableRowKey Key;
    Key.indices.append(K.begin(), K.end());
    Rows.push_back(std::move(Key));
  }

  // Re-parse this entry's own source module (the one captured before the column
  // declarations), so a new generation never inherits the previous one's
  // declarations.
  if (sourceBitcode_.empty())
    return Refuse("no captured source module to re-plan");
  // A module's destructor still uses its LLVMContext. On the second rebuild
  // the previous parsed module must die BEFORE replacing its context.
  generationModule_.reset();
  generationContext_ = std::make_unique<LLVMContext>();
  auto Buf = MemoryBuffer::getMemBuffer(sourceBitcode_, "small-table-source",
                                        /*RequiresNullTerminator=*/false);
  auto ModOrErr = parseBitcodeFile(Buf->getMemBufferRef(), *generationContext_);
  if (!ModOrErr) {
    Error = toString(ModOrErr.takeError());
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  generationModule_ = std::move(*ModOrErr);

  std::unique_ptr<EJitSmallTableReadBorrow> Borrow =
      provider_->borrow(contract_.sourceVarName, Error);
  if (!Borrow)
    return make_error<StringError>(Error, inconvertibleErrorCode());

  EJitSmallTableRequest Req;
  Req.module = generationModule_.get();
  Req.entryName = contract_.entryName;
  Req.sourceVarName = contract_.sourceVarName;
  Req.dims = contract_.dims;
  Req.source = Borrow->source();
  Req.authorizedRows = Rows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness.domainEpoch = domainEpoch_;
  Req.readiness.providerLabel = providerLabel_;
  Req.readiness.coversDeclaredDomain = provider_->coversDeclaredDomain();
  Req.readiness.borrowedStable = true;

  // The solver runs per field over the union, so only the fields whose values
  // actually changed widen.
  auto Planned = EJitSmallTablePlanner::plan(Req, Error);
  if (!Planned)
    return make_error<StringError>(Error, inconvertibleErrorCode());
  Planned->runtimeRowAdmission = true;
  Planned->storage = EJitSmallTableStorage::RuntimeOwned;
  if (!Planned->verifyProjections(&Error))
    return make_error<StringError>(Error, inconvertibleErrorCode());

  // Retention budget (spec §8): the previous generation stays alive for code
  // that may still dispatch to it. Check before claiming another monotonic
  // allocation; charge the actual page-rounded backing, not only its payload.
  if (retainedBytes_ > options_.retentionCapacityLimit ||
      resource_->allocatedBytes() > options_.retentionCapacityLimit - retainedBytes_) {
    Error = ("retention budget exceeded: keeping generation " +
             Twine(resource_->generation()) + " alive needs " +
             Twine(retainedBytes_ + resource_->allocatedBytes()) +
             " bytes, above the limit of " +
             Twine(options_.retentionCapacityLimit))
                .str();
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }

  auto NewResource = EJitSmallTableTableResource::create(
      *Planned, resource_->generation() + 1, options_.resourceCapacityLimit,
      Error, options_.sharedDataPool, options_.requireSharedDataStorage);
  if (!NewResource)
    return make_error<StringError>(Error, inconvertibleErrorCode());

  // Migrate every union member into the NEW resource before anything is
  // committed: reading, validation and publication all happen against the new
  // plan, and a failure refuses the whole change with the old generation still
  // in place.
  EJitSmallTableContract NewContract = buildAdmissionContract(*Planned);
  for (EJitSmallTableFieldContract &FC : NewContract.fields)
    if (FC.strategy == EJitSmallTableStrategy::Table)
      FC.publishedValues.clear();

  uint64_t MigratedRows = 0;
  for (const EJitSmallTableRowKey &Key : Rows) {
    std::string ReadError;
    auto Member = readAdmissionMember(NewContract, Borrow->source(), Key.indices,
                                      ReadError);
    if (!Member)
      return Refuse(ReadError);
    std::string WhyAdmission;
    const EJitSmallTableAdmission Class =
        validateAdmission(NewContract, *Member, &WhyAdmission);
    if (Class != EJitSmallTableAdmission::Compatible &&
        Class != EJitSmallTableAdmission::Extendable)
      return Refuse("member cannot be migrated into the new generation: " +
                    WhyAdmission);
    for (unsigned F = 0; F < NewContract.fields.size(); ++F) {
      const EJitSmallTableFieldContract &FC = NewContract.fields[F];
      if (FC.strategy != EJitSmallTableStrategy::Table)
        continue;
      const uint64_t Coord =
          projectCoordinate(NewContract.dims, FC, Key.indices);
      if (Coord == std::numeric_limits<uint64_t>::max())
        return Refuse("member projection leaves the retained-axis schema");
      if (NewResource->publish(F, Coord, Member->bits[F]) ==
          EJitSmallTableTableResource::PublishResult::Conflict)
        return Refuse("migrated member conflicts inside the new resource");
      ++MigratedRows;
    }
  }

  // Generation re-check after the last read and before the new resource becomes
  // the one dispatchable code is bound to (spec §6.1.1/§6.3).
  if (!provider_->epochCurrent(domainEpoch_) || Borrow->stale()) {
    Borrow->invalidate("configuration generation moved during migration");
    return Refuse("configuration generation moved during migration");
  }
  Borrow->release();

  // Commit: retain the previous resource, adopt the new generation and declare
  // its columns in the freshly parsed module.
  resource_.swap(NewResource);
  retained_.push_back(std::move(NewResource));
  retainedBytes_ += retained_.back()->allocatedBytes();
  plan_ = std::make_shared<const EJitSmallTablePlan>(std::move(*Planned));
  contract_ = std::move(NewContract);
  {
    auto MutableSet = std::make_shared<EJitSmallTablePlanSet>();
    MutableSet->add(plan_);
    planSet_ = MutableSet;
  }
  std::string MaterializeError;
  if (!EJitSmallTablePass::materialize(*generationModule_, *plan_,
                                       &MaterializeError)) {
    Error = MaterializeError;
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  bitcode_.clear();
  raw_string_ostream OS(bitcode_);
  WriteBitcodeToFile(*generationModule_, OS);
  OS.flush();

  engine_->setSmallTablePlans(planSet_);
  codeGeneration_ = codeGeneration_ + 1;
  pendingGeneration_ = true;
  stats_.generationsPrepared++;
  stats_.migratedRows += MigratedRows;
  // The previous generation's session is over: its samples belong to the old
  // contract/resource identity and can never be merged into a new one.
  cancel("table generation changed: a new generation is being prepared");
  sessionSamples_ = 0;
  sampleKeys_.clear();
  bundle_.reset();
  frozen_ = false;
  return plan_.get();
}

bool EJitSmallTableRuntime::retireGenerationsUpTo(uint64_t Generation) {
  if (!resource_)
    return false;
  // Never free the storage this runtime currently publishes into: an
  // above-current request is refused rather than obeyed.
  if (Generation > resource_->generation())
    return false;
  // PHYSICAL PROTECTION (spec §7): a generation whose columns a real execution
  // is still reading is not freed here. Its retirement is recorded and the
  // storage is released by `releaseReader` when the last reader of that
  // generation leaves. `inFlight_` alone is NOT the criterion: a cancelled
  // session zeroes the logical in-flight bookkeeping while the execution that
  // already loaded the compiled address is still inside it.
  freeRetainedUpTo(Generation);
  return true;
}

void EJitSmallTableRuntime::freeRetainedUpTo(uint64_t Generation) {
  for (auto It = retained_.begin(); It != retained_.end();) {
    const uint64_t G = (*It)->generation();
    if (G > Generation) {
      ++It;
      continue;
    }
    if (physicalReaders(G) != 0) {
      // A real execution is inside this generation. Keep the storage (and its
      // bytes in the retention accounting) and remember that it must go as soon
      // as that reader returns.
      if (retired_.find(G) == retired_.end()) {
        retired_[G] = physicalReaders(G);
        stats_.deferredRetirements++;
      }
      ++It;
      continue;
    }
    retainedBytes_ -= (*It)->allocatedBytes();
    It = retained_.erase(It);
    stats_.retiredGenerations++;
  }
}

void EJitSmallTableRuntime::freeIfUnread(uint64_t Generation) {
  if (physicalReaders(Generation) != 0 ||
      retired_.find(Generation) == retired_.end())
    return;
  for (auto It = retained_.begin(); It != retained_.end(); ++It) {
    if ((*It)->generation() != Generation)
      continue;
    retainedBytes_ -= (*It)->allocatedBytes();
    retained_.erase(It);
    stats_.retiredGenerations++;
    break;
  }
  if (retired_.erase(Generation) != 0)
    stats_.reclaimedAfterReaders++;
}

uint64_t EJitSmallTableRuntime::reclaimRetiredGenerations() {
  uint64_t Released = 0;
  // Collect first: `freeIfUnread` erases from `retired_`.
  std::vector<uint64_t> Due;
  for (const auto &Entry : retired_)
    if (physicalReaders(Entry.first) == 0)
      Due.push_back(Entry.first);
  for (uint64_t G : Due) {
    freeIfUnread(G);
    ++Released;
  }
  return Released;
}

uint64_t EJitSmallTableRuntime::physicalReaders(uint64_t Generation) const {
  auto It = readers_.find(Generation);
  return It == readers_.end() ? 0 : It->second;
}

std::vector<uint64_t> EJitSmallTableRuntime::physicallyReadGenerations() const {
  std::vector<uint64_t> Out;
  for (const auto &Entry : readers_)
    if (Entry.second != 0)
      Out.push_back(Entry.first);
  return Out;
}

uint64_t EJitSmallTableRuntime::pendingRetireBytes() const {
  uint64_t Bytes = 0;
  for (const auto &Entry : retired_)
    for (const std::unique_ptr<EJitSmallTableTableResource> &R : retained_)
      if (R->generation() == Entry.first)
        Bytes += R->allocatedBytes();
  return Bytes;
}

void EJitSmallTableRuntime::acquireReader(uint64_t Generation) {
  ++readers_[Generation];
}

void EJitSmallTableRuntime::releaseReader(uint64_t Generation) {
  auto It = readers_.find(Generation);
  if (It == readers_.end())
    return;
  if (It->second > 0)
    --It->second;
  if (It->second == 0)
    readers_.erase(It);
  // A generation whose retirement waited for exactly this reader can now go.
  freeIfUnread(Generation);
}

void EJitSmallTableRuntime::releaseSessionBorrowIfDrained() {
  if (!sessionOpen_ && sessionInFlight_ == 0 && sessionBorrow_) {
    sessionBorrow_->release();
    sessionBorrow_.reset();
  }
  for (auto It = retiredSessionBorrows_.begin();
       It != retiredSessionBorrows_.end();) {
    const uint64_t Session = It->first;
    const bool StillExecuting = std::any_of(
        samplingExecutions_.begin(), samplingExecutions_.end(),
        [Session](const auto &Execution) {
          return Execution.second.sessionId == Session;
        });
    if (StillExecuting) {
      ++It;
      continue;
    }
    It->second->release();
    It = retiredSessionBorrows_.erase(It);
  }
}

EJitSmallTableAdmission
EJitSmallTableRuntime::admitAndPublish(ArrayRef<uint64_t> Indices,
                                       StringRef BoundReason,
                                       std::string *Why) {
  auto Fail = [&](StringRef Msg) {
    if (Why)
      *Why = Msg.str();
    stats_.aotRefusals++;
    return EJitSmallTableAdmission::Unusable;
  };

  if (!plan_ || !resource_)
    return Fail("no prepared plan");
  if (Indices.size() != contract_.dims.size())
    return Fail("member coordinate does not match the declared schema");

  std::string BorrowError;
  std::unique_ptr<EJitSmallTableReadBorrow> Borrow =
      provider_->borrow(contract_.sourceVarName, BorrowError);
  if (!Borrow)
    return Fail(BorrowError);
  if (Borrow->epoch() != domainEpoch_) {
    Borrow->invalidate("configuration generation moved");
    return Fail("borrow belongs to a different configuration generation");
  }

  std::string ReadError;
  auto Member =
      readAdmissionMember(contract_, Borrow->source(), Indices, ReadError);
  if (!Member)
    return Fail(ReadError);

  std::string WhyAdmission;
  const EJitSmallTableAdmission Class =
      validateAdmission(contract_, *Member, &WhyAdmission);

  if (Class == EJitSmallTableAdmission::Compatible) {
    Borrow->release();
    if (Why)
      *Why = WhyAdmission;
    return Class;
  }
  if (Class != EJitSmallTableAdmission::Extendable) {
    Borrow->release();
    stats_.aotRefusals++;
    if (Why)
      *Why = WhyAdmission;
    return Class;
  }

  // Extendable: the member's projection may add coordinates that are not yet
  // published. Publish them, then re-validate the published set so a conflict
  // discovered here can never be published as if it were agreed.
  for (unsigned F = 0; F < contract_.fields.size(); ++F) {
    const EJitSmallTableFieldContract &FC = contract_.fields[F];
    if (FC.strategy != EJitSmallTableStrategy::Table)
      continue;
    const uint64_t Coord = projectCoordinate(contract_.dims, FC, Indices);
    if (Coord == std::numeric_limits<uint64_t>::max())
      return Fail("member projection leaves the retained-axis schema");
    switch (resource_->publish(F, Coord, Member->bits[F])) {
    case EJitSmallTableTableResource::PublishResult::Stored:
      stats_.publishedRows++;
      break;
    case EJitSmallTableTableResource::PublishResult::AlreadySame:
      break;
    case EJitSmallTableTableResource::PublishResult::Conflict:
      return Fail("published projection conflicts with the member value");
    case EJitSmallTableTableResource::PublishResult::OutOfRange:
      return Fail("member projection leaves the column capacity");
    case EJitSmallTableTableResource::PublishResult::NotATable:
      break;
    }
  }

  // Re-check the generation after the last read and before this member becomes
  // dispatchable (spec §6.1.1/§6.3).
  if (!provider_->epochCurrent(domainEpoch_) || Borrow->stale()) {
    Borrow->invalidate("configuration generation moved during admission");
    return Fail("configuration generation moved during member admission");
  }
  Borrow->release();

  for (unsigned F = 0; F < contract_.fields.size(); ++F)
    if (contract_.fields[F].strategy == EJitSmallTableStrategy::Table)
      contract_.fields[F].publishedValues = resource_->publishedValues(F);

  if (Why)
    *Why = WhyAdmission;
  (void)BoundReason;
  return EJitSmallTableAdmission::Extendable;
}

EJitSmallTableAdmission
EJitSmallTableRuntime::admitMember(ArrayRef<uint64_t> Indices,
                                   std::string *Why) {
  const EJitSmallTableAdmission Class = admitAndPublish(Indices, {}, Why);
  switch (Class) {
  case EJitSmallTableAdmission::Compatible:
    stats_.admittedMembers++;
    stats_.compatibleMembers++;
    break;
  case EJitSmallTableAdmission::Extendable:
    stats_.admittedMembers++;
    stats_.extendableMembers++;
    break;
  case EJitSmallTableAdmission::Conflict:
    stats_.conflictMembers++;
    break;
  case EJitSmallTableAdmission::Unusable:
    stats_.unusableMembers++;
    break;
  }
  std::vector<uint64_t> Key(Indices.begin(), Indices.end());
  // One entry per coordinate: a member re-admitted by a later generation
  // replaces its previous classification instead of leaving a stale one behind
  // (an old Conflict entry must not keep a now-admitted member on the AOT path).
  for (auto &Entry : admitted_)
    if (Entry.first == Key) {
      Entry.second = Class;
      return Class;
    }
  admitted_.push_back({std::move(Key), Class});
  return Class;
}

Expected<void *> EJitSmallTableRuntime::compileCommonT1(uint64_t CodeGeneration,
                                                        std::string &Error) {
  if (!plan_ || !resource_) {
    Error = "compileCommonT1 before prepare";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  if (!provider_->epochCurrent(domainEpoch_)) {
    noteGenerationChange("configuration generation moved before T1");
    Error = cancellationReason_;
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }

  codeGeneration_ = CodeGeneration;

  // Bind the runtime-owned resource to the column symbols BEFORE the module is
  // loaded, so the compiled code addresses this resource and no other. The
  // address is resolved again after the compile and must match.
  for (const EJitSmallTableTableResource::Column &C : resource_->columns()) {
    void *Addr = resource_->columnAddress(C.fieldIndex);
    if (!Addr) {
      Error = "table resource has no address for column " + C.symbolName;
      return make_error<StringError>(Error, inconvertibleErrorCode());
    }
    engine_->addUserSymbol(C.symbolName, Addr);
  }
  boundResourceAddress_ = reinterpret_cast<uintptr_t>(resource_->base());

  std::unique_ptr<EJitSmallTableReadBorrow> Borrow =
      provider_->borrow(contract_.sourceVarName, Error);
  if (!Borrow)
    return make_error<StringError>(Error, inconvertibleErrorCode());

  // Publish every provider-ready member the contract admits, before anything can
  // dispatch to this code.
  SmallVector<uint64_t, 4> Indices;
  for (const EJitSmallTableReadyMember &Member : provider_->readyMembers()) {
    if (!Member.fieldsInitialized || Member.indices.size() != contract_.dims.size())
      continue;
    Indices.clear();
    Indices.append(Member.indices.begin(), Member.indices.end());
    std::string Why;
    const EJitSmallTableAdmission Class = admitMember(Indices, &Why);
    (void)Class;
    EJIT_DIAG("small-table-runtime: member c=%llu t=%llu admission=%s (%s)",
              (unsigned long long)(Indices.size() > 0 ? Indices[0] : 0),
              (unsigned long long)(Indices.size() > 1 ? Indices[1] : 0),
              admissionName(Class), Why.c_str());
  }
  if (!provider_->epochCurrent(domainEpoch_) || Borrow->stale()) {
    Borrow->invalidate("configuration generation moved during row publication");
    Error = "configuration generation moved during row publication";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  Borrow->release();

  SpecializationContext Ctx;
  Ctx.fnName = plan_->entryName;
  Ctx.cacheKey = CodeGeneration;
  Ctx.optLevel = options_.optLevel;
  Ctx.tier = CompileTier::Instrumented;
  activeCtx_ = std::make_unique<SpecializationContext>(std::move(Ctx));
  engine_->setActiveContext(activeCtx_.get());

  if (auto Err = engine_->loadBitcodeModule(bitcode_, CodeGeneration,
                                            activeCtx_->fnName)) {
    engine_->setActiveContext(nullptr);
    Error = toString(std::move(Err));
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  auto FnOrErr = engine_->lookup(CodeGeneration, activeCtx_->fnName);
  if (!FnOrErr) {
    engine_->setActiveContext(nullptr);
    Error = toString(FnOrErr.takeError());
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
#if defined(EJIT_SRE_CODE_POOL)
  if (auto Err = engine_->flushPendingCode()) {
    Error = toString(std::move(Err));
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
#endif

  // Real resource identity: the compiled code must address the SAME resource
  // this generation published into. A same-named symbol from another compile is
  // not proof of a shared table (spec §6.6/§10).
  for (const EJitSmallTableTableResource::Column &C : resource_->columns()) {
    auto ColOrErr = engine_->lookup(CodeGeneration, C.symbolName);
    if (!ColOrErr) {
      Error = "column " + C.symbolName + " did not resolve after T1";
      consumeError(ColOrErr.takeError());
      return make_error<StringError>(Error, inconvertibleErrorCode());
    }
    if (reinterpret_cast<uintptr_t>(*ColOrErr) !=
        reinterpret_cast<uintptr_t>(resource_->columnAddress(C.fieldIndex))) {
      Error = "T1 bound a different table resource for column " + C.symbolName;
      return make_error<StringError>(Error, inconvertibleErrorCode());
    }
  }

  // Capture the real Tier-1 counter addresses for the later profile synthesis.
  counterNames_.clear();
  counterAddrs_.clear();
  if (engine_->getLastCounterNames().empty()) {
    engine_->setActiveContext(nullptr);
    Error = "common T1 emitted no profile counters; refusing a profile-free session";
    cancel(Error);
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  if (beforeCounterCaptureForTesting_) {
    // Consume the test callback once, before any session can open. The normal
    // loop below still performs every real ORC lookup and handles its errors.
    auto BeforeCapture = std::move(beforeCounterCaptureForTesting_);
    beforeCounterCaptureForTesting_ = nullptr;
    BeforeCapture(*engine_, CodeGeneration, engine_->getLastCounterNames());
  }
  auto FailCapture = [&](std::string Detail) -> Expected<void *> {
    engine_->setActiveContext(nullptr);
    counterNames_.clear();
    counterAddrs_.clear();
    Error = "incomplete common T1 profile counter capture: " + Detail;
    cancel(Error);
    return make_error<StringError>(Error, inconvertibleErrorCode());
  };
  for (const std::string &Name : engine_->getLastCounterNames()) {
    auto Profc = engine_->lookup(CodeGeneration, "__profc_" + Name);
    auto Profd = engine_->lookup(CodeGeneration, "__profd_" + Name);
    if (Profc && Profd) {
      // ORC's legal symbol suffix is not an internal function's PGO lookup
      // name: e.g. "_string__helper" versus "<string>;helper". Preserve the
      // exact canonical name captured by lowering, and verify it against the
      // real emitted metadata before allowing ANY sampling session to open.
      StringRef ProfileName = engine_->getCounterProfileName(Name);
      if (ProfileName.empty() || !*Profc || !*Profd)
        return FailCapture("missing canonical profile name or counter address "
                           "for symbol suffix " + Name);
      const auto *Data =
          reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(*Profd);
      const uint64_t NameHash = IndexedInstrProf::ComputeHash(ProfileName);
      if (Data->NameRef != NameHash)
        return FailCapture("canonical profile name hash does not match __profd_" +
                           Name + " NameRef for " + ProfileName.str());
      if (llvm::any_of(counterNames_, [&](const std::string &Existing) {
            return StringRef(Existing) == ProfileName;
          }))
        return FailCapture("duplicate canonical profile name " +
                           ProfileName.str());
      counterNames_.push_back(ProfileName.str());
      counterAddrs_.push_back(
          {reinterpret_cast<uintptr_t>(*Profc),
           reinterpret_cast<uintptr_t>(*Profd)});
    } else {
      std::string Missing;
      if (!Profc)
        Missing = "__profc_" + Name + ": " + toString(Profc.takeError());
      if (!Profd)
        Missing += (Missing.empty() ? std::string() : "; ") +
                   "__profd_" + Name + ": " + toString(Profd.takeError());
      return FailCapture(std::move(Missing));
    }
  }
  engine_->setActiveContext(nullptr);

  // ONE session per entry/code generation (spec §10): opening the session is not
  // a sample, and no sample is counted until a real admitted entry happens. The
  // aggregate budget restarts here because a new code generation is a new
  // sampling session; a late member admitted into the SAME generation does not
  // restart it.
  sessionId_++;
  sessionOpen_ = true;
  frozen_ = false;
  bundle_.reset();
  sessionSamples_ = 0;
  sessionInFlight_ = 0;
  sampleKeys_.clear();
  pendingGeneration_ = false;
  return *FnOrErr;
}

bool EJitSmallTableRuntime::enterAdmitted(ArrayRef<uint64_t> Indices,
                                          EJitSmallTableSampleTicket *Ticket,
                                          std::string *Why) {
  if (Ticket)
    *Ticket = EJitSmallTableSampleTicket{};
  auto Refuse = [&](StringRef Msg) {
    if (Why)
      *Why = Msg.str();
    stats_.aotRefusals++;
    return false;
  };

#ifndef EJIT_FREESTANDING
  // Serialize quota admission with ticket creation and completion. Rejecting
  // above-quota T1 before obtaining a borrow also prevents real profile
  // counters from accumulating unbounded calls after bookkeeping reached64.
  std::lock_guard<std::mutex> Guard(mutex_);
#endif

  if (!sessionOpen_ || frozen_)
    return Refuse(cancellationReason_.empty() ? "no open sampling session"
                                              : cancellationReason_);
  if (!provider_ || !provider_->epochCurrent(domainEpoch_)) {
    noteGenerationChange("configuration generation moved before dispatch");
    return Refuse(cancellationReason_);
  }

  const EJitSmallTableAdmission *Class = nullptr;
  for (const auto &Entry : admitted_)
    if (Entry.first.size() == Indices.size() &&
        std::equal(Entry.first.begin(), Entry.first.end(), Indices.begin())) {
      Class = &Entry.second;
      break;
    }
  if (!Class)
    return Refuse("member was never admitted: it must stay AOT");
  if (*Class != EJitSmallTableAdmission::Compatible &&
      *Class != EJitSmallTableAdmission::Extendable)
    return Refuse("member admission is not compatible with the specialized code");

  if (samplingExhausted())
    return Refuse("aggregate sampling budget reached; use AOT until T2 publishes");

  if (!Ticket)
    return Refuse("a real execution requires a completion ticket");

  // The sampling window itself runs under the protected read borrow: the
  // instrumented entry may still read the source region for fields the plan did
  // not specialize, and a sample must never be taken outside the configuration
  // generation the plan was proven against (spec §6.1.1/§10). The borrow is
  // taken once per session and held until freeze/cancel, not per sample.
  if (!sessionBorrow_) {
    std::string BorrowError;
    sessionBorrow_ = provider_->borrow(contract_.sourceVarName, BorrowError);
    if (!sessionBorrow_)
      return Refuse("no protected read borrow for the sampling session: " +
                    BorrowError);
    if (!provider_->epochCurrent(domainEpoch_) || sessionBorrow_->stale()) {
      sessionBorrow_->invalidate("configuration generation moved at dispatch");
      sessionBorrow_.reset();
      noteGenerationChange("configuration generation moved at dispatch");
      return Refuse(cancellationReason_);
    }
  }

  if (nextTicketSerial_ == std::numeric_limits<uint64_t>::max())
    return Refuse("sampling execution ticket space exhausted");
  const uint64_t Serial = nextTicketSerial_++;
  // Every actual admitted T1 remains protected until its real leave, including
  // the last quota member while freeze or cancellation arrives.
  stats_.acceptedSamples++;
  sessionSamples_++;
  sampleKeys_.push_back(std::vector<uint64_t>(Indices.begin(), Indices.end()));
  inFlight_++;
  sessionInFlight_++;
  const uint64_t Generation = resourceGeneration();
  samplingExecutions_.emplace(Serial, SamplingExecution{sessionId_, Generation});
  acquireReader(Generation);
  Ticket->sessionId = sessionId_;
  Ticket->serial = Serial;
  Ticket->valid = true;
  Ticket->counted = true;
  if (Why)
    Why->clear();
  return true;
}

void EJitSmallTableRuntime::leaveAdmitted(
    const EJitSmallTableSampleTicket &Ticket) {
#ifndef EJIT_FREESTANDING
  std::lock_guard<std::mutex> Guard(mutex_);
#endif
  auto It = samplingExecutions_.find(Ticket.serial);
  if (!Ticket.valid || It == samplingExecutions_.end() ||
      It->second.sessionId != Ticket.sessionId) {
    stats_.staleCallbacks++;
    return;
  }
  const SamplingExecution Execution = It->second;
  samplingExecutions_.erase(It);
  // `inFlight_` is the PHYSICAL count of executions that were entered and have
  // not returned. A cancellation stops new samples from being counted but never
  // zeroes this: while it is non-zero a real call is still inside the table and
  // inside the sampling window's protected read, so both are retained.
  if (inFlight_ > 0)
    --inFlight_;
  if (Execution.sessionId == sessionId_ && sessionInFlight_ > 0)
    --sessionInFlight_;
  if (Execution.sessionId != sessionId_)
    stats_.staleCallbacks++;
  releaseReader(Execution.resourceGeneration);
  // The window's protected read is released only once no real execution is
  // still reading the source region it guarded (§6.1.1: cancel/failure releases
  // the borrow, and the release cannot precede the last real reader).
  releaseSessionBorrowIfDrained();
#ifndef EJIT_FREESTANDING
  inFlightCV_.notify_all();
#endif
}

Expected<const EJitSmallTableProfileBundle *>
EJitSmallTableRuntime::freeze(std::string &Error, bool Force) {
  if (bundle_)
    return bundle_.get();
  if (!sessionOpen_) {
    Error = cancellationReason_.empty() ? "no open sampling session"
                                        : cancellationReason_;
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  if (!Force && !samplingExhausted()) {
    Error = "aggregate sampling budget not reached (" +
            std::to_string(sessionSamples_) + " of " +
            std::to_string(options_.sampling.aggregateLimit) + ")";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }

  // A granted dispatch is not a completed sample: wait for every admitted
  // execution that is still in flight before reading the counters (spec §10).
#ifdef EJIT_FREESTANDING
  if (sessionInFlight_ != 0) {
    Error = "admitted sampling executions are still in flight and this build "
            "has no blocking wait; refusing to freeze an incomplete sample";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
#else
  {
    std::unique_lock<std::mutex> Lock(mutex_);
    bool Drained = sessionInFlight_ == 0;
    if (!Drained && options_.sampling.waitForInFlightOnFreeze) {
      const auto Deadline =
          options_.sampling.freezeWaitMillis == 0
              ? std::chrono::milliseconds(30000)
              : std::chrono::milliseconds(options_.sampling.freezeWaitMillis);
      Drained = inFlightCV_.wait_for(
          Lock, Deadline, [this] { return sessionInFlight_ == 0; });
    }
    if (!Drained) {
      Error = options_.sampling.waitForInFlightOnFreeze
                  ? "admitted executions are still in flight after the freeze "
                    "wait"
                  : "admitted executions are still in flight; refusing to "
                    "freeze a session whose real executions have not returned";
      return make_error<StringError>(Error, inconvertibleErrorCode());
    }
  }
#endif

  if (!provider_ || !provider_->epochCurrent(domainEpoch_)) {
    Error = "configuration generation moved before profile freeze";
    cancel(Error);
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }

  std::vector<PgoCounterRef> Refs;
  Refs.reserve(counterNames_.size());
  for (size_t I = 0; I < counterNames_.size(); ++I)
    Refs.push_back({counterNames_[I].c_str(), counterAddrs_[I].first,
                    counterAddrs_[I].second});

  std::string Profile = synthesizeProfileBuffer(Refs);
  if (Profile.empty()) {
    Error = "profile synthesis produced no bundle: refusing to compile T2 "
            "without a real profile";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }

  auto B = std::make_unique<EJitSmallTableProfileBundle>();
  B->entryName = plan_->entryName;
  B->codeGeneration = codeGeneration_;
  B->domainEpoch = domainEpoch_;
  B->sessionId = sessionId_;
  B->contractHash = contract_.identityHash;
  B->resourceAddress = boundResourceAddress_;
  B->resourceGeneration = resource_->generation();
  B->sampleCount = sessionSamples_;
  B->readinessProvider = providerLabel_;
  B->counters = std::move(Refs);
  B->profileData = std::move(Profile);
  {
    std::vector<std::vector<uint64_t>> Unique = sampleKeys_;
    llvm::sort(Unique);
    Unique.erase(std::unique(Unique.begin(), Unique.end()), Unique.end());
    B->participatingMembers = Unique.size();
  }
  bundle_ = std::move(B);
  sessionOpen_ = false;
  frozen_ = true;
  // The sampling window is over: the bundle is immutable and the protected read
  // borrow that guarded the window is released (a failed freeze above leaves the
  // session running and keeps it). Freezing required the session's own
  // `sessionInFlight_ == 0`. `inFlight_` may still be non-zero for executions a
  // cancel gave up on: those are not this (or any) session's samples, and the
  // borrow is retained for them until their own real return.
  releaseSessionBorrowIfDrained();
  return bundle_.get();
}

Expected<void *> EJitSmallTableRuntime::compileCommonT2(std::string &Error) {
  if (!bundle_) {
    Error = "compileCommonT2 without a frozen bundle";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  if (bundle_->resourceAddress != boundResourceAddress_) {
    Error = "frozen bundle belongs to a different table resource";
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }

  SpecializationContext Ctx;
  Ctx.fnName = plan_->entryName;
  Ctx.cacheKey = codeGeneration_;
  Ctx.optLevel = options_.optLevel;
  Ctx.tier = CompileTier::PGOUse;
  Ctx.profileData = bundle_->profileData;
  activeCtx_ = std::make_unique<SpecializationContext>(std::move(Ctx));
  engine_->setActiveContext(activeCtx_.get());

  if (auto Err = engine_->loadBitcodeModule(bitcode_, codeGeneration_,
                                            activeCtx_->fnName)) {
    engine_->setActiveContext(nullptr);
    Error = toString(std::move(Err));
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
  auto FnOrErr = engine_->lookup(codeGeneration_, activeCtx_->fnName);
  if (!FnOrErr) {
    engine_->setActiveContext(nullptr);
    Error = toString(FnOrErr.takeError());
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
#if defined(EJIT_SRE_CODE_POOL)
  if (auto Err = engine_->flushPendingCode()) {
    Error = toString(std::move(Err));
    return make_error<StringError>(Error, inconvertibleErrorCode());
  }
#endif

  // T2 must read the SAME shared resource as T1: this is the check that a
  // same-named symbol from a separate compilation is not silently accepted as
  // the same table (spec §8/§10).
  for (const EJitSmallTableTableResource::Column &C : resource_->columns()) {
    auto ColOrErr = engine_->lookup(codeGeneration_, C.symbolName);
    if (!ColOrErr) {
      Error = "column " + C.symbolName + " did not resolve after T2";
      consumeError(ColOrErr.takeError());
      return make_error<StringError>(Error, inconvertibleErrorCode());
    }
    if (reinterpret_cast<uintptr_t>(*ColOrErr) !=
        reinterpret_cast<uintptr_t>(resource_->columnAddress(C.fieldIndex))) {
      Error = "T2 bound a different table resource for column " + C.symbolName;
      return make_error<StringError>(Error, inconvertibleErrorCode());
    }
  }
  engine_->setActiveContext(nullptr);
  return *FnOrErr;
}

void EJitSmallTableRuntime::cancel(StringRef Reason) {
  cancellationReason_ = Reason.empty() ? std::string("cancelled") : Reason.str();
  // A cancelled session is no longer the current one: a ticket it granted is a
  // stale callback, never a completion of the session that follows. The ticket
  // serial moves so a completion that arrives later can be RECOGNIZED as stale
  // while still being accounted against its OWN (old) session.
  // Park this session's protected read independently of a replacement session.
  // A new session must obtain its own borrow even while old calls still run.
  if (sessionBorrow_ && sessionInFlight_ != 0)
    retiredSessionBorrows_.emplace(sessionId_, std::move(sessionBorrow_));
  if (sessionOpen_)
    sessionId_++;
  sessionOpen_ = false;
  // The cancelled session gave up its samples: they can never be merged, so the
  // session's own barrier drops. The PHYSICAL count does not: the calls are
  // still running and their generations stay retained.
  sessionInFlight_ = 0;
  // Logical cancellation is not physical completion: `inFlight_` is the real
  // count of entered-and-not-returned executions and is decremented by their
  // own `leaveAdmitted`. An execution that was cancelled mid-flight therefore
  // keeps its generation retained, and the sampling window's protected read is
  // released only when the last of them returns.
  releaseSessionBorrowIfDrained();
#ifndef EJIT_FREESTANDING
  inFlightCV_.notify_all();
#endif
}

void EJitSmallTableRuntime::noteGenerationChange(StringRef Reason) {
  stats_.generationChanges++;
  cancel(Reason.empty() ? "configuration generation changed" : Reason);
}

} // namespace ejit
} // namespace llvm
