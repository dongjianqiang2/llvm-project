//===-- EJitGlobalLayout.cpp - Owner-side complete final layout
//------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include <algorithm>
#include <limits>

using namespace llvm::ejit;

namespace {
EJitCompileRequest slotRequest(const EJitSharedCacheSlot &S) {
  EJitCompileRequest R{};
  R.funcIndex = S.funcIndex;
  R.numDims = S.numDims;
  R.generation = S.generation;
  for (uint32_t I = 0; I < R.numDims; ++I) {
    R.dims[I] = S.dims[I];
    R.versions[I] = S.versions[I];
  }
  R.boundCount = S.layoutBoundCount ? S.layoutBoundCount - 1 : 0;
  for (uint32_t I = 0; I < R.boundCount; ++I)
    R.boundPointers[I] = S.layoutBounds[I];
  return R;
}
} // namespace

bool EJitSharedTaskPool::validateLayoutManifest(
    const std::vector<EJitCompileRequest> &manifest,
    const EJitLayoutBudgets &budgets, std::vector<LayoutMember> &members) {
  if (manifest.empty() || manifest.size() > budgets.maxMembers ||
      manifest.size() > kEJitSharedQueueSlots)
    return false;
  std::vector<uint32_t> occupancy(kEJitSharedCacheBuckets, 0);
  for (size_t I = 0; I < manifest.size(); ++I) {
    EJitCompileRequest R = manifest[I];
    R.generation = state_->generation.loadAcquire();
    const uint32_t tier = decodeReqTier(R.funcIndex);
    if (stripReqTier(R.funcIndex) >= kEJitSharedMaxFuncIndex ||
        R.numDims > kEJitSharedMaxDims ||
        !validateBoundPtrDescriptors(R.boundPointers, R.boundCount) ||
        tier != (state_->pgoEnabled.loadAcquire() ? kEJitTierPgoUse
                                                  : kEJitTierBaseline))
      return false;
    for (uint32_t D = 0; D < R.numDims; ++D) {
      if (R.dims[D].dimType >= kEJitSharedDimTypes ||
          R.dims[D].instanceId >= kEJitSharedInstances)
        return false;
      for (uint32_t E = 0; E < D; ++E)
        if (R.dims[D].dimType == R.dims[E].dimType)
          return false;
    }
    for (const auto &M : members) {
      // The legacy dispatch key cannot represent different bindings/versions
      // of the same entry+dims concurrently. Reject instead of aliasing them.
      EJitCompileRequest Existing = M.req, Candidate = R;
      for (uint32_t D = 0; D < Existing.numDims; ++D)
        Existing.versions[D] = 0;
      for (uint32_t D = 0; D < Candidate.numDims; ++D)
        Candidate.versions[D] = 0;
      if (sameIdentity(Existing, Candidate, false))
        return false;
    }
    uint32_t bucket =
        hashIdentity(stripReqTier(R.funcIndex), R.dims, R.numDims) %
        kEJitSharedCacheBuckets;
    if (++occupancy[bucket] > kEJitSharedCacheSlots)
      return false;
    LayoutMember M;
    M.req = R;
    // An entry declared with no dims and no bounds is name-only: the pool
    // captures its identity from the first admitted request (see
    // captureDeferredLayoutIdentity). A registered 0-dim/0-bound function is
    // indistinguishable from a deferred entry here, and never needs the
    // distinction: its requests always carry an empty identity and match the
    // member directly.
    M.nameOnly = (R.numDims == 0 && R.boundCount == 0);
    if (R.layoutVersionPolicy > 1) {
      EJIT_DIAG("global layout invalid version policy rank=%zu policy=%u", I,
                R.layoutVersionPolicy);
      return false;
    }
    if (!captureLayoutVersions(M, true))
      return false;
    members.push_back(M);
    EJIT_DIAG("global layout manifest rank=%zu func=%u dims=%u tier=%u", I,
              stripReqTier(R.funcIndex), R.numDims, tier);
    for (uint32_t D = 0; D < R.numDims; ++D)
      EJIT_DIAG("global layout manifest rank=%zu dim=%u type=%u instance=%u "
                "version=%u",
                I, D, R.dims[D].dimType, R.dims[D].instanceId, R.versions[D]);
    for (uint32_t B = 0; B < R.boundCount; ++B)
      EJIT_DIAG(
          "global layout manifest rank=%zu boundArg=%u address=%p bytes=%u", I,
          R.boundPointers[B].argIndex, R.boundPointers[B].rawPtr,
          R.boundPointers[B].size);
  }
  return true;
}

bool EJitSharedTaskPool::captureLayoutVersions(LayoutMember &M,
                                               bool validateExplicit) {
  bool active = true;
  for (uint32_t D = 0; D < M.req.numDims; ++D) {
    auto P = M.req.dims[D];
    uint32_t current = instanceVersion(P.dimType, P.instanceId);
    bool enabled = isInstanceEnabled(P.dimType, P.instanceId);
    if (M.req.layoutVersionPolicy == 1 && validateExplicit &&
        (!enabled || M.req.versions[D] != current)) {
      EJIT_DIAG("global layout explicit version rejected func=%u dim=%u "
                "instance=%u expected=%u live=%u enabled=%u",
                stripReqTier(M.req.funcIndex), P.dimType, P.instanceId,
                M.req.versions[D], current, static_cast<unsigned>(enabled));
      return false;
    }
    active &= enabled;
    M.req.versions[D] = current;
  }
  M.versionCaptured = active;
  return true;
}

bool EJitSharedTaskPool::requestGlobalLayoutRefresh(
    const std::vector<EJitCompileRequest> &manifest, uint64_t fingerprint) {
  if (!state_ || !isOwner_ || !layoutConfigured_ ||
      state_->initState.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedInitState::Ready))
    return false;
  uint32_t expected = 0;
  if (!layoutRefreshCommand_.compareExchange(expected, 1))
    return false;
  std::vector<LayoutMember> members;
  if (!validateLayoutManifest(manifest, layoutBudgets_, members)) {
    layoutRefreshCommand_.storeRelease(0);
    return false; // An invalid replacement never invalidates the legal layout.
  }
  layoutRefreshMembers_ = std::move(members);
  layoutRefreshFingerprint_ = fingerprint;
  layoutRefreshCommand_.storeRelease(2);
  return true;
}

bool EJitSharedTaskPool::serviceGlobalLayoutRefresh() {
  if (layoutRefreshCommand_.loadAcquire() == 2 &&
      globalLayoutState() != EJitGlobalLayoutState::Refreshing) {
    layoutRefreshPolls_ = 0;
    state_->globalLayoutState.storeRelease(
        static_cast<uint32_t>(EJitGlobalLayoutState::Refreshing));
  }
  if (globalLayoutState() != EJitGlobalLayoutState::Refreshing)
    return false;
  for (const auto &B : state_->buckets)
    for (const auto &S : B.slots)
      if (S.samplingExecutions.loadAcquire() != 0) {
        if (++layoutRefreshPolls_ > layoutBudgets_.maxCollectionPolls) {
          if (layoutRefreshCommand_.loadAcquire() == 2) {
            layoutRefreshMembers_.clear();
            layoutRefreshCommand_.storeRelease(0);
          }
          failGlobalLayout(EJitGlobalLayoutError::Cancelled, 0);
        }
        return true; // Nonblocking; no profile reset or slot reuse before
                     // leaves.
      }
  clearGlobalArtifacts();
  // Staged collective-window results (and any other staged entries) own
  // Pending markers; drop them BEFORE the epoch changes — afterwards their
  // generation can no longer match and the slot would linger forever.
  for (const auto &P : pendingPublishes_)
    cacheDropPending(P.req, P.fn);
  pendingPublishes_.clear();
  layoutDraining_.clear();
  // Parked unlisted Tier-2 entries and deferred batch requests belong to the
  // retired generation. Only their staged MARKERS must be dropped BEFORE the
  // epoch changes — after the bump cacheDropPending can no longer match the
  // marker's generation and the Pending slot would linger forever. The dedup
  // claims need no per-entry handling here: the post-bump sweep below clears
  // the whole previous generation.
  for (const auto &P : pendingBatchCompiles_)
    if (P.hasSharedMarker)
      cacheDropPending(P.req, nullptr);
  pendingBatchCompiles_.clear();
  layoutDeferredT2_.clear();
  layoutUnlistedDraining_.clear();
  layoutReleasedSampling_.clear();
  layoutAppendedPublishes_.clear();
  // Drain old requests before the epoch changes. No new misses are admitted in
  // Refreshing; late producers retain the old generation and are dropped later.
  EJitCompileRequest old{};
  while (queuePop(old))
    dedupClear(old.funcIndex, old.generation);
  if (layoutReset_ && !layoutReset_(layoutCtx_)) {
    failGlobalLayout(EJitGlobalLayoutError::Preparation, 0);
    return true;
  }
  uint32_t expected = 0;
  while (!state_->pgoAdmissionLock.compareExchange(expected, 1))
    expected = 0;
  for (auto &F : state_->pgoActiveFunctions)
    F.storeRelaxed(0);
  for (auto &Q : state_->pgoProgressQuarters)
    Q.storeRelaxed(0);
  state_->pgoActiveFunctionCount.storeRelaxed(0);
  state_->pgoCompletedFunctions.storeRelaxed(0);
  const uint32_t previousGeneration = state_->generation.fetchAdd(1);
  state_->pgoAdmissionLock.storeRelease(0);
  for (uint32_t F = 0; F < kEJitSharedMaxFuncIndex; ++F)
    dedupClear(F, previousGeneration);
  const bool replacement = layoutRefreshCommand_.loadAcquire() == 2;
  if (replacement)
    layoutMembers_ = std::move(layoutRefreshMembers_);
  const uint32_t generation = previousGeneration + 1;
  for (auto &M : layoutMembers_) {
    auto req = M.req;
    // Explicit descriptors assert the requested admission snapshot, not an
    // instruction to keep executing a stale version after lifecycle turnover.
    if (!replacement)
      req.layoutVersionPolicy = 0;
    // nameOnly/identityCaptured are DECLARATION semantics, not generation-
    // scoped staging state like ready/artifact/published: wiping them here
    // would turn a name-only member back into an exact one, and a later
    // identity of its function would then precision-kill the refreshed
    // layout. The explicit-replacement path is safe by construction — its
    // fresh members go through validateLayoutManifest, which re-marks the
    // name-only entries.
    const bool NameOnly = M.nameOnly;
    const bool IdentityCaptured = M.identityCaptured;
    M = LayoutMember{};
    M.req = req;
    M.nameOnly = NameOnly;
    M.identityCaptured = IdentityCaptured;
    M.req.generation = generation;
    if (!captureLayoutVersions(M, replacement)) {
      failGlobalLayout(EJitGlobalLayoutError::Manifest,
                       &M - layoutMembers_.data());
      layoutRefreshCommand_.storeRelease(0);
      return true;
    }
  }
  state_->dispatchEpoch.fetchAdd(1);
  icacheDrainAll("global-layout-refresh");
  layoutPolls_ = layoutRefreshPolls_ = layoutPublishPolls_ = 0;
  layoutFailureRecorded_ = false;
  state_->globalLayoutRequired.storeRelaxed(layoutMembers_.size());
  state_->globalLayoutReady.storeRelaxed(0);
  state_->globalLayoutPublished.storeRelaxed(0);
  state_->globalLayoutUnlistedReady.storeRelaxed(0);
  state_->globalLayoutUnlistedAppended.storeRelaxed(0);
  state_->globalLayoutError.storeRelaxed(0);
  state_->globalLayoutErrorRank.storeRelaxed(0);
  if (replacement) {
    if (layoutRefreshFingerprint_) {
      regFingerprint_ = layoutRefreshFingerprint_;
      state_->registrationFingerprint.storeRelease(regFingerprint_);
    }
    layoutRefreshCommand_.storeRelease(0);
  }
  state_->globalLayoutState.storeRelease(
      static_cast<uint32_t>(EJitGlobalLayoutState::Collecting));
  EJIT_DIAG("global layout refreshed in-process gen=%u members=%zu; retired "
            "pools retained",
            generation, layoutMembers_.size());
  return true;
}

bool EJitSharedTaskPool::configureGlobalLayout(
    const std::vector<EJitCompileRequest> &manifest,
    const EJitLayoutBudgets &budgets, PrepareArtifactCallback prepare,
    LinkArtifactCallback link, DropArtifactCallback drop, void *ctx,
    SamplingPreparedCallback sampled) {
  if (!state_ || manifest.empty() || !prepare || !link || !drop ||
      layoutConfigured_ ||
      kEJitSharedCacheBuckets * kEJitSharedCacheSlots >= 0x8000u ||
      budgets.maxMembers == 0 || manifest.size() > budgets.maxMembers ||
      manifest.size() > kEJitSharedQueueSlots ||
      budgets.maxCollectionPolls == 0 || budgets.maxArtifactBytes == 0)
    return false;
  const auto Init =
      static_cast<EJitSharedInitState>(state_->initState.loadAcquire());
  if (Init != EJitSharedInitState::Initializing &&
      !(Init == EJitSharedInitState::Ready &&
        state_->workerTaskId.loadAcquire() == 0))
    return false;
  std::vector<LayoutMember> members;
  if (!validateLayoutManifest(manifest, budgets, members))
    return false;
  for (const auto &M : members)
    if (M.req.boundCount)
      state_->globalLayoutBoundFunctions[stripReqTier(M.req.funcIndex)]
          .storeRelease(1);
  layoutMembers_ = std::move(members);
  layoutBudgets_ = budgets;
  layoutPrepare_ = prepare;
  layoutLink_ = link;
  layoutDrop_ = drop;
  layoutCtx_ = ctx;
  layoutSampled_ = sampled;
  layoutConfigured_ = true;
  layoutPolls_ = 0;
  layoutSamplingThreshold_ = state_->tier2Threshold.loadAcquire();
  layoutSamplingMax_ = state_->pgoMaxActiveFunctions.loadAcquire();
  layoutAppendedPublishes_.clear();
  state_->globalLayoutRequired.storeRelaxed(layoutMembers_.size());
  state_->globalLayoutUnlistedReady.storeRelaxed(0);
  state_->globalLayoutUnlistedAppended.storeRelaxed(0);
  state_->globalLayoutState.storeRelease(
      static_cast<uint32_t>(EJitGlobalLayoutState::Collecting));
  return true;
}

size_t EJitSharedTaskPool::globalMemberFor(const EJitCompileRequest &R) const {
  for (size_t I = 0; I < layoutMembers_.size(); ++I)
    if (sameIdentity(layoutMembers_[I].req, R))
      return I;
  return layoutMembers_.size();
}

size_t EJitSharedTaskPool::captureDeferredLayoutIdentity(
    const EJitCompileRequest &R) {
  // An empty request identity has nothing to capture: it can only ever match
  // a member directly, exactly as before.
  if (R.numDims == 0 && R.boundCount == 0)
    return layoutMembers_.size();
  // Same caps validateLayoutManifest enforces on declared dims; a request
  // outside them is left unmatched and takes the ordinary routing.
  for (uint32_t D = 0; D < R.numDims; ++D)
    if (R.dims[D].dimType >= kEJitSharedDimTypes ||
        R.dims[D].instanceId >= kEJitSharedInstances)
      return layoutMembers_.size();
  for (size_t I = 0; I < layoutMembers_.size(); ++I) {
    LayoutMember &M = layoutMembers_[I];
    if (!M.nameOnly || M.identityCaptured ||
        stripReqTier(M.req.funcIndex) != stripReqTier(R.funcIndex))
      continue;
    // Mirror the manifest's dispatch-key collision rule: another member
    // already holding this (funcIndex, dims) — regardless of versions or
    // bindings — is the aliasing configuration validateLayoutManifest
    // rejects. Leave the request unmatched so it takes the unlisted routing
    // instead of creating two bodies that evict each other in one bucket.
    bool Collides = false;
    for (size_t J = 0; !Collides && J < layoutMembers_.size(); ++J) {
      const auto &Other = layoutMembers_[J];
      if (J == I || Other.req.numDims != R.numDims ||
          stripReqTier(Other.req.funcIndex) != stripReqTier(R.funcIndex))
        continue;
      Collides = true;
      for (uint32_t D = 0; D < R.numDims; ++D)
        if (Other.req.dims[D].dimType != R.dims[D].dimType ||
            Other.req.dims[D].instanceId != R.dims[D].instanceId) {
          Collides = false;
          break;
        }
    }
    if (Collides)
      return layoutMembers_.size();
    // Copy ONLY the identity payload: the tier encoding, generation and
    // fallback pointer stay the member's own.
    M.req.numDims = R.numDims;
    for (uint32_t D = 0; D < R.numDims; ++D) {
      M.req.dims[D] = R.dims[D];
      M.req.versions[D] = R.versions[D];
    }
    M.req.boundCount = R.boundCount;
    for (uint32_t B = 0; B < R.boundCount; ++B)
      M.req.boundPointers[B] = R.boundPointers[B];
    M.identityCaptured = true;
    EJIT_DIAG("global layout captured name-only member rank=%zu func=%u "
              "dims=%u bounds=%u",
              I, stripReqTier(R.funcIndex), R.numDims, R.boundCount);
    return I;
  }
  return layoutMembers_.size();
}

bool EJitSharedTaskPool::layoutToleratesUnlistedIdentity(
    uint32_t funcIndex) const {
  for (const auto &M : layoutMembers_)
    if (M.nameOnly && stripReqTier(M.req.funcIndex) == funcIndex)
      return true;
  return false;
}

bool EJitSharedTaskPool::layoutDeclaresFunc(uint32_t funcIndex) const {
  for (const auto &M : layoutMembers_)
    if (stripReqTier(M.req.funcIndex) == funcIndex)
      return true;
  return false;
}

bool EJitSharedTaskPool::layoutTier2AlreadyDelivered(
    const EJitCompileRequest &R) const {
  uint32_t bucket =
      hashIdentity(stripReqTier(R.funcIndex), R.dims, R.numDims) %
      kEJitSharedCacheBuckets;
  for (const auto &S : state_->buckets[bucket].slots)
    if (S.state.loadAcquire() ==
            static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
        S.tier.loadAcquire() == kEJitTierPgoUse &&
        sameIdentity(slotRequest(S), R))
      return true;
  // A collective-append body parked for the post-COMPLETE flush counts as
  // delivered: a re-arm raced against it must not win the slot.
  for (const auto &P : layoutAppendedPublishes_)
    if (sameIdentity(P.req, R))
      return true;
  return false;
}

void EJitSharedTaskPool::refreshLayoutUnlistedReady() {
  if (!layoutConfigured_ ||
      globalLayoutState() != EJitGlobalLayoutState::Collecting)
    return;
  state_->globalLayoutUnlistedReady.storeRelaxed(static_cast<uint32_t>(
      layoutDeferredT2_.size() + pendingBatchCompiles_.size()));
}

bool EJitSharedTaskPool::samplingDrained(const EJitCompileRequest &R) const {
  uint32_t bucket = hashIdentity(stripReqTier(R.funcIndex), R.dims, R.numDims) %
                    kEJitSharedCacheBuckets;
  for (const auto &S : state_->buckets[bucket].slots)
    if (S.state.loadAcquire() ==
            static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
        S.tier.loadAcquire() == kEJitTierInstrumented &&
        sameIdentity(slotRequest(S), R))
      return S.hitCount.loadAcquire() >= state_->tier2Threshold.loadAcquire() &&
             S.samplingExecutions.loadAcquire() == 0 &&
             S.samplingReturns.loadAcquire() >=
                 state_->tier2Threshold.loadAcquire();
  return false;
}

void EJitSharedTaskPool::releaseSamplingAdmission(const EJitCompileRequest &R) {
  if (!samplingDrained(R))
    return;
  for (const auto &Released : layoutReleasedSampling_)
    if (sameIdentity(Released, R))
      return;
  // At most maxMembers plus kEJitSharedQueueSlots drained append requests
  // can be waiting for publication in this generation.
  layoutReleasedSampling_.push_back(R);
  uint32_t expected = 0;
  while (!state_->pgoAdmissionLock.compareExchange(expected, 1))
    expected = 0;
  const uint32_t encoded = stripReqTier(R.funcIndex) + 1;
  for (uint32_t I = 0; I < kEJitSharedMaxConcurrentProfiles; ++I)
    if (state_->pgoActiveFunctions[I].loadRelaxed() == encoded) {
      state_->pgoActiveFunctions[I].storeRelease(0);
      state_->pgoProgressQuarters[I].storeRelaxed(0);
      state_->pgoActiveFunctionCount.fetchSub(1);
      EJIT_DIAG("global layout sampling drained func=%u; admission released "
                "before final allocation",
                encoded - 1);
      break;
    }
  state_->pgoAdmissionLock.storeRelease(0);
}

void EJitSharedTaskPool::clearGlobalArtifacts() {
  for (auto &M : layoutMembers_) {
    if (M.artifact && layoutDrop_)
      layoutDrop_(layoutCtx_, M.artifact);
    M.artifact = nullptr;
  }
  state_->globalLayoutArtifactBytes.storeRelaxed(0);
}

void EJitSharedTaskPool::invalidateMaterializedLayout(bool includeCollection) {
  uint32_t phase = state_->globalLayoutState.loadAcquire();
  if (!includeCollection &&
      phase == static_cast<uint32_t>(EJitGlobalLayoutState::Failed)) {
    state_->globalLayoutState.compareExchange(
        phase, static_cast<uint32_t>(EJitGlobalLayoutState::Refreshing));
    return;
  }
  while (phase >= static_cast<uint32_t>(includeCollection
                                            ? EJitGlobalLayoutState::Collecting
                                            : EJitGlobalLayoutState::Linking) &&
         phase <= static_cast<uint32_t>(EJitGlobalLayoutState::Complete)) {
    if (state_->globalLayoutState.compareExchange(
            phase,
            static_cast<uint32_t>(includeCollection
                                      ? EJitGlobalLayoutState::Failed
                                      : EJitGlobalLayoutState::Refreshing))) {
      state_->globalLayoutError.storeRelease(
          static_cast<uint32_t>(EJitGlobalLayoutError::StaleIdentity));
      return;
    }
  }
}

void EJitSharedTaskPool::failGlobalLayout(EJitGlobalLayoutError error,
                                          uint32_t rank) {
  if (globalLayoutState() == EJitGlobalLayoutState::Refreshing &&
      error == EJitGlobalLayoutError::StaleIdentity)
    return; // Worker will retire this attempt and rebuild the full generation.
  if (globalLayoutState() == EJitGlobalLayoutState::Failed &&
      layoutFailureRecorded_)
    return;
  bool lifecycleChanged = false;
  for (const auto &M : layoutMembers_)
    lifecycleChanged |= M.versionCaptured && !versionsCurrent(M.req);
  if (error == EJitGlobalLayoutError::StaleIdentity && lifecycleChanged &&
      state_->mode.loadAcquire() ==
          static_cast<uint32_t>(EJitCompileMode::Async)) {
    state_->globalLayoutState.storeRelease(
        static_cast<uint32_t>(EJitGlobalLayoutState::Refreshing));
    layoutRefreshPolls_ = 0;
    return;
  }
  layoutFailureRecorded_ = true;
  state_->globalLayoutError.storeRelaxed(static_cast<uint32_t>(error));
  state_->globalLayoutErrorRank.storeRelaxed(rank);
  state_->globalLayoutState.storeRelease(
      static_cast<uint32_t>(EJitGlobalLayoutState::Failed));
  const uint32_t UnlistedParked =
      static_cast<uint32_t>(layoutDeferredT2_.size());
  const uint32_t UnlistedBatch =
      static_cast<uint32_t>(pendingBatchCompiles_.size());
  EJIT_DIAG("global layout FAILED gen=%u rank=%u reason=%u ready=%u/%u "
            "published=%u unlistedParked=%u unlistedBatch=%u "
            "artifactBytes=%llu; legal fallback retained (unlisted released "
            "to ordinary compilation)",
            state_->generation.loadAcquire(), rank,
            static_cast<unsigned>(error),
            state_->globalLayoutReady.loadRelaxed(),
            state_->globalLayoutRequired.loadRelaxed(),
            state_->globalLayoutPublished.loadRelaxed(), UnlistedParked,
            UnlistedBatch,
            static_cast<unsigned long long>(
                state_->globalLayoutArtifactBytes.loadRelaxed()));
  for (size_t I = 0; I < layoutMembers_.size(); ++I)
    EJIT_DIAG("global layout failure member rank=%zu published=%u body=%p", I,
              static_cast<unsigned>(layoutMembers_[I].published),
              layoutMembers_[I].fn);
  for (const auto &P : pendingPublishes_) {
    if (publishFn_)
      publishFn_(publishCtx_, P.req, false);
    // A staged entry (collective-window batch result) owns a Pending slot
    // with this exact fn pointer; dropping it lets a fresh miss re-enqueue on
    // the ordinary fallback instead of wedging on AlreadyPending. Entries
    // parked without staging (Tier-1 retries, codeReady Tier-2) carry no
    // marker, and cacheDropPending no-ops for them.
    cacheDropPending(P.req, P.fn);
    finishPgoRequest(P.req, false, "layout-pending-t1-abort");
    dedupClear(P.req.funcIndex, P.req.generation);
  }
  pendingPublishes_.clear();
  // Parked unlisted Tier-2 entries: their admission and dedup claims were
  // released at park time; clearing the list returns them to ordinary misses
  // on the fallback paths (the dedupClear is an idempotent safety net for a
  // re-armed duplicate that re-claimed after its park).
  for (const auto &R : layoutDeferredT2_)
    dedupClear(R.funcIndex, R.generation);
  layoutDeferredT2_.clear();
  // Deferred batch requests: drop every staged marker (and any retained
  // coarse claim) so a fresh miss re-enqueues on the ordinary path instead of
  // sticking on AlreadyPending.
  for (const auto &P : pendingBatchCompiles_) {
    if (P.hasSharedMarker)
      cacheDropPending(P.req, nullptr);
    dedupClear(P.req.funcIndex, P.req.generation);
  }
  pendingBatchCompiles_.clear();
  layoutAppendedPublishes_.clear();
  state_->globalLayoutUnlistedReady.storeRelaxed(0);
  state_->globalLayoutUnlistedAppended.storeRelaxed(0);
  // Objects are not executable. Linked/published NO_RECLAIM bytes remain
  // charged to their pools and are NEVER freed or overwritten here.
  clearGlobalArtifacts();
  for (auto &M : layoutMembers_) {
    cacheDropPending(M.req, nullptr);
    dedupClear(M.req.funcIndex, M.req.generation);
  }
}

bool EJitSharedTaskPool::stageGlobalArtifact(const EJitCompileRequest &R) {
  size_t rank = globalMemberFor(R);
  if (rank == layoutMembers_.size()) {
    failGlobalLayout(EJitGlobalLayoutError::Manifest, rank);
    dedupClear(R.funcIndex, R.generation);
    return false;
  }
  auto &M = layoutMembers_[rank];
  if (M.ready) {
    dedupClear(R.funcIndex, R.generation);
    return true;
  }
  if (R.generation != state_->generation.loadAcquire() || !versionsCurrent(R)) {
    failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, rank);
    return false;
  }
  if (decodeReqTier(R.funcIndex) == kEJitTierPgoUse && !samplingDrained(R))
    return false;
  void *artifact = nullptr;
  uint64_t bytes = 0;
  if (!layoutPrepare_(layoutCtx_, R, &artifact, &bytes) || !artifact) {
    if (artifact)
      layoutDrop_(layoutCtx_, artifact);
    failGlobalLayout(EJitGlobalLayoutError::Preparation, rank);
    return false;
  }
  uint64_t used = state_->globalLayoutArtifactBytes.loadRelaxed();
  if (R.generation != state_->generation.loadAcquire() || !versionsCurrent(R) ||
      bytes > layoutBudgets_.maxArtifactBytes - used) {
    layoutDrop_(layoutCtx_, artifact);
    failGlobalLayout(bytes > layoutBudgets_.maxArtifactBytes - used
                         ? EJitGlobalLayoutError::ArtifactBudget
                         : EJitGlobalLayoutError::StaleIdentity,
                     rank);
    return false;
  }
  M.artifact = artifact;
  M.bytes = bytes;
  M.ready = true;
  state_->globalLayoutArtifactBytes.storeRelaxed(used + bytes);
  state_->globalLayoutReady.fetchAdd(1);
  if (decodeReqTier(R.funcIndex) == kEJitTierPgoUse) {
    if (layoutSampled_)
      layoutSampled_(layoutCtx_, R);
    releaseSamplingAdmission(R);
    M.samplingReleased = true;
  } else
    cacheStageBatchRequest(R); // Complete-key marker, no executable pointer.
  dedupClear(R.funcIndex, R.generation);
  EJIT_DIAG("global layout ARTIFACT_READY rank=%zu func=%u bytes=%llu", rank,
            stripReqTier(R.funcIndex), static_cast<unsigned long long>(bytes));
  return true;
}

bool EJitSharedTaskPool::serviceGlobalLayout() {
  if (!layoutConfigured_)
    return false;
  if (serviceGlobalLayoutRefresh())
    return true;
  if (globalLayoutState() == EJitGlobalLayoutState::Collecting &&
      !pendingPublishes_.empty()) {
    auto P = pendingPublishes_.front();
    if (P.req.generation != state_->generation.loadAcquire() ||
        !versionsCurrent(P.req)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity,
                       globalMemberFor(P.req));
      return true;
    }
    EJitCompiledCodeInfo info{};
    if (!codeRangeFn_ || !codeRangeFn_(codeRangeCtx_, P.fn, &info)) {
      failGlobalLayout(EJitGlobalLayoutError::Range, globalMemberFor(P.req));
      return true;
    }
    auto result = cachePublish(P.req, P.fn, &info, false);
    if (result == EJitPublishStatus::Retry) {
      if (++layoutPublishPolls_ > layoutBudgets_.maxCollectionPolls)
        failGlobalLayout(EJitGlobalLayoutError::Publish,
                         globalMemberFor(P.req));
      return false; // Other queue/control work may continue while this retries.
    }
    if (result != EJitPublishStatus::Published) {
      failGlobalLayout(EJitGlobalLayoutError::Publish, globalMemberFor(P.req));
      return true;
    }
    if (publishFn_)
      publishFn_(publishCtx_, P.req, true);
    dedupClear(P.req.funcIndex, P.req.generation);
    EJIT_STAT_INC(state_->counters.tier1Compiles);
    EJIT_STAT_INC(state_->counters.asyncCompiles);
    pendingPublishes_.erase(pendingPublishes_.begin());
    return true;
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Failed) {
    if (!layoutFailureRecorded_)
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
    // A layout failure relinquishes ordering, not the live T1 round. Ordinary
    // fallback must finish its remaining samples and consume that same profile.
    return false;
  }
  // Capture live versions on the first active admission, not cold init's zero.
  // A later turnover rebuilds ALL members, including unchanged required roots.
  for (auto &M : layoutMembers_) {
    if (!M.versionCaptured) {
      captureLayoutVersions(M, false);
    } else if (!versionsCurrent(M.req)) {
      layoutRefreshPolls_ = 0;
      state_->globalLayoutState.storeRelease(
          static_cast<uint32_t>(EJitGlobalLayoutState::Refreshing));
      return true;
    }
  }
  bool ExpectedPgo =
      decodeReqTier(layoutMembers_.front().req.funcIndex) == kEJitTierPgoUse;
  if (state_->mode.loadAcquire() !=
          static_cast<uint32_t>(EJitCompileMode::Async) ||
      (state_->pgoEnabled.loadAcquire() != 0) != ExpectedPgo ||
      (ExpectedPgo &&
       (state_->tier2Threshold.loadAcquire() != layoutSamplingThreshold_ ||
        state_->pgoMaxActiveFunctions.loadAcquire() != layoutSamplingMax_))) {
    failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
    return true;
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Complete) {
    for (size_t I = 0; I < layoutMembers_.size(); ++I)
      if (!versionsCurrent(layoutMembers_[I].req) ||
          layoutMembers_[I].req.generation !=
              state_->generation.loadAcquire()) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, I);
        return true;
      }
    return false;
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Collecting) {
    for (size_t I = 0; I < layoutMembers_.size(); ++I)
      if (layoutMembers_[I].ready && !versionsCurrent(layoutMembers_[I].req)) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, I);
        return true;
      }
    if (++layoutPolls_ > layoutBudgets_.maxCollectionPolls) {
      size_t missing = 0;
      while (missing < layoutMembers_.size() && layoutMembers_[missing].ready)
        ++missing;
      failGlobalLayout(EJitGlobalLayoutError::MissingTarget, missing);
      return true;
    }
    for (size_t I = 0; I < layoutDraining_.size();) {
      auto R = layoutDraining_[I];
      if (!versionsCurrent(R) ||
          R.generation != state_->generation.loadAcquire()) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity,
                         globalMemberFor(R));
        return true;
      }
      if (!samplingDrained(R)) {
        ++I;
        continue;
      }
      layoutDraining_.erase(layoutDraining_.begin() + I);
      stageGlobalArtifact(R);
      if (globalLayoutState() == EJitGlobalLayoutState::Failed)
        return true;
    }
    if (state_->globalLayoutReady.loadAcquire() != layoutMembers_.size())
      return false;
    if (!transitionGlobalLayout(EJitGlobalLayoutState::Collecting,
                                EJitGlobalLayoutState::Linking)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
    uintptr_t previous = 0;
    for (size_t I = 0; I < layoutMembers_.size(); ++I) {
      auto &M = layoutMembers_[I];
      if (M.req.generation != state_->generation.loadAcquire() ||
          !versionsCurrent(M.req)) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, I);
        return true;
      }
      if (!layoutLink_(layoutCtx_, M.artifact, &M.fn) || !M.fn) {
        failGlobalLayout(EJitGlobalLayoutError::Linking, I);
        return true;
      }
      uintptr_t current = reinterpret_cast<uintptr_t>(M.fn);
      if (I && current <= previous) {
        failGlobalLayout(EJitGlobalLayoutError::PhysicalOrder, I);
        return true;
      }
      previous = current;
      EJIT_DIAG("global layout LINKED rank=%zu func=%u body=%p", I,
                stripReqTier(M.req.funcIndex), M.fn);
    }
    if (!transitionGlobalLayout(EJitGlobalLayoutState::Linking,
                                EJitGlobalLayoutState::Linked)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
    // Collective append: unlisted functions join AFTER the manifest bodies.
    // Parked Tier-2 entries first (funcIndex ascending), then the deferred
    // batch requests (existing dims→funcIndex→tier order) — the single seal
    // below then covers every allocation made in this window.
    uint32_t Appended = 0;
    const size_t PublishBase = pendingPublishes_.size();
    std::vector<EJitCompileRequest> DeferredT2;
    DeferredT2.swap(layoutDeferredT2_);
    std::stable_sort(DeferredT2.begin(), DeferredT2.end(),
                     [](const auto &A, const auto &B) {
                       return stripReqTier(A.funcIndex) < stripReqTier(B.funcIndex);
                     });
    // A re-armed identity can park more than once while collection waited;
    // compile only the latest request of each identity.
    for (size_t I = 0; I < DeferredT2.size(); ++I) {
      bool Superseded = false;
      for (size_t J = I + 1; J < DeferredT2.size(); ++J)
        if (sameIdentity(DeferredT2[J], DeferredT2[I])) {
          Superseded = true;
          break;
        }
      if (Superseded)
        continue;
      // Count only requests that actually produced an appended body: a
      // compile/publish failure leaves the function to the ordinary retry
      // paths and is not part of the collective window. Failure detection
      // uses the global compile/publish counters (the same heuristic as the
      // member Tier-1 check in pollOne); a concurrent unrelated failure can
      // only undercount the diagnostic, never overcount it.
      uint64_t Failures = state_->counters.compileFailed.loadRelaxed() +
                          state_->counters.publishFailed.loadRelaxed();
      runCompile(DeferredT2[I]);
      workerThrottle();
      if (Failures == state_->counters.compileFailed.loadRelaxed() +
                          state_->counters.publishFailed.loadRelaxed())
        ++Appended;
    }
    const uint32_t BatchCount =
        static_cast<uint32_t>(pendingBatchCompiles_.size());
    uint64_t BatchFailures = state_->counters.compileFailed.loadRelaxed() +
                             state_->counters.publishFailed.loadRelaxed();
    compilePendingBatchRequests();
    BatchFailures = state_->counters.compileFailed.loadRelaxed() +
                        state_->counters.publishFailed.loadRelaxed() -
                    BatchFailures;
    if (BatchFailures > BatchCount)
      BatchFailures = BatchCount;
    Appended += BatchCount - static_cast<uint32_t>(BatchFailures);
    // Snapshot the appended slice (parked results pushed by the runCompiles
    // above) so the COMPLETE summary can list it even after post-freeze
    // ordinary arrivals mix into the pending list.
    layoutAppendedPublishes_.assign(pendingPublishes_.begin() + PublishBase,
                                    pendingPublishes_.end());
    state_->globalLayoutUnlistedReady.storeRelaxed(0);
    state_->globalLayoutUnlistedAppended.storeRelaxed(Appended);
    EJIT_DIAG("global layout collective append: unlistedT2=%zu batch=%u "
              "appended=%u",
              DeferredT2.size(), BatchCount, Appended);
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Linked ||
      globalLayoutState() == EJitGlobalLayoutState::Sealing) {
    if (globalLayoutState() == EJitGlobalLayoutState::Linked &&
        !transitionGlobalLayout(EJitGlobalLayoutState::Linked,
                                EJitGlobalLayoutState::Sealing)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
    if (!codeBatchFlushFn_ || !codeBatchFlushFn_(codeBatchCtx_)) {
      failGlobalLayout(EJitGlobalLayoutError::Seal, 0);
      return true;
    }
    for (size_t I = 0; I < layoutMembers_.size(); ++I) {
      auto &M = layoutMembers_[I];
      if (!codeReadyFn_ || !codeReadyFn_(codeBatchCtx_, M.fn) ||
          !codeRangeFn_ || !codeRangeFn_(codeRangeCtx_, M.fn, &M.range) ||
          !M.range.codeSize || M.range.poolKind != EJitCodePoolKind::Near ||
          reinterpret_cast<uintptr_t>(M.fn) < M.range.codeStart ||
          reinterpret_cast<uintptr_t>(M.fn) - M.range.codeStart >=
              M.range.codeSize) {
        failGlobalLayout(EJitGlobalLayoutError::Range, I);
        return true;
      }
    }
    clearGlobalArtifacts();
    if (!transitionGlobalLayout(EJitGlobalLayoutState::Sealing,
                                EJitGlobalLayoutState::Publishing)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Publishing) {
    for (size_t I = 0; I < layoutMembers_.size(); ++I) {
      auto &M = layoutMembers_[I];
      if (M.published)
        continue;
      if (globalLayoutState() != EJitGlobalLayoutState::Publishing) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, I);
        return true;
      }
      auto PS = cachePublish(M.req, M.fn, &M.range,
                             decodeReqTier(M.req.funcIndex) == kEJitTierPgoUse);
      if (PS == EJitPublishStatus::Retry) {
        if (++layoutPublishPolls_ > layoutBudgets_.maxCollectionPolls)
          failGlobalLayout(EJitGlobalLayoutError::Publish, I);
        return true; // Linked/sealed addresses and published subset stay owned.
      }
      if (PS != EJitPublishStatus::Published) {
        failGlobalLayout(PS == EJitPublishStatus::VersionMismatch
                             ? EJitGlobalLayoutError::StaleIdentity
                             : EJitGlobalLayoutError::Publish,
                         I);
        return true;
      }
      M.published = true;
      state_->globalLayoutPublished.fetchAdd(1);
      EJIT_STAT_INC(state_->counters.asyncCompiles);
      if (decodeReqTier(M.req.funcIndex) == kEJitTierPgoUse) {
        EJIT_STAT_INC(state_->counters.tier2Compiles);
        finishPgoRequest(M.req, true);
      }
      if (publishFn_)
        publishFn_(publishCtx_, M.req, true);
      EJIT_DIAG(
          "global layout PUBLISHED rank=%zu func=%u body=%p range=0x%llx+%llu",
          I, stripReqTier(M.req.funcIndex), M.fn,
          static_cast<unsigned long long>(M.range.codeStart),
          static_cast<unsigned long long>(M.range.codeSize));
    }
    if (!transitionGlobalLayout(EJitGlobalLayoutState::Publishing,
                                EJitGlobalLayoutState::Complete)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
    publishCodePoolStats();
    // Publish-result summary: rank order was enforced during linking; the
    // verdict here additionally checks that every appended unlisted body
    // landed above the manifest region (the collective append promise).
    uintptr_t LastMember = 0;
    for (const auto &M : layoutMembers_) {
      const uintptr_t Body = reinterpret_cast<uintptr_t>(M.fn);
      if (Body > LastMember)
        LastMember = Body;
    }
    bool AppendedOrdered = true;
    for (const auto &P : layoutAppendedPublishes_)
      if (reinterpret_cast<uintptr_t>(P.fn) <= LastMember)
        AppendedOrdered = false;
    EJIT_DIAG("global layout COMPLETE gen=%u members=%u/%u "
              "unlistedAppended=%u addressOrder=%s collectionPolls=%u "
              "publishPolls=%u",
              state_->generation.loadAcquire(),
              state_->globalLayoutPublished.loadRelaxed(),
              state_->globalLayoutRequired.loadRelaxed(),
              state_->globalLayoutUnlistedAppended.loadRelaxed(),
              AppendedOrdered ? "manifest-then-appended" : "crossed",
              layoutPolls_, layoutPublishPolls_);
    for (const auto &P : layoutAppendedPublishes_)
      EJIT_DIAG("global layout appended unlisted func=%u body=%p",
                stripReqTier(P.req.funcIndex), P.fn);
    return true;
  }
  return false;
}

bool EJitSharedTaskPool::validateGlobalLayoutAttachment(
    const std::vector<EJitCompileRequest> &manifest) {
  if (!state_ || state_->initState.loadAcquire() !=
                     static_cast<uint32_t>(EJitSharedInitState::Ready))
    return true; // Initial owner validates after field initialization.
  if (globalLayoutState() == EJitGlobalLayoutState::Disabled ||
      globalLayoutState() == EJitGlobalLayoutState::Refreshing)
    return false;
  std::vector<LayoutMember> members;
  EJitLayoutBudgets limits;
  return validateLayoutManifest(manifest, limits, members);
}
