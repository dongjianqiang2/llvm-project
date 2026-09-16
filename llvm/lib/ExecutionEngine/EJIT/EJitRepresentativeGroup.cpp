//===-- EJitRepresentativeGroup.cpp - representative-PGO group lifecycle ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRepresentativeGroup.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <utility>

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// One logical member record: independent for every member of the group, no
/// matter how many members physically share one Tier-2 object.
struct MemberRecord {
  uint64_t logicalKey = 0;
  uint32_t funcIndex = 0;
  uint64_t token = 0;
  bool waiter = false;
  bool cancelled = false;
  bool settled = false;
  bool shared = false;
  uint64_t codeId = 0;
  void *fn = nullptr;
};

struct GroupState {
  uint64_t groupId = 0;
  uint64_t generation = 1;
  EJitGroupAdmissionPolicy policy;
  EJitRepresentativeSession rep;
  EJitFrozenProfileBundle bundle;
  uint64_t bundleAttemptToken = 0;
  /// The generation's physical Tier-2 object, established by the first member
  /// whose final identity passed the exact compare; later members reuse it only
  /// when their own identity compares equal.
  bool hasPhysical = false;
  uint64_t physicalCodeId = 0;
  void *physicalFn = nullptr;
  std::vector<uint64_t> retiredAttempts;
  std::vector<MemberRecord> members;
};

uint64_t retainedBytesOf(const EJitProfileBundle &B) {
  uint64_t Bytes = B.indexedProfile.size();
  Bytes += B.scalarSites.size() * sizeof(PgoScalarSite);
  Bytes += B.verifiedTargets.size() * sizeof(EJitProfileBundle::VerifiedTarget);
  for (const PgoFunctionSchema &S : B.schema)
    Bytes += sizeof(PgoFunctionSchema) + S.pgoName.size();
  return Bytes;
}

} // namespace

struct EJitRepresentativeGroupRegistry::Impl {
  explicit Impl(Limits L) : limits(L) {}

  Limits limits;
  std::vector<GroupState> groups;
  uint64_t nextAttemptToken = 1;
  uint64_t nextSamplingSessionId = 1;
  uint64_t nextWaiterToken = 1;
  EJitGroupDiagnostics diag;

  GroupState *find(uint64_t groupId, uint64_t generation) {
    for (GroupState &G : groups)
      if (G.groupId == groupId && G.generation == generation)
        return &G;
    return nullptr;
  }
  const GroupState *find(uint64_t groupId, uint64_t generation) const {
    for (const GroupState &G : groups)
      if (G.groupId == groupId && G.generation == generation)
        return &G;
    return nullptr;
  }
  GroupState *findAny(uint64_t groupId) {
    for (GroupState &G : groups)
      if (G.groupId == groupId)
        return &G;
    return nullptr;
  }
  MemberRecord *findMember(GroupState &G, uint64_t token) {
    for (MemberRecord &M : G.members)
      if (M.token == token)
        return &M;
    return nullptr;
  }
  bool isRetired(const GroupState &G, uint64_t attemptToken) const {
    return std::find(G.retiredAttempts.begin(), G.retiredAttempts.end(),
                     attemptToken) != G.retiredAttempts.end();
  }
};

EJitRepresentativeGroupRegistry::EJitRepresentativeGroupRegistry()
    : P(std::make_unique<Impl>(Limits{})) {}

EJitRepresentativeGroupRegistry::EJitRepresentativeGroupRegistry(Limits L)
    : P(std::make_unique<Impl>(L)) {}

EJitRepresentativeGroupRegistry::~EJitRepresentativeGroupRegistry() = default;

EJitGroupAdmitReject
EJitRepresentativeGroupRegistry::admissionReject(const EJitGroupAdmissionPolicy &Policy) {
  if (!Policy.pgoEnabled)
    return EJitGroupAdmitReject::PgoDisabled;
  if (!Policy.asyncService)
    return EJitGroupAdmitReject::NotAsync;
  if (!Policy.normalOnlinePgo)
    return EJitGroupAdmitReject::AuditOnly;
  if (Policy.modeChangeInFlight)
    return EJitGroupAdmitReject::ModeChangeInFlight;
  if (Policy.dispatchQuota == 0)
    return EJitGroupAdmitReject::ZeroQuota;
  return EJitGroupAdmitReject::None;
}

Expected<EJitGroupHandle>
EJitRepresentativeGroupRegistry::openGroup(uint64_t CandidateGroupId,
                                           const EJitGroupAdmissionPolicy &Policy) {
  // The gate runs before any side effect: a rejected policy must leave the
  // group table untouched (no group, no generation, no admission record).
  if (EJitGroupAdmitReject R = admissionReject(Policy); R != EJitGroupAdmitReject::None) {
    ++P->diag.rejectedAdmissions;
    return make_error<StringError>(
        "representative sharing is not admissible for this policy",
        std::make_error_code(std::errc::operation_not_permitted));
  }
  if (CandidateGroupId == 0)
    return make_error<StringError>(
        "candidate group id 0 is reserved",
        std::make_error_code(std::errc::invalid_argument));
  if (GroupState *Existing = P->findAny(CandidateGroupId)) {
    // Refresh the policy of the live generation without touching its state.
    Existing->policy = Policy;
    return EJitGroupHandle{Existing->groupId, Existing->generation};
  }
  if (P->groups.size() >= P->limits.maxGroups) {
    ++P->diag.rejectedAdmissions;
    return make_error<StringError>(
        "representative group table is full",
        std::make_error_code(std::errc::no_buffer_space));
  }
  GroupState G;
  G.groupId = CandidateGroupId;
  G.generation = 1;
  G.policy = Policy;
  P->groups.push_back(std::move(G));
  ++P->diag.admittedGroups;
  return EJitGroupHandle{CandidateGroupId, 1};
}

Expected<EJitRepresentativeSession>
EJitRepresentativeGroupRegistry::electRepresentative(
    const EJitGroupHandle &Handle, ArrayRef<EJitGroupMember> Members,
    uint64_t NowUsec) {
  GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G)
    return make_error<StringError>(
        "representative election for an unknown or retired generation",
        std::make_error_code(std::errc::invalid_argument));
  // One representative per group generation: a second election returns the
  // live session unchanged instead of starting a competing sampling window.
  if (G->rep.valid())
    return G->rep;

  const EJitGroupMember *Legal = nullptr;
  for (const EJitGroupMember &M : Members) {
    // funcIndex 0 is a VALID dense index (the invalid sentinel is
    // kEJitInvalidFuncIndex): testing it against 0 would make the very first
    // registered entry permanently ineligible to represent its own group.
    if (M.funcIndex == kEJitInvalidFuncIndex)
      continue;
    if (!M.cellActive)
      continue; // an inactive cell cannot be the representative
    if (M.cold)
      continue; // a cold cell has no live sampling window to measure
    Legal = &M;
    break; // arrival order, NOT a hardcoded cell 0
  }
  if (!Legal)
    return make_error<StringError>(
        "no legal active member to act as the group representative",
        std::make_error_code(std::errc::invalid_argument));

  EJitRepresentativeSession S;
  S.groupId = G->groupId;
  S.generation = G->generation;
  S.attemptToken = P->nextAttemptToken++;
  S.samplingSessionId = P->nextSamplingSessionId++;
  S.logicalKey = Legal->logicalKey;
  S.funcIndex = Legal->funcIndex;
  S.dispatchLimit = G->policy.dispatchQuota;
  S.dispatchCount = 0;
  S.quotaEnd = 0;
  S.ownsQuota = true;
  G->rep = S;
  (void)NowUsec; // the boundary is frozen by the dispatch that reaches the limit
  ++P->diag.representativeSessions;
  return S;
}

const EJitRepresentativeSession *
EJitRepresentativeGroupRegistry::currentRepresentative(
    const EJitGroupHandle &Handle) const {
  const GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G || !G->rep.valid())
    return nullptr;
  return &G->rep;
}

Expected<EJitWaiterToken>
EJitRepresentativeGroupRegistry::joinWaiter(const EJitGroupHandle &Handle,
                                            const EJitGroupMember &M) {
  GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G)
    return make_error<StringError>(
        "waiter joined an unknown or retired generation",
        std::make_error_code(std::errc::invalid_argument));
  if (M.funcIndex == kEJitInvalidFuncIndex || !M.cellActive)
    return make_error<StringError>(
        "waiter must be an active cell of the group",
        std::make_error_code(std::errc::invalid_argument));
  if (G->members.size() >= P->limits.maxMembersPerGroup)
    return make_error<StringError>(
        "group member table is full",
        std::make_error_code(std::errc::no_buffer_space));

  MemberRecord R;
  R.logicalKey = M.logicalKey;
  R.funcIndex = M.funcIndex;
  R.token = P->nextWaiterToken++;
  R.waiter = true;
  G->members.push_back(R);
  ++P->diag.waitersJoined;
  return EJitWaiterToken{G->groupId, G->generation, R.token, R.logicalKey};
}

EJitDispatchOutcome EJitRepresentativeGroupRegistry::recordRepresentativeDispatch(
    const EJitGroupHandle &Handle, const EJitRepresentativeSession &S,
    uint64_t NowUsec) {
  ++P->diag.logicalRequests;
  GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G) {
    ++P->diag.staleSettlements;
    return EJitDispatchOutcome::Stale;
  }
  // Exact ownership: the session must be this generation's current session.
  if (!G->rep.valid() || G->rep.attemptToken != S.attemptToken ||
      G->rep.samplingSessionId != S.samplingSessionId) {
    if (P->isRetired(*G, S.attemptToken) || S.attemptToken != G->rep.attemptToken) {
      ++P->diag.staleSettlements;
      return EJitDispatchOutcome::Stale;
    }
    return EJitDispatchOutcome::OutsideSession;
  }
  if (G->rep.dispatchCount >= G->rep.dispatchLimit)
    return EJitDispatchOutcome::OutsideSession; // long closed: no late count

  ++G->rep.dispatchCount;
  ++P->diag.representativeDispatches;
  // The final allowed dispatch freezes the boundary exactly once; an
  // unconfigured clock (0) stays an honest "unknown", never a compile time.
  if (G->rep.dispatchCount == G->rep.dispatchLimit) {
    G->rep.quotaEnd = NowUsec;
    return EJitDispatchOutcome::CountedAndClosed;
  }
  return EJitDispatchOutcome::Counted;
}

EJitPublishOutcome EJitRepresentativeGroupRegistry::publishBundle(
    const EJitGroupHandle &Handle, const EJitRepresentativeSession &S,
    EJitProfileBundle Bundle) {
  GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G) {
    ++P->diag.staleSettlements;
    return EJitPublishOutcome::Stale;
  }
  if (G->bundleAttemptToken != 0 && G->bundleAttemptToken == S.attemptToken)
    return EJitPublishOutcome::AlreadyPublished; // duplicate late callback
  if (!G->rep.valid() || G->rep.attemptToken != S.attemptToken ||
      G->rep.samplingSessionId != S.samplingSessionId) {
    ++P->diag.staleSettlements;
    return EJitPublishOutcome::Stale;
  }
  // A shared Tier-2 must consume a real representative profile. An empty edge
  // profile, an empty schema, an unavailable observation or a session that
  // never really dispatched are all rejected instead of fabricating a shared
  // path from a synthetic/partial bundle.
  if (!Bundle.hasEdgeProfile || Bundle.indexedProfile.empty() ||
      Bundle.schema.empty() ||
      Bundle.dispatchQuality == T1DispatchObservationQuality::Unavailable ||
      G->rep.dispatchCount == 0) {
    return EJitPublishOutcome::InvalidBundle;
  }

  // The registry owns the session identity fields: a caller cannot mislabel a
  // bundle with another attempt's or generation's numbers.
  Bundle.groupId = G->groupId;
  Bundle.groupGeneration = G->generation;
  Bundle.samplingSessionId = G->rep.samplingSessionId;
  Bundle.representativeLogicalKey = G->rep.logicalKey;
  Bundle.representativeAttemptToken = G->rep.attemptToken;
  Bundle.actualDispatchCount = G->rep.dispatchCount;
  Bundle.dispatchLimit = G->rep.dispatchLimit;
  Bundle.quotaEnd = G->rep.quotaEnd;
  if (Bundle.freezeCompletedAt == 0)
    Bundle.freezeCompletedAt = G->rep.quotaEnd;

  P->diag.retainedBundleBytes += retainedBytesOf(Bundle);
  switch (Bundle.quality) {
  case ProfileSnapshotQuality::Complete:
    ++P->diag.completeProfiles;
    break;
  case ProfileSnapshotQuality::ApproximateInFlight:
    ++P->diag.approximateProfiles;
    break;
  case ProfileSnapshotQuality::EdgeOnly:
    ++P->diag.edgeOnlyProfiles;
    break;
  case ProfileSnapshotQuality::ValueDataDropped:
    ++P->diag.valueDroppedProfiles;
    break;
  }
  ++P->diag.bundlePublications;
  G->bundleAttemptToken = G->rep.attemptToken;
  G->bundle = std::make_shared<const EJitProfileBundle>(std::move(Bundle));
  return EJitPublishOutcome::Published;
}

EJitFrozenProfileBundle
EJitRepresentativeGroupRegistry::bundleFor(const EJitGroupHandle &Handle) const {
  const GroupState *G = P->find(Handle.groupId, Handle.generation);
  return G ? G->bundle : nullptr;
}

bool EJitRepresentativeGroupRegistry::schemaCompatible(
    const EJitGroupHandle &Handle, ArrayRef<PgoFunctionSchema> MemberSchema,
    std::string *Reason) const {
  const GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G || !G->bundle) {
    if (Reason)
      *Reason = "no frozen bundle for this generation";
    return false;
  }
  if (MemberSchema.size() != G->bundle->schema.size()) {
    if (Reason)
      *Reason = "schema entry count differs from the frozen bundle";
    return false;
  }
  for (const PgoFunctionSchema &Want : MemberSchema) {
    const PgoFunctionSchema *Have = nullptr;
    for (const PgoFunctionSchema &S : G->bundle->schema)
      if (S.pgoName == Want.pgoName) {
        Have = &S;
        break;
      }
    if (!Have) {
      if (Reason)
        *Reason = "schema entry '" + Want.pgoName + "' is absent from the bundle";
      return false;
    }
    if (Have->funcHash != Want.funcHash ||
        Have->pgoNameHash != Want.pgoNameHash ||
        Have->numCounters != Want.numCounters ||
        Have->numIcSites != Want.numIcSites ||
        Have->numMemSites != Want.numMemSites ||
        Have->numScalarSites != Want.numScalarSites) {
      if (Reason)
        *Reason = "schema mismatch for '" + Want.pgoName +
                  "' (hashes or site counts differ)";
      return false;
    }
  }
  return true;
}

EJitShareDecision EJitRepresentativeGroupRegistry::decideMember(
    const EJitWaiterToken &W, const EJitFinalCodeIdentity &Identity,
    const EJitPreparedCodeEmitter &Emitter) const {
  EJitShareDecision D;
  const GroupState *G = P->find(W.groupId, W.generation);
  if (!G) {
    D.kind = EJitMemberShare::Stale;
    D.reason = "waiter generation was retired";
    return D;
  }
  const MemberRecord *M = nullptr;
  for (const MemberRecord &R : G->members)
    if (R.token == W.token) {
      M = &R;
      break;
    }
  if (!M) {
    D.kind = EJitMemberShare::Stale;
    D.reason = "unknown waiter token";
    return D;
  }
  if (M->cancelled) {
    D.kind = EJitMemberShare::Cancelled;
    D.reason = "waiter was cancelled";
    return D;
  }
  if (M->settled) {
    D.kind = EJitMemberShare::Stale;
    D.reason = "waiter already settled (exactly once)";
    return D;
  }
  // The publication barrier: a non-representative must stay on AOT until the
  // generation's shared profile is frozen.
  if (!G->bundle) {
    D.kind = EJitMemberShare::NotReady;
    D.reason = "shared representative profile is not published yet";
    return D;
  }
  if (!G->hasPhysical) {
    D.kind = EJitMemberShare::Emit;
    D.reason = "first validated identity of this generation";
    return D;
  }
  // Exact compare against the generation's physical object: full IR plus
  // effective bindings plus scope plus digest, never a digest-only match.
  if (Emitter.linkedIdentityEquals(G->physicalCodeId, Identity)) {
    D.kind = EJitMemberShare::Reuse;
    D.codeId = G->physicalCodeId;
    D.fn = G->physicalFn;
    D.reason = "final identity equals the generation's physical object";
    return D;
  }
  D.kind = EJitMemberShare::Emit;
  D.reason = "final identity differs: independent physical code";
  return D;
}

bool EJitRepresentativeGroupRegistry::noteRepresentativeCode(
    const EJitGroupHandle &Handle, const EJitRepresentativeSession &S,
    uint64_t CodeId, void *Fn, bool EmittedNew) {
  GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G || !G->rep.valid() || G->rep.attemptToken != S.attemptToken)
    return false;
  if (G->hasPhysical)
    return false;
  G->hasPhysical = true;
  G->physicalCodeId = CodeId;
  G->physicalFn = Fn;
  if (EmittedNew)
    ++P->diag.physicalCodeObjects;
  else
    ++P->diag.sharedPhysicalReuses;
  return true;
}

bool EJitRepresentativeGroupRegistry::completeMember(const EJitWaiterToken &W,
                                                     uint64_t CodeId, void *Fn,
                                                     bool EmittedNew) {
  GroupState *G = P->find(W.groupId, W.generation);
  if (!G)
    return false; // retired generation: settle nothing
  MemberRecord *M = P->findMember(*G, W.token);
  if (!M || M->cancelled || M->settled)
    return false; // exactly once per exact token
  M->settled = true;
  M->codeId = CodeId;
  M->fn = Fn;
  M->shared = !EmittedNew;
  ++P->diag.waitersCompleted;
  if (EmittedNew) {
    if (!G->hasPhysical) {
      G->hasPhysical = true;
      G->physicalCodeId = CodeId;
      G->physicalFn = Fn;
      ++P->diag.physicalCodeObjects;
    } else {
      // A distinct final identity produced its own physical object; the group
      // keeps both, it never merges them by digest.
      ++P->diag.physicalCodeObjects;
      ++P->diag.independentPhysicalObjects;
    }
  } else {
    ++P->diag.sharedPhysicalReuses;
  }
  return true;
}

Expected<EJitGroupHandle> EJitRepresentativeGroupRegistry::invalidateRepresentative(
    const EJitGroupHandle &Handle, const EJitRepresentativeSession &S,
    StringRef Reason) {
  (void)Reason;
  GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G)
    return make_error<StringError>(
        "cannot invalidate an unknown or retired generation",
        std::make_error_code(std::errc::invalid_argument));
  if (!G->rep.valid() || G->rep.attemptToken != S.attemptToken)
    return make_error<StringError>(
        "cannot invalidate a session that is not the current representative",
        std::make_error_code(std::errc::invalid_argument));
  // Retire the exact session: its late callbacks can only settle as Stale, and
  // every waiter of the retired generation must re-join the new one.
  G->retiredAttempts.push_back(G->rep.attemptToken);
  G->rep = EJitRepresentativeSession();
  G->bundle.reset();
  G->bundleAttemptToken = 0;
  G->hasPhysical = false;
  G->physicalCodeId = 0;
  G->physicalFn = nullptr;
  // Every waiter belongs to the retired generation. Mark it cancelled before
  // incrementing the handle generation so a late waiter callback cannot settle
  // against the replacement session or keep its source borrow alive.
  for (MemberRecord &M : G->members) {
    if (M.waiter && !M.settled && !M.cancelled) {
      M.cancelled = true;
      ++P->diag.waitersCancelled;
    }
  }
  ++G->generation;
  ++P->diag.representativeReElections;
  return EJitGroupHandle{G->groupId, G->generation};
}

bool EJitRepresentativeGroupRegistry::cancelWaiter(const EJitWaiterToken &W) {
  GroupState *G = P->find(W.groupId, W.generation);
  if (!G)
    return false;
  MemberRecord *M = P->findMember(*G, W.token);
  if (!M || M->settled || M->cancelled)
    return false; // unknown, already settled, or already cancelled: exactly once
  M->cancelled = true;
  ++P->diag.waitersCancelled;
  return true;
}

bool EJitRepresentativeGroupRegistry::cancelRepresentative(
    const EJitGroupHandle &Handle, const EJitRepresentativeSession &S) {
  GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G || !G->rep.valid() || G->rep.attemptToken != S.attemptToken)
    return false;
  G->retiredAttempts.push_back(G->rep.attemptToken);
  G->rep = EJitRepresentativeSession();
  G->bundle.reset();
  G->bundleAttemptToken = 0;
  G->hasPhysical = false;
  G->physicalCodeId = 0;
  G->physicalFn = nullptr;
  for (MemberRecord &M : G->members) {
    if (M.waiter && !M.settled && !M.cancelled) {
      M.cancelled = true;
      ++P->diag.waitersCancelled;
    }
  }
  ++G->generation;
  ++P->diag.representativeReElections;
  return true;
}

size_t EJitRepresentativeGroupRegistry::groupCount() const {
  return P->groups.size();
}

size_t
EJitRepresentativeGroupRegistry::waiterCount(const EJitGroupHandle &Handle) const {
  const GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G)
    return 0;
  size_t N = 0;
  for (const MemberRecord &M : G->members)
    if (M.waiter && !M.cancelled && !M.settled)
      ++N;
  return N;
}

bool EJitRepresentativeGroupRegistry::memberSettled(const EJitWaiterToken &W) const {
  const GroupState *G = P->find(W.groupId, W.generation);
  if (!G)
    return false;
  for (const MemberRecord &M : G->members)
    if (M.token == W.token)
      return M.settled;
  return false;
}

EJitGroupDiagnostics EJitRepresentativeGroupRegistry::diagnostics() const {
  return P->diag;
}

EJitGroupSnapshot
EJitRepresentativeGroupRegistry::snapshot(const EJitGroupHandle &Handle) const {
  EJitGroupSnapshot S;
  const GroupState *G = P->find(Handle.groupId, Handle.generation);
  if (!G)
    return S;
  S.valid = true;
  S.groupId = G->groupId;
  S.generation = G->generation;
  if (G->rep.valid()) {
    S.hasRepresentative = true;
    S.representative = G->rep;
  }
  if (G->bundle) {
    S.hasBundle = true;
    S.bundleGeneration = G->bundle->groupGeneration;
    S.bundleDispatchCount = G->bundle->actualDispatchCount;
    S.bundleDispatchLimit = G->bundle->dispatchLimit;
    S.bundleQuotaEnd = G->bundle->quotaEnd;
  }
  S.hasPhysical = G->hasPhysical;
  S.physicalCodeId = G->physicalCodeId;
  S.physicalFn = G->physicalFn;
  for (const MemberRecord &M : G->members) {
    ++S.members;
    if (M.waiter && !M.cancelled && !M.settled)
      ++S.waiters;
    if (M.settled)
      ++S.settledMembers;
  }
  return S;
}

EJitGroupSnapshot EJitRepresentativeGroupRegistry::firstGroupSnapshot() const {
  return snapshotAt(0);
}

EJitGroupSnapshot EJitRepresentativeGroupRegistry::snapshotAt(size_t Index) const {
  if (Index >= P->groups.size()) return {};
  return snapshot({P->groups[Index].groupId, P->groups[Index].generation});
}
