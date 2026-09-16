//===-- EJitRepresentativeGroup.h - representative-PGO group scheduling ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Representative-PGO group lifecycle for the version code-reuse milestone.
//
// One candidate group (exact classifier, EJitCandidateDirectory) owns ONE
// representative Tier-1 sampling session, ONE immutable EJitProfileBundle per
// generation, and ONE final physical Tier-2 code object per validated final-IR
// identity. Non-representative members stay on AOT until that bundle is
// published; they never run a private Tier-1 and never consume their own
// sampling admission.
//
// This component is deliberately engine-independent: it owns the group
// lifecycle, the quota, the waiter tokens, the publication barrier and the
// per-member logical records, and reuses the production
// EJitFinalCodeIdentity/EJitPreparedCodeEmitter compare for the physical-code
// decision. The caller owns the ORC engine, the pool and the compile driver.
//
// V1 sharing supports Async + normal online PGO only. Every rejection is
// decided before any group/queue/admission side effect.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITREPRESENTATIVEGROUP_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITREPRESENTATIVEGROUP_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitPreparedCode.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace ejit {

/// Why a group may not be admitted for V1 sharing. Checked before any side
/// effect; a non-None result must leave the pool, the queue and the group table
/// untouched.
enum class EJitGroupAdmitReject : uint8_t {
  None = 0,
  /// Config::enablePgo is off: no Instrumented Tier-1 session can exist, so a
  /// shared Tier-2 would silently be a non-PGO compile.
  PgoDisabled,
  /// Sync compile path: there is no background sampling session to wait for.
  NotAsync,
  /// Diagnostics-only (audit) profile: it must not publish shared code.
  AuditOnly,
  /// A live compile-mode change is not settled.
  ModeChangeInFlight,
  /// A zero quota could never complete a representative session.
  ZeroQuota,
};

/// V1 representative-sharing policy for one group.
struct EJitGroupAdmissionPolicy {
  bool pgoEnabled = false;
  bool asyncService = false;
  bool normalOnlinePgo = false;
  bool modeChangeInFlight = false;
  /// Real Tier-1 dispatches the ONE representative session may consume. The
  /// group quota is consumed by the representative only.
  uint64_t dispatchQuota = 64;
  /// Shared-state generation observed at admission time.
  uint32_t poolGeneration = 0;
};

/// One logical member (cell) offered to its candidate group. logicalKey is the
/// caller's per-cell identity (dim/version binding); it is NOT the group key and
/// never enters the physical-code identity.
struct EJitGroupMember {
  uint64_t logicalKey = 0;
  uint32_t funcIndex = 0;
  /// The cell exists and is enabled for this group.
  bool cellActive = false;
  /// A cold cell may not become the representative (it has no live sampling
  /// window to measure), but it may still join as a waiter.
  bool cold = false;
};

/// A group generation. Every handle is generation-stamped so a late caller from
/// a retired generation can never act on the replacement.
struct EJitGroupHandle {
  uint64_t groupId = 0;
  uint64_t generation = 0;
  bool valid() const { return groupId != 0 && generation != 0; }
};

/// The ONE representative sampling session of a group generation.
struct EJitRepresentativeSession {
  uint64_t groupId = 0;
  uint64_t generation = 0;
  /// Identifies this exact attempt across queue items, callbacks and retries.
  uint64_t attemptToken = 0;
  /// Distinct identity from the request token: the sampling session the profile
  /// bundle belongs to.
  uint64_t samplingSessionId = 0;
  uint64_t logicalKey = 0;
  uint32_t funcIndex = 0;
  uint64_t dispatchLimit = 0;
  uint64_t dispatchCount = 0;
  /// Timestamp of the final allowed dispatch; 0 = unknown clock. Never the
  /// Tier-2 queue or compile time.
  uint64_t quotaEnd = 0;
  /// True only for the session that owns the group's quota.
  bool ownsQuota = false;
  bool valid() const { return attemptToken != 0 && samplingSessionId != 0; }
};

/// A non-representative member's token. It carries the exact generation it
/// joined: a token from a retired generation can never settle against the
/// replacement generation.
struct EJitWaiterToken {
  uint64_t groupId = 0;
  uint64_t generation = 0;
  uint64_t token = 0;
  uint64_t logicalKey = 0;
  bool valid() const { return token != 0; }
};

enum class EJitDispatchOutcome : uint8_t {
  /// One real dispatch counted, quota still open.
  Counted,
  /// The final allowed dispatch: counted and the boundary frozen exactly once.
  CountedAndClosed,
  /// The session is no longer the group's current owner (retired/replaced).
  Stale,
  /// Not a representative session of this group (or already closed).
  OutsideSession,
};

enum class EJitPublishOutcome : uint8_t {
  Published,
  /// A duplicate late callback for an already published session: the first
  /// bundle is untouched and no second bundle is created.
  AlreadyPublished,
  /// Retired/replaced session or generation.
  Stale,
  /// The bundle is not a real profile (no edge profile, no schema, or no
  /// trustworthy observation): publishing it would fabricate a shared path.
  InvalidBundle,
};

/// Physical-code decision for one member of a group generation.
enum class EJitMemberShare : uint8_t {
  /// The caller must link this member's own prepared code: it is the first
  /// validated identity of the generation (or the identity differs from the
  /// generation's physical object).
  Emit,
  /// The caller must use the group's existing physical object: the member's
  /// final identity compared equal to it (full IR + effective bindings, not a
  /// digest).
  Reuse,
  /// The generation's profile bundle is not published yet: the member stays on
  /// AOT (it must not compile a Tier-2 without the shared profile).
  NotReady,
  /// Retired generation/session, or already settled token.
  Stale,
  Cancelled,
  /// The member's PGO schema does not match the frozen bundle: reusing its code
  /// would silently consume a different profile.
  SchemaRejected,
};

struct EJitShareDecision {
  EJitMemberShare kind = EJitMemberShare::NotReady;
  uint64_t codeId = 0;
  void *fn = nullptr;
  std::string reason;
};

/// Read-only snapshot of ONE live group generation, for diagnostics and for the
/// runtime's C-level stats entry point. Every field is a plain copy of live
/// state: no raw registry pointer escapes and nothing can be mutated through it.
struct EJitGroupSnapshot {
  bool valid = false;
  uint64_t groupId = 0;
  uint64_t generation = 0;
  bool hasRepresentative = false;
  EJitRepresentativeSession representative;
  bool hasBundle = false;
  uint64_t bundleGeneration = 0;
  uint64_t bundleDispatchCount = 0;
  uint64_t bundleDispatchLimit = 0;
  uint64_t bundleQuotaEnd = 0;
  bool hasPhysical = false;
  uint64_t physicalCodeId = 0;
  void *physicalFn = nullptr;
  uint32_t waiters = 0;
  uint32_t members = 0;
  uint32_t settledMembers = 0;
};

/// Counters for honest reporting. Logical requests, physical codegen, the
/// representative sample scope, waiters, profile quality and retained bytes are
/// separate fields on purpose.
struct EJitGroupDiagnostics {
  uint64_t logicalRequests = 0;
  uint64_t admittedGroups = 0;
  uint64_t rejectedAdmissions = 0;
  uint64_t representativeSessions = 0;
  uint64_t representativeReElections = 0;
  uint64_t representativeDispatches = 0;
  uint64_t waitersJoined = 0;
  uint64_t waitersCompleted = 0;
  uint64_t waitersCancelled = 0;
  uint64_t bundlePublications = 0;
  uint64_t retainedBundleBytes = 0;
  uint64_t completeProfiles = 0;
  uint64_t approximateProfiles = 0;
  uint64_t edgeOnlyProfiles = 0;
  uint64_t valueDroppedProfiles = 0;
  uint64_t physicalCodeObjects = 0;
  uint64_t sharedPhysicalReuses = 0;
  uint64_t independentPhysicalObjects = 0;
  uint64_t schemaRejections = 0;
  uint64_t staleSettlements = 0;
};

/// One group's cold-path lifecycle owner. Not thread-safe: the compile owner
/// serializes group operations (the pool worker already serializes compilation).
class EJitRepresentativeGroupRegistry {
public:
  struct Limits {
    uint32_t maxGroups = 128;
    uint32_t maxMembersPerGroup = 128;
    uint64_t maxRetainedBundleBytes = 64ull * 1024 * 1024;
  };

  EJitRepresentativeGroupRegistry();
  explicit EJitRepresentativeGroupRegistry(Limits L);
  ~EJitRepresentativeGroupRegistry();
  EJitRepresentativeGroupRegistry(const EJitRepresentativeGroupRegistry &) =
      delete;
  EJitRepresentativeGroupRegistry &
  operator=(const EJitRepresentativeGroupRegistry &) = delete;

  /// The V1 admission gate. Pure function: no side effect, safe to call before
  /// the caller touches the pool/queue/group table.
  static EJitGroupAdmitReject
  admissionReject(const EJitGroupAdmissionPolicy &Policy);

  /// Open (or re-open) the group for a candidate-group id. A rejected policy
  /// returns an error and creates nothing.
  Expected<EJitGroupHandle> openGroup(uint64_t candidateGroupId,
                                      const EJitGroupAdmissionPolicy &Policy);

  /// Elect the group's representative from the offered members. The
  /// representative is the first LEGAL member in arrival order: active, not
  /// cold, and belonging to this group's function set - never a hardcoded
  /// cell 0. An existing live session of the same generation is returned
  /// unchanged (one representative per group generation).
  Expected<EJitRepresentativeSession>
  electRepresentative(const EJitGroupHandle &G,
                      ArrayRef<EJitGroupMember> Members, uint64_t nowUsec);

  /// The current representative session of a group, if any.
  const EJitRepresentativeSession *currentRepresentative(
      const EJitGroupHandle &G) const;

  /// Join a non-representative member as a waiter. Consumes no sampling
  /// admission: the member stays AOT until the generation's bundle is frozen.
  /// Rejected for an illegal member or an unknown/stale generation.
  Expected<EJitWaiterToken> joinWaiter(const EJitGroupHandle &G,
                                       const EJitGroupMember &M);

  /// One REAL granted Tier-1 dispatch = at most one quota unit, and only for
  /// the exact current owner (group + generation + attempt + session).
  EJitDispatchOutcome
  recordRepresentativeDispatch(const EJitGroupHandle &G,
                               const EJitRepresentativeSession &S,
                               uint64_t nowUsec);

  /// Freeze and publish the immutable bundle exactly once for this session.
  EJitPublishOutcome publishBundle(const EJitGroupHandle &G,
                                   const EJitRepresentativeSession &S,
                                   EJitProfileBundle Bundle);

  /// The generation's frozen bundle (shared, const, immutable). Null until
  /// publication. Every member consumes this same object.
  EJitFrozenProfileBundle bundleFor(const EJitGroupHandle &G) const;

  /// Exact schema compatibility of a consuming member against the frozen
  /// bundle: PGO name, CFG hash, name hash and every site count must match.
  /// Returns false with a reason instead of dropping value/scalar data.
  bool schemaCompatible(const EJitGroupHandle &G,
                        ArrayRef<PgoFunctionSchema> MemberSchema,
                        std::string *Reason = nullptr) const;

  /// Decide (no side effect on physical code) whether this waiter may reuse the
  /// generation's physical object. The compare is the emitter's exact
  /// full-identity compare; a digest is never accepted as proof. Callers must
  /// have validated the member's schema with schemaCompatible() first: schema
  /// compatibility and exact final-code eligibility are separate gates.
  EJitShareDecision decideMember(const EJitWaiterToken &W,
                                 const EJitFinalCodeIdentity &Identity,
                                 const EJitPreparedCodeEmitter &Emitter) const;

  /// Record the representative's own Tier-2 object as the generation's physical
  /// code (the first validated final identity). Later members reuse it only
  /// after their own exact compare. Returns false for a stale session or when
  /// the generation already has a physical object.
  bool noteRepresentativeCode(const EJitGroupHandle &G,
                              const EJitRepresentativeSession &S,
                              uint64_t CodeId, void *Fn, bool EmittedNew);

  /// Record the settled member exactly once, after the caller linked or reused
  /// code. Returns false when the token is stale/cancelled/unknown or was
  /// already settled (exactly-once settlement).
  bool completeMember(const EJitWaiterToken &W, uint64_t CodeId, void *Fn,
                      bool EmittedNew);

  /// Retire the current representative (cold/timeout/cancel) and open the next
  /// generation of the same group. Late callbacks of the retired session settle
  /// as Stale and can never write into the replacement generation.
  Expected<EJitGroupHandle>
  invalidateRepresentative(const EJitGroupHandle &G,
                           const EJitRepresentativeSession &S,
                           StringRef Reason);

  /// Cancel one waiter. Its exact token settles once as cancelled; the rest of
  /// the group is unaffected.
  bool cancelWaiter(const EJitWaiterToken &W);

  /// Cancel the current representative session without replacing it.
  bool cancelRepresentative(const EJitGroupHandle &G,
                            const EJitRepresentativeSession &S);

  size_t groupCount() const;
  size_t waiterCount(const EJitGroupHandle &G) const;
  bool memberSettled(const EJitWaiterToken &W) const;
  EJitGroupDiagnostics diagnostics() const;
  /// Read-only snapshot of one live generation (for diagnostics and the C stats
  /// entry point). An unknown/stale handle yields valid=false.
  EJitGroupSnapshot snapshot(const EJitGroupHandle &G) const;
  /// Read-only snapshot of the FIRST live group (lowest group id order of
  /// insertion). valid=false when no group exists. Used by the C stats entry
  /// point, which reports one live generation at a time.
  EJitGroupSnapshot firstGroupSnapshot() const;
  EJitGroupSnapshot snapshotAt(size_t Index) const;

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITREPRESENTATIVEGROUP_H