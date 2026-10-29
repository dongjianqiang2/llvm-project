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

//===----------------------------------------------------------------------===//
// Hot-path record primitive. After the armed gate (one acquire load) and the
// ABI gate (three loads of the same shared line), every access below is a
// RELAXED RMW on the CALLING core's private shard line: no CAS, no lock, no
// cross-core write to a shared cache line.
//===----------------------------------------------------------------------===//
static inline void ejitVpRecord(uint64_t value, uint64_t functionIdentity,
                                uint32_t kind, uint32_t siteIdx) {
  EJitVpSharedState &st = gEJitVpState;
  if (st.armed.loadAcquire() == 0)
    return;
  // ABI validation on the hot path is three acquire loads of the header line
  // (already shared for the armed read above); a mismatched/foreign blob must
  // never be written through an out-of-bounds layout.
  if (st.magic.loadAcquire() != kEJitVpAbiMagic ||
      st.abiVersion.loadAcquire() != kEJitVpAbiVersion ||
      st.structSize.loadAcquire() != sizeof(EJitVpSharedState))
    return;
  const uint32_t core = EJitCoreId::current();
  if (core >= st.maxCores) {
    if (st.losslessMode.loadAcquire())
      st.losslessFailed.storeRelease(1);
    return;
  }

  EJitVpShard &shard = st.shards[core];
  const uint64_t key = ejitVpSiteKey(functionIdentity, kind, siteIdx);

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
  const bool Lossless = st.losslessMode.loadAcquire() != 0;
  EJitVpSite *Selected = nullptr;
  if (Lossless) {
    uint32_t expected = 0;
    if (!payload.updateBusy.compareExchange(expected, 1)) {
      // A preempting same-core writer must not wait for the interrupted writer.
      // Latch an unusable round instead of silently publishing missing data.
      st.losslessFailed.storeRelease(1);
      shard.writers[half].fetchSub(1);
      return;
    }
    const uint64_t Mask = static_cast<uint64_t>(kEJitVpSitesPerCore) - 1u;
    for (uint32_t Probe = 0; Probe < kEJitVpSitesPerCore; ++Probe) {
      auto &Candidate = payload.sites[(key + Probe) & Mask];
      uint64_t Existing = Candidate.siteKey.loadAcquire();
      if (Existing == key || Existing == 0) {
        Selected = &Candidate;
        break;
      }
    }
    if (!Selected) {
      st.losslessFailed.storeRelease(1);
      payload.updateBusy.storeRelease(0);
      shard.writers[half].fetchSub(1);
      return;
    }
  } else {
    Selected =
        &payload.sites[key & (static_cast<uint64_t>(kEJitVpSitesPerCore) - 1u)];
  }
  EJitVpSite &site = *Selected;

  // Direct-mapped site table: a different key displaces the slot wholesale
  // (bounded approximation, documented in EJIT_VALUE_PROFILE.md §3.2).
  if (site.siteKey.loadRelaxed() != key) {
    site.siteKey.storeRelaxed(key);
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
  if (Lossless)
    payload.updateBusy.storeRelease(0);
  shard.writers[half].fetchSub(1);
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
  const uint8_t *d = static_cast<const uint8_t *>(data);
  const uint64_t nameRef = vpLoadU64(d + kProfdNameRefOff);
  const uint32_t nsIC = vpLoadU16(d + kProfdNumValueSitesOff + 0);
  const uint32_t nsMem = vpLoadU16(d + kProfdNumValueSitesOff + 2);
  if (index < nsIC) {
    ejitVpRecord(value, nameRef, kEJitVpIndirectCall, index);
    return;
  }
  if (index - nsIC < nsMem) {
    ejitVpRecord(value, nameRef, kEJitVpMemOpSize, index - nsIC);
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
  ejitVpRecord(value, funcHash, kEJitVpScalar, siteIdx);
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
  st.losslessMode.storeRelaxed(0);
  st.losslessFailed.storeRelaxed(0);
  st.armed.storeRelease(0);
  st.shardStride = static_cast<uint32_t>(sizeof(EJitVpShard));
  st.sitesPerCore = kEJitVpSitesPerCore;
  st.k = kEJitVpK;
  st.maxCores = kEJitVpMaxCores;
  st.drainTicks = kEJitVpDrainTicks;
  for (uint32_t i = 0; i < 2; ++i)
    st.headerPad[i] = 0;
  for (uint32_t c = 0; c < kEJitVpMaxCores; ++c) {
    st.shards[c].generation.storeRelaxed(0);
    st.shards[c].writers[0].storeRelaxed(0);
    st.shards[c].writers[1].storeRelaxed(0);
    for (uint32_t h = 0; h < 2; ++h) {
      st.shards[c].payload[h].updateBusy.storeRelaxed(0);
      for (uint32_t s = 0; s < kEJitVpSitesPerCore; ++s) {
        EJitVpSite &site = st.shards[c].payload[h].sites[s];
        site.siteKey.storeRelaxed(0);
        site.total.storeRelaxed(0);
        for (uint32_t i = 0; i < kEJitVpK; ++i) {
          site.cand[i].value.storeRelaxed(0);
          site.cand[i].count.storeRelaxed(0);
        }
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

void ejit::ejitVpResetFunction(uint64_t nameHash,
                               ArrayRef<EJitVpKindSiteCount> siteCounts) {
  if (!ejitVpEnsureInitialized())
    return;
  // Bound the total site count defensively: the direct-mapped table only has
  // kEJitVpSitesPerCore slots per core, and probing more keys than slots would
  // simply revisit the same slots (each extra round zeroes a displaced key's
  // slot too - still safe). Clamp to a sane upper bound.
  for (uint32_t c = 0; c < kEJitVpMaxCores; ++c) {
    for (uint32_t h = 0; h < 2; ++h) {
      for (const EJitVpKindSiteCount &sc : siteCounts) {
        if (sc.kind >= kEJitVpKindCount || sc.count == 0)
          continue;
        for (uint32_t idx = 0; idx < sc.count; ++idx) {
          const uint64_t key = ejitVpSiteKey(nameHash, sc.kind, idx);
          const bool Lossless = gEJitVpState.losslessMode.loadAcquire() != 0;
          const uint32_t Probes = Lossless ? kEJitVpSitesPerCore : 1;
          for (uint32_t Probe = 0; Probe < Probes; ++Probe) {
            auto &site =
                gEJitVpState.shards[c]
                    .payload[h]
                    .sites[(key + Probe) &
                           (static_cast<uint64_t>(kEJitVpSitesPerCore) - 1u)];
            if (site.siteKey.loadRelaxed() != key)
              continue;
            site.total.storeRelaxed(0);
            for (uint32_t i = 0; i < kEJitVpK; ++i) {
              site.cand[i].value.storeRelaxed(0);
              site.cand[i].count.storeRelaxed(0);
            }
            // Publish emptiness LAST: a sibling's different key may reuse
            // this slot as soon as it observes zero.
            site.siteKey.storeRelease(0);
            break;
          }
        }
      }
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
                                  std::vector<EJitVpSiteSample> &out) {
  for (uint32_t s = 0; s < kEJitVpSitesPerCore; ++s) {
    EJitVpSite &site = payload.sites[s];
    const uint64_t key = site.siteKey.loadRelaxed();
    if (key == 0)
      continue;

    EJitVpSiteSample sample;
    sample.siteKey = key;
    sample.total = site.total.loadRelaxed();
    for (uint32_t i = 0; i < kEJitVpK; ++i) {
      sample.values[i] = site.cand[i].value.loadRelaxed();
      sample.counts[i] = site.cand[i].count.loadRelaxed();
    }

    site.siteKey.storeRelaxed(0);
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

bool ejit::ejitVpTakeSnapshot(std::vector<EJitVpSiteSample> &out) {
  if (!ejitVpEnsureInitialized())
    return false;

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
      continue;
    }
    ejitVpCollectAndClear(shard.payload[next], out);
    retired[c] = shard.generation.fetchAdd(1) & 1u;
  }

  // Shrink the straggler window: a producer that loaded the old generation
  // before the flip has a few stores left; yield so they land before the copy.
  ejitVpDrain();

  for (uint32_t c = 0; c < kEJitVpMaxCores; ++c) {
    if (retired[c] > 1u)
      continue;
    EJitVpShard &shard = gEJitVpState.shards[c];
    if (shard.writers[retired[c]].loadAcquire() != 0)
      continue;
    ejitVpCollectAndClear(shard.payload[retired[c]], out);
  }
  return true;
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

void ejit::ejitVpSetLosslessMode(bool enabled) {
  if (!ejitVpEnsureInitialized())
    return;
  gEJitVpState.losslessFailed.storeRelaxed(0);
  gEJitVpState.losslessMode.storeRelease(enabled ? 1 : 0);
}

bool ejit::ejitVpLosslessFailed() {
  return gEJitVpState.losslessFailed.loadAcquire() != 0;
}

bool ejit::ejitVpResetDrained() {
  if (!ejitVpEnsureInitialized())
    return false;
  ejitVpSetArmed(false);
  for (const auto &S : gEJitVpState.shards)
    if (S.writers[0].loadAcquire() || S.writers[1].loadAcquire())
      return false;
  for (auto &S : gEJitVpState.shards)
    for (auto &P : S.payload) {
      P.updateBusy.storeRelaxed(0);
      for (auto &Site : P.sites) {
        Site.total.storeRelaxed(0);
        for (auto &C : Site.cand) {
          C.value.storeRelaxed(0);
          C.count.storeRelaxed(0);
        }
        Site.siteKey.storeRelease(0);
      }
    }
  gEJitVpState.losslessFailed.storeRelease(0);
  return true;
}
