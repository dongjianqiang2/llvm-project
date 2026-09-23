//===-- EJitVpCollector.cpp - Online-PGO runtime value collector ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Implementation of the per-core K-way heavy-hitter value collector and the
//  double-buffered generation release/acquire snapshot protocol
//  (EJIT_VALUE_PROFILE.md §3). Compiled only under EJIT_SRE_PGO_VALUE_PROFILE.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreTask.h"

using namespace llvm;
using namespace llvm::ejit;

// The single process-global collector blob. EJIT_SHARED_SECTION places it in
// inter-core shared memory on a real multi-core build; on host it is ordinary
// .bss (one process already shares one address space). Own ABI identity - the
// taskpool blob is untouched. Defined inside the namespace explicitly: on
// MSVC a using-directive does not make a global-scope definition bind to the
// namespace member declared in the header.
namespace llvm {
namespace ejit {
EJIT_SHARED_SECTION EJitVpSharedState gEJitVpState;
} // namespace ejit
} // namespace llvm

//===----------------------------------------------------------------------===//
// __llvm_profile_data (__profd_) field offsets, mirroring InstrProfData.inc
// (same LLVM build -> identical layout; the existing EJitProfileMerge.cpp
// relies on the same assumption for FuncHash/NumCounters). 64-bit only, like
// the rest of the EJIT PGO runtime path.
//===----------------------------------------------------------------------===//
static_assert(sizeof(void *) == 8,
              "EJIT VP runtime profd offsets assume 64-bit targets");
namespace {
constexpr uintptr_t kProfdNameRefOff = 0;
constexpr uintptr_t kProfdNumValueSitesOff = 52; // uint16_t[IPVK_Last+1]
} // namespace

static inline uint64_t vpLoadU64(const void *p) {
  return __atomic_load_n(reinterpret_cast<const uint64_t *>(p),
                         __ATOMIC_RELAXED);
}
static inline uint16_t vpLoadU16(const void *p) {
  return __atomic_load_n(reinterpret_cast<const uint16_t *>(p),
                         __ATOMIC_RELAXED);
}

static bool ejitVpAbiValid() {
  const EJitVpSharedState &State = gEJitVpState;
  return State.magic.loadAcquire() == kEJitVpAbiMagic &&
         State.abiVersion.loadAcquire() == kEJitVpAbiVersion &&
         State.structSize.loadAcquire() == sizeof(EJitVpSharedState);
}

namespace {
constexpr uint64_t kVpGateOpen = 1u;
constexpr uint64_t kVpGateRef = 2u;
constexpr uint64_t kVpGateTransition = uint64_t(1) << 63;
constexpr uint32_t kVpRetireNormal = 0u;
constexpr uint32_t kVpRetirePreserve = 1u;
constexpr uint32_t kVpRetireDiscard = 2u;
} // namespace

static bool ejitVpSessionActive(uint64_t SessionId) {
  if (SessionId == 0)
    return false;
  for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
    const EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
    if (Slot.samplingSessionId.loadAcquire() == SessionId)
      return (Slot.gate.loadAcquire() & kVpGateOpen) != 0;
  }
  return false;
}

static EJitVpSessionSlot *ejitVpAcquireSession(uint64_t SessionId) {
  for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
    EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
    if (Slot.samplingSessionId.loadAcquire() != SessionId)
      continue;
    uint64_t Gate = Slot.gate.loadAcquire();
    do {
      if (!(Gate & kVpGateOpen) || (Gate & kVpGateTransition))
        break;
      if (Slot.gate.compareExchange(Gate, Gate + kVpGateRef)) {
        if (Slot.samplingSessionId.loadAcquire() == SessionId)
          return &Slot;
        Slot.gate.fetchSub(kVpGateRef);
        break;
      }
    } while (true);
  }
  return nullptr;
}

#ifdef EJIT_VP_BINDING_LOOKUP_TEST_HOOK
extern "C" void ejitVpBindingLookupTestHook();
#endif

static uint64_t ejitVpSessionForProfd(uintptr_t ProfdAddr) {
  for (uint32_t I = 0; I < kEJitVpMaxProfdBindings; ++I) {
    const EJitVpProfdBinding &Binding = gEJitVpState.profdBindings[I];
    for (unsigned Attempt = 0; Attempt < 3; ++Attempt) {
      const uint64_t Before = Binding.sequence.loadAcquire();
      if (Before & 1u)
        continue;
      const uintptr_t Address = Binding.profdAddr.loadAcquire();
      if (Address != ProfdAddr)
        break;
#ifdef EJIT_VP_BINDING_LOOKUP_TEST_HOOK
      ejitVpBindingLookupTestHook();
#endif
      const uint64_t SessionId = Binding.samplingSessionId.loadAcquire();
      const uint64_t After = Binding.sequence.loadAcquire();
      if (Before == After && !(After & 1u))
        return SessionId;
    }
  }
  return 0;
}

static void ejitVpPublishBinding(EJitVpProfdBinding &Binding,
                                 uint64_t SessionId, uintptr_t ProfdAddr) {
  Binding.sequence.fetchAdd(1);
  Binding.samplingSessionId.storeRelaxed(SessionId);
  Binding.profdAddr.storeRelaxed(ProfdAddr);
  Binding.sequence.fetchAdd(1);
}

static uint64_t ejitVpStorageKey(uint64_t SessionId, uint64_t FunctionIdentity,
                                 uint32_t Kind, uint32_t SiteIdx) {
  return ejitVpSiteKey(FunctionIdentity ^ (SessionId * 0x9E3779B97F4A7C15ULL),
                       Kind, SiteIdx);
}

//===----------------------------------------------------------------------===//
// Hot-path record primitive. Production sessions acquire a bounded shared
// session gate before entering the calling core's shard; the payload updates
// themselves remain relaxed operations on that core's private shard lines.
//===----------------------------------------------------------------------===//
static inline void ejitVpRecord(uint64_t value, uint64_t samplingSessionId,
                                uint64_t functionIdentity, uint32_t kind,
                                uint32_t siteIdx) {
  EJitVpSharedState &st = gEJitVpState;
  if (samplingSessionId == 0 && st.armed.loadAcquire() == 0)
    return;
  // Validate the ABI before reading session tables whose offsets changed in
  // v3. A foreign blob must never be indexed through this build's layout.
  if (!ejitVpAbiValid())
    return;
  EJitVpSessionSlot *SessionSlot = nullptr;
  if (samplingSessionId == 0)
    samplingSessionId = kEJitVpLegacySessionId;
  else {
    SessionSlot = ejitVpAcquireSession(samplingSessionId);
    if (!SessionSlot)
      return;
  }
  const uint32_t core = EJitCoreId::current();
  if (core >= st.maxCores) {
    if (SessionSlot)
      SessionSlot->gate.fetchSub(kVpGateRef);
    return;
  }

  EJitVpShard &shard = st.shards[core];
  const uint64_t key =
      ejitVpStorageKey(samplingSessionId, functionIdentity, kind, siteIdx);

  // Register in a half before writing it, then recheck generation. Once the
  // collector flips generation and observes writers[retired] == 0, no producer
  // can subsequently touch that retired payload: a late registrant sees the
  // changed generation and backs out before its first payload access.
  uint64_t generation;
  uint32_t half;
  for (;;) {
    generation = shard.generation.loadAcquire();
    half = static_cast<uint32_t>(generation & 1u);
    shard.writers[half].fetchAdd(1);
    if (shard.generation.loadAcquire() == generation)
      break;
    shard.writers[half].fetchSub(1);
  }

  EJitVpPayload &payload = shard.payload[half];
  EJitVpSite &site =
      payload.sites[key & (static_cast<uint64_t>(kEJitVpSitesPerCore) - 1u)];

  // Direct-mapped site table: a different key displaces the slot wholesale
  // (bounded approximation, documented in EJIT_VALUE_PROFILE.md §3.2).
  if (site.siteKey.loadRelaxed() != key ||
      site.samplingSessionId.loadRelaxed() != samplingSessionId ||
      site.functionIdentity.loadRelaxed() != functionIdentity ||
      site.kind.loadRelaxed() != kind ||
      site.siteIndex.loadRelaxed() != siteIdx) {
    site.siteKey.storeRelaxed(key);
    site.samplingSessionId.storeRelaxed(samplingSessionId);
    site.functionIdentity.storeRelaxed(functionIdentity);
    site.kind.storeRelaxed(kind);
    site.siteIndex.storeRelaxed(siteIdx);
    site.total.storeRelaxed(0);
    for (uint32_t i = 0; i < kEJitVpK; ++i) {
      site.cand[i].value.storeRelaxed(0);
      site.cand[i].count.storeRelaxed(0);
    }
  }

  // K-way heavy hitter: match the value, else take the first empty slot, else
  // displace the lowest-count candidate (adapts toward recent values).
  uint32_t victim = 0;
  uint64_t victimCount = ~uint64_t(0);
  bool matched = false;
  for (uint32_t i = 0; i < kEJitVpK; ++i) {
    const uint64_t v = site.cand[i].value.loadRelaxed();
    const uint64_t c = site.cand[i].count.loadRelaxed();
    if (c != 0 && v == value) {
      site.cand[i].count.fetchAddRelaxed(1);
      matched = true;
      break;
    }
    if (c < victimCount) {
      victim = i;
      victimCount = c;
    }
  }
  if (!matched) {
    site.cand[victim].value.storeRelaxed(value);
    site.cand[victim].count.storeRelaxed(1);
  }
  site.total.fetchAddRelaxed(1);
  shard.writers[half].fetchSub(1);
  if (SessionSlot)
    SessionSlot->gate.fetchSub(kVpGateRef);
}

//===----------------------------------------------------------------------===//
// LLVM InstrProfiling lowering hooks (see InstrProfiling.cpp
// lowerValueProfileInst): the flat index is indirect-call sites first, then
// memop sites - exactly the split recorded in the __profd_ NumValueSites[].
//===----------------------------------------------------------------------===//
extern "C" void __llvm_profile_instrument_target(uint64_t value, void *data,
                                                 uint32_t index) {
  if (!data)
    return;
  if (!ejitVpAbiValid())
    return;
  const uint8_t *d = static_cast<const uint8_t *>(data);
  const uint64_t nameRef = vpLoadU64(d + kProfdNameRefOff);
  uint64_t session = ejitVpSessionForProfd(reinterpret_cast<uintptr_t>(data));
  if (session == 0 && gEJitVpState.lastIssuedSessionId.loadAcquire() != 0)
    return;
  const uint32_t nsIC = vpLoadU16(d + kProfdNumValueSitesOff + 0);
  const uint32_t nsMem = vpLoadU16(d + kProfdNumValueSitesOff + 2);
  if (index < nsIC) {
    ejitVpRecord(value, session, nameRef, kEJitVpIndirectCall, index);
    return;
  }
  if (index - nsIC < nsMem) {
    ejitVpRecord(value, session, nameRef, kEJitVpMemOpSize, index - nsIC);
    return;
  }
  // Out of range (vtable sites / layout drift): drop rather than misattribute.
}

extern "C" void __llvm_profile_instrument_memop(uint64_t value, void *data,
                                                uint32_t index) {
  __llvm_profile_instrument_target(value, data, index);
}

extern "C" void ejit_vp_record_scalar(uint64_t funcHash, uint32_t siteIdx,
                                      uint64_t value) {
  ejitVpRecord(value, 0, funcHash, kEJitVpScalar, siteIdx);
}

extern "C" void ejit_vp_record_scalar_session(uint64_t samplingSessionId,
                                              uint64_t funcHash,
                                              uint32_t siteIdx,
                                              uint64_t value) {
  ejitVpRecord(value, samplingSessionId, funcHash, kEJitVpScalar, siteIdx);
}

//===----------------------------------------------------------------------===//
// Cold-path collector API (owner worker only).
//===----------------------------------------------------------------------===//

bool ejit::ejitVpEnsureInitialized() {
  EJitVpSharedState &st = gEJitVpState;
  if (st.magic.loadAcquire() == kEJitVpAbiMagic)
    return st.abiVersion.loadAcquire() == kEJitVpAbiVersion &&
           st.structSize.loadAcquire() == sizeof(EJitVpSharedState);

  // Field-initialize the zero-filled blob. Every racing initializer writes the
  // same constants, so this is idempotent; producers never touch the blob
  // before it is armed (armed starts 0).
  st.headerReserved = 0;
  st.armed.storeRelease(0);
  st.headerAlignPad = 0;
  st.lastIssuedSessionId.storeRelaxed(0);
  st.shardStride = static_cast<uint32_t>(sizeof(EJitVpShard));
  st.sitesPerCore = kEJitVpSitesPerCore;
  st.k = kEJitVpK;
  st.maxCores = kEJitVpMaxCores;
  st.drainTicks = kEJitVpDrainTicks;
  for (uint32_t i = 0; i < 3; ++i)
    st.headerPad[i] = 0;
  for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
    st.sessions[I].samplingSessionId.storeRelaxed(0);
    st.sessions[I].requestAttemptToken.storeRelaxed(0);
    st.sessions[I].gate.storeRelaxed(0);
    st.sessions[I].retireState.storeRelaxed(kVpRetireNormal);
    st.sessions[I].reserved = 0;
  }
  for (uint32_t I = 0; I < kEJitVpMaxProfdBindings; ++I) {
    st.profdBindings[I].sequence.storeRelaxed(0);
    st.profdBindings[I].samplingSessionId.storeRelaxed(0);
    st.profdBindings[I].profdAddr.storeRelaxed(0);
  }
  for (uint32_t c = 0; c < kEJitVpMaxCores; ++c) {
    st.shards[c].generation.storeRelaxed(0);
    st.shards[c].writers[0].storeRelaxed(0);
    st.shards[c].writers[1].storeRelaxed(0);
    for (uint32_t h = 0; h < 2; ++h)
      for (uint32_t s = 0; s < kEJitVpSitesPerCore; ++s) {
        EJitVpSite &site = st.shards[c].payload[h].sites[s];
        site.siteKey.storeRelaxed(0);
        site.samplingSessionId.storeRelaxed(0);
        site.functionIdentity.storeRelaxed(0);
        site.kind.storeRelaxed(0);
        site.siteIndex.storeRelaxed(0);
        site.total.storeRelaxed(0);
        for (uint32_t i = 0; i < kEJitVpK; ++i) {
          site.cand[i].value.storeRelaxed(0);
          site.cand[i].count.storeRelaxed(0);
        }
      }
  }
  st.structSize.storeRelaxed(static_cast<uint32_t>(sizeof(EJitVpSharedState)));
  st.abiVersion.storeRelaxed(kEJitVpAbiVersion);
  // Zero the stats (single-writer counters) for a deterministic first round.
  st.stats.merges.storeRelaxed(0);
  st.stats.icValueSites.storeRelaxed(0);
  st.stats.memopValueSites.storeRelaxed(0);
  st.stats.scalarValueSites.storeRelaxed(0);
  st.stats.scalarDropped.storeRelaxed(0);
  st.stats.scalarSpecialized.storeRelaxed(0);
  // The magic store publishes the whole header (release); a peer pairs it with
  // the armed acquire gate in the record path (happens-before via setArmed).
  st.magic.storeRelease(kEJitVpAbiMagic);
  return true;
}

void ejit::ejitVpSetArmed(bool armed) {
  if (!ejitVpEnsureInitialized())
    return;
  gEJitVpState.armed.storeRelease(armed ? 1u : 0u);
}

bool ejit::ejitVpIsArmed() {
  return ejitVpEnsureInitialized() && gEJitVpState.armed.loadAcquire() != 0;
}

static bool ejitVpAllocateSessionSlot(uint64_t SessionId,
                                      uint64_t RequestAttemptToken = 0) {
  for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
    EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
    if (Slot.samplingSessionId.loadAcquire() != 0)
      continue;
    // Claim the free slot with a closed reference. A concurrent stale cancel
    // may also transiently reference a free slot, so zero ID alone is not a
    // sufficient reuse condition.
    uint64_t FreeGate = 0;
    if (!Slot.gate.compareExchange(FreeGate, kVpGateTransition))
      continue;
    Slot.retireState.storeRelaxed(kVpRetireNormal);
    Slot.requestAttemptToken.storeRelaxed(RequestAttemptToken);
    Slot.samplingSessionId.storeRelease(SessionId);
    Slot.gate.storeRelease(kVpGateOpen);
    return true;
  }
  return false;
}

bool ejit::ejitVpBeginSession(uint64_t SessionId) {
  if (!SessionId || SessionId == kEJitVpLegacySessionId ||
      !ejitVpEnsureInitialized())
    return false;
  uint64_t Last = gEJitVpState.lastIssuedSessionId.loadAcquire();
  do {
    if (SessionId <= Last)
      return false;
  } while (!gEJitVpState.lastIssuedSessionId.compareExchange(Last, SessionId));
  return ejitVpAllocateSessionSlot(SessionId);
}

uint64_t ejit::ejitVpCreateSession(uint64_t RequestAttemptToken) {
  if (!ejitVpEnsureInitialized())
    return 0;
  const uint64_t SessionId = gEJitVpState.lastIssuedSessionId.fetchAdd(1) + 1;
  if (SessionId == 0 || SessionId == kEJitVpLegacySessionId)
    return 0;
  if (ejitVpAllocateSessionSlot(SessionId, RequestAttemptToken))
    return SessionId;

  // Capacity pressure services failed cancellation/abort retirements. A T2
  // freeze that owns partial samples marks its slot retained and is never
  // scavenged here.
  for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
    EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
    const uint64_t OldId = Slot.samplingSessionId.loadAcquire();
    if (!OldId || (Slot.gate.loadAcquire() & kVpGateOpen) != 0 ||
        Slot.retireState.loadAcquire() == kVpRetirePreserve)
      continue;
    std::vector<EJitVpSiteSample> Discarded;
    (void)ejitVpTakeSessionSnapshot(OldId, Discarded);
    if (ejitVpAllocateSessionSlot(SessionId, RequestAttemptToken))
      return SessionId;
  }
  return 0;
}

static bool ejitVpPinSessionSlot(EJitVpSessionSlot &Slot) {
  uint64_t Gate = Slot.gate.loadAcquire();
  do {
    if (Gate & kVpGateTransition)
      return false;
  } while (!Slot.gate.compareExchange(Gate, Gate + kVpGateRef));
  return true;
}

static void ejitVpCloseSessionGate(EJitVpSessionSlot &Slot) {
  uint64_t Gate = Slot.gate.loadAcquire();
  while ((Gate & kVpGateOpen) &&
         !Slot.gate.compareExchange(Gate, Gate & ~kVpGateOpen)) {
  }
}

void ejit::ejitVpEndSession(uint64_t SessionId, bool PreserveForRetry) {
  if (!SessionId || !ejitVpEnsureInitialized())
    return;
  for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
    EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
    if (Slot.samplingSessionId.loadAcquire() != SessionId ||
        !ejitVpPinSessionSlot(Slot))
      continue;
    if (Slot.samplingSessionId.loadAcquire() == SessionId) {
      if (PreserveForRetry) {
        uint32_t State = kVpRetireNormal;
        (void)Slot.retireState.compareExchange(State, kVpRetirePreserve);
      } else {
        Slot.retireState.storeRelease(kVpRetireDiscard);
      }
      ejitVpCloseSessionGate(Slot);
      Slot.gate.fetchSub(kVpGateRef);
      return;
    }
    Slot.gate.fetchSub(kVpGateRef);
  }
}

#ifdef EJIT_VP_CANCEL_TEST_HOOK
extern "C" void ejitVpCancelTestHook();
#endif

void ejit::ejitVpCancelAttempt(uint64_t RequestAttemptToken) {
  if (!RequestAttemptToken || !ejitVpEnsureInitialized())
    return;
  for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
    EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
    if (Slot.requestAttemptToken.loadAcquire() != RequestAttemptToken)
      continue;
#ifdef EJIT_VP_CANCEL_TEST_HOOK
    ejitVpCancelTestHook();
#endif

    // Hold a closed or active slot across the token recheck. Exclusive
    // retirement and allocation both reject a live reference.
    if (!ejitVpPinSessionSlot(Slot))
      continue;
    if (Slot.requestAttemptToken.loadAcquire() == RequestAttemptToken) {
      Slot.retireState.storeRelease(kVpRetireDiscard);
      ejitVpCloseSessionGate(Slot);
      Slot.gate.fetchSub(kVpGateRef);
      return;
    }
    Slot.gate.fetchSub(kVpGateRef);
  }
}

bool ejit::ejitVpSessionDiscarded(uint64_t SessionId) {
  if (!SessionId || !ejitVpEnsureInitialized())
    return true;
  for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
    const EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
    if (Slot.samplingSessionId.loadAcquire() == SessionId)
      return Slot.retireState.loadAcquire() == kVpRetireDiscard;
  }
  // A driver-side partial accumulator for an ID no longer present in the
  // bounded registry has no remaining retry owner and is safe to erase.
  return true;
}

bool ejit::ejitVpBindProfileData(uint64_t SessionId, uintptr_t ProfdAddr) {
  if (!SessionId || !ProfdAddr || !ejitVpSessionActive(SessionId))
    return false;
  for (uint32_t I = 0; I < kEJitVpMaxProfdBindings; ++I) {
    EJitVpProfdBinding &Binding = gEJitVpState.profdBindings[I];
    const uintptr_t Existing = Binding.profdAddr.loadAcquire();
    if (Existing == ProfdAddr)
      return Binding.samplingSessionId.loadAcquire() == SessionId;
    if (Existing == 0) {
      ejitVpPublishBinding(Binding, SessionId, ProfdAddr);
      return true;
    }
  }
  return false;
}

void ejit::ejitVpResetFunction(uint64_t NameHash,
                               ArrayRef<EJitVpKindSiteCount> SiteCounts) {
  if (!ejitVpEnsureInitialized())
    return;
  for (uint32_t C = 0; C < kEJitVpMaxCores; ++C)
    for (uint32_t H = 0; H < 2; ++H)
      for (uint32_t S = 0; S < kEJitVpSitesPerCore; ++S) {
        EJitVpSite &Site = gEJitVpState.shards[C].payload[H].sites[S];
        if (Site.functionIdentity.loadRelaxed() != NameHash)
          continue;
        bool Matches = false;
        for (const EJitVpKindSiteCount &Count : SiteCounts)
          if (Site.kind.loadRelaxed() == Count.kind &&
              Site.siteIndex.loadRelaxed() < Count.count) {
            Matches = true;
            break;
          }
        if (!Matches)
          continue;
        Site.siteKey.storeRelaxed(0);
        Site.samplingSessionId.storeRelaxed(0);
        Site.functionIdentity.storeRelaxed(0);
        Site.kind.storeRelaxed(0);
        Site.siteIndex.storeRelaxed(0);
        Site.total.storeRelaxed(0);
        for (uint32_t I = 0; I < kEJitVpK; ++I) {
          Site.cand[I].value.storeRelaxed(0);
          Site.cand[I].count.storeRelaxed(0);
        }
      }
}

//===----------------------------------------------------------------------===//
// Snapshot: prepare the inactive half, flip generation, drain, then copy only
// retired halves with no registered writers.
//===----------------------------------------------------------------------===//
static void ejitVpDrain() {
  for (uint32_t i = 0; i < kEJitVpDrainTicks; ++i)
    EJitSreTask::yield();
}

static void ejitVpCollectAndClear(EJitVpPayload &payload,
                                  std::vector<EJitVpSiteSample> &out,
                                  uint64_t onlySession = 0) {
  for (uint32_t s = 0; s < kEJitVpSitesPerCore; ++s) {
    EJitVpSite &site = payload.sites[s];
    const uint64_t key = site.siteKey.loadRelaxed();
    if (key == 0)
      continue;
    const uint64_t SessionId = site.samplingSessionId.loadRelaxed();
    if (onlySession != 0 && SessionId != onlySession)
      continue;

    EJitVpSiteSample sample;
    sample.siteKey =
        ejitVpSiteKey(site.functionIdentity.loadRelaxed(),
                      site.kind.loadRelaxed(), site.siteIndex.loadRelaxed());
    sample.samplingSessionId = SessionId;
    sample.total = site.total.loadRelaxed();
    for (uint32_t i = 0; i < kEJitVpK; ++i) {
      sample.values[i] = site.cand[i].value.loadRelaxed();
      sample.counts[i] = site.cand[i].count.loadRelaxed();
    }

    site.siteKey.storeRelaxed(0);
    site.samplingSessionId.storeRelaxed(0);
    site.functionIdentity.storeRelaxed(0);
    site.kind.storeRelaxed(0);
    site.siteIndex.storeRelaxed(0);
    site.total.storeRelaxed(0);
    for (uint32_t i = 0; i < kEJitVpK; ++i) {
      site.cand[i].value.storeRelaxed(0);
      site.cand[i].count.storeRelaxed(0);
    }

    bool any = false;
    for (uint32_t i = 0; i < kEJitVpK; ++i)
      any |= sample.counts[i] != 0;
    if (any)
      out.push_back(sample);
  }
}

static bool ejitVpTakeSnapshotImpl(uint64_t OnlySession,
                                   std::vector<EJitVpSiteSample> &out) {
  if (!ejitVpEnsureInitialized())
    return false;
  bool Complete = true;

  // Flip every core's generation: producers observing the new value write the
  // OTHER half from now on. fetchAdd(1) = release on the flip, acquire of prior
  // producer stores on this same word order chain. The writer handshake below
  // establishes when a retired payload is immutable.
  uint64_t retired[kEJitVpMaxCores];
  for (uint32_t c = 0; c < kEJitVpMaxCores; ++c) {
    EJitVpShard &shard = gEJitVpState.shards[c];
    const uint64_t generation = shard.generation.loadAcquire();
    const uint32_t next = static_cast<uint32_t>((generation + 1u) & 1u);

    // A half skipped in the previous snapshot may still contain old data.
    // Recover it after every late writer has left, then clear it before the
    // generation flip makes it visible to producers.
    if (shard.writers[next].loadAcquire() != 0) {
      retired[c] = 2u;
      Complete = false;
      continue;
    }
    ejitVpCollectAndClear(shard.payload[next], out, OnlySession);
    retired[c] = shard.generation.fetchAdd(1) & 1u;
  }

  // Shrink the straggler window: a producer that loaded the old generation
  // before the flip has a few stores left; yield so they land before the copy.
  ejitVpDrain();

  for (uint32_t c = 0; c < kEJitVpMaxCores; ++c) {
    if (retired[c] > 1u)
      continue;
    EJitVpShard &shard = gEJitVpState.shards[c];
    if (shard.writers[retired[c]].loadAcquire() != 0) {
      Complete = false;
      continue;
    }
    ejitVpCollectAndClear(shard.payload[retired[c]], out, OnlySession);
  }
  EJitVpSessionSlot *RetiringSlot = nullptr;
  if (Complete && OnlySession != 0 && OnlySession != kEJitVpLegacySessionId) {
    for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
      EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
      if (Slot.samplingSessionId.loadAcquire() != OnlySession)
        continue;
      uint64_t FreeGate = 0;
      if (!Slot.gate.compareExchange(FreeGate, kVpGateTransition))
        Complete = false;
      else if (Slot.samplingSessionId.loadAcquire() == OnlySession)
        RetiringSlot = &Slot;
      else
        Slot.gate.storeRelease(0);
      break;
    }
  }
  if (Complete && OnlySession != 0 && OnlySession != kEJitVpLegacySessionId) {
    for (uint32_t I = 0; I < kEJitVpMaxProfdBindings; ++I) {
      EJitVpProfdBinding &Binding = gEJitVpState.profdBindings[I];
      if (Binding.samplingSessionId.loadAcquire() != OnlySession)
        continue;
      ejitVpPublishBinding(Binding, 0, 0);
    }
    if (RetiringSlot) {
      RetiringSlot->retireState.storeRelaxed(kVpRetireNormal);
      RetiringSlot->requestAttemptToken.storeRelaxed(0);
      RetiringSlot->samplingSessionId.storeRelease(0);
      RetiringSlot->gate.storeRelease(0);
    }
  }
  return Complete;
}

bool ejit::ejitVpTakeSnapshot(std::vector<EJitVpSiteSample> &out) {
  return ejitVpTakeSnapshotImpl(0, out);
}

bool ejit::ejitVpTakeSessionSnapshot(uint64_t SamplingSessionId,
                                     std::vector<EJitVpSiteSample> &out) {
  if (!SamplingSessionId)
    return false;
  if (SamplingSessionId != kEJitVpLegacySessionId)
    for (uint32_t I = 0; I < kEJitVpMaxSessions; ++I) {
      EJitVpSessionSlot &Slot = gEJitVpState.sessions[I];
      if (Slot.samplingSessionId.loadAcquire() == SamplingSessionId &&
          ejitVpPinSessionSlot(Slot)) {
        if (Slot.samplingSessionId.loadAcquire() == SamplingSessionId)
          ejitVpCloseSessionGate(Slot);
        Slot.gate.fetchSub(kVpGateRef);
        break;
      }
    }
  return ejitVpTakeSnapshotImpl(SamplingSessionId, out);
}

void ejit::ejitVpBumpMergeCounts(uint64_t icSites, uint64_t memopSites,
                                 uint64_t scalarSites, uint64_t scalarDropped) {
  if (!ejitVpEnsureInitialized())
    return;
  gEJitVpState.stats.merges.fetchAddRelaxed(1);
  gEJitVpState.stats.icValueSites.fetchAddRelaxed(icSites);
  gEJitVpState.stats.memopValueSites.fetchAddRelaxed(memopSites);
  gEJitVpState.stats.scalarValueSites.fetchAddRelaxed(scalarSites);
  gEJitVpState.stats.scalarDropped.fetchAddRelaxed(scalarDropped);
}

void ejit::ejitVpBumpScalarSpecialized(uint64_t n) {
  if (!ejitVpEnsureInitialized())
    return;
  gEJitVpState.stats.scalarSpecialized.fetchAddRelaxed(n);
}

void ejit::ejitVpStatsSnapshot(EJitVpStatsOut &out) {
  if (!ejitVpEnsureInitialized())
    return;
  out.merges = gEJitVpState.stats.merges.loadRelaxed();
  out.icValueSites = gEJitVpState.stats.icValueSites.loadRelaxed();
  out.memopValueSites = gEJitVpState.stats.memopValueSites.loadRelaxed();
  out.scalarValueSites = gEJitVpState.stats.scalarValueSites.loadRelaxed();
  out.scalarDropped = gEJitVpState.stats.scalarDropped.loadRelaxed();
  out.scalarSpecialized = gEJitVpState.stats.scalarSpecialized.loadRelaxed();
}
