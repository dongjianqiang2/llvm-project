//===-- EJitVerify.cpp - may_const substitution verifier ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runtime half of Config::verifySubstitution. See EJitVerify.h.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitVerify.h"

#ifdef EJIT_VERIFY_SUBSTITUTION

#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"

using namespace llvm::ejit;

namespace {

// The verifier is diagnostic-only but is called by the compile worker and by
// arbitrary workload cores. A plain atomic counter cannot cross core-private
// BSS, so the complete store is placed with the same platform seam as the
// shared taskpool blob. The default host build leaves the attribute empty and
// gets one process-wide object, which is the intended host simulation.
constexpr uint64_t kMaxReportedMismatches = 32;

enum : uint32_t { kSlotEmpty = 0, kSlotFilling = 1, kSlotReady = 2 };

struct SiteRecord {
  EJitAtomicU32 state;
  EJitAtomicU64 identityId;
  EJitAtomicU32 identityLength;
  EJitAtomicU64 checks;
  EJitAtomicU64 mismatches;
  EJitAtomicU64 lastFrozen;
  EJitAtomicU64 lastActual;
  // Full structural key when it fits. This is not the bounded display name.
  char identity[kVerifySiteIdentityMax];
  char name[kVerifySiteNameMax];
};

struct VerifySharedState {
  // Shared reader/writer protocol. Readers cover a complete check, query, or
  // emission note. Reset takes the writer side and waits for all prior users,
  // so no SiteRecord pointer can outlive a row retirement.
  EJitAtomicU32 writeFlag;
  EJitAtomicU32 readers;
  // Record creation is a short critical section nested inside a read epoch.
  EJitAtomicU32 tableLock;

  EJitAtomicU64 sites;
  EJitAtomicU64 checks;
  EJitAtomicU64 mismatches;
  SiteRecord siteTable[kVerifyMaxSites];
  EJitAtomicU32 siteTableUsed;
};

EJIT_SHARED_SECTION VerifySharedState gVerifyState;

void beginRead() {
  for (;;) {
    while (gVerifyState.writeFlag.loadAcquire() != 0) {
    }
    gVerifyState.readers.fetchAdd(1);
    if (gVerifyState.writeFlag.loadAcquire() == 0)
      return;
    gVerifyState.readers.fetchSub(1);
  }
}

void endRead() { gVerifyState.readers.fetchSub(1); }

void beginWrite() {
  uint32_t expected = 0;
  while (!gVerifyState.writeFlag.compareExchange(expected, 1))
    expected = 0;
  while (gVerifyState.readers.loadAcquire() != 0) {
  }
}

void endWrite() { gVerifyState.writeFlag.storeRelease(0); }

void lockTable() {
  uint32_t expected = 0;
  while (!gVerifyState.tableLock.compareExchange(expected, 1))
    expected = 0;
}

void unlockTable() { gVerifyState.tableLock.storeRelease(0); }

bool boundedLength(const char *src, size_t cap, uint32_t &length) {
  if (!src)
    return false;
  for (size_t i = 0; i < cap; ++i) {
    if (src[i] == '\0') {
      length = static_cast<uint32_t>(i);
      return true;
    }
  }
  return false;
}

void copyBounded(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0;
  for (; i + 1 < cap && src[i] != '\0'; ++i)
    dst[i] = src[i];
  dst[i] = '\0';
}

bool identityEquals(const SiteRecord &record, const char *identity,
                    uint64_t identityId) {
  const uint64_t storedId = record.identityId.loadAcquire();
  if (identityId != 0)
    return storedId == identityId;
  if (storedId != 0)
    return false;

  uint32_t length = 0;
  if (!boundedLength(identity, kVerifySiteIdentityMax, length))
    return false;
  if (record.identityLength.loadAcquire() != length)
    return false;
  for (uint32_t i = 0; i < length; ++i)
    if (record.identity[i] != identity[i])
      return false;
  return true;
}

/// Find or append a record while the caller holds a read epoch. The table lock
/// removes duplicate rows for concurrent first execution of one identity.
SiteRecord *lookupSite(const char *site, const char *identity,
                       uint64_t identityId) {
  if (!identity)
    identity = site;

  uint32_t identityLength = 0;
  if (identityId == 0 &&
      !boundedLength(identity, kVerifySiteIdentityMax, identityLength))
    return nullptr;

  lockTable();
  const uint32_t used = gVerifyState.siteTableUsed.loadRelaxed();
  for (uint32_t i = 0; i < used && i < kVerifyMaxSites; ++i) {
    SiteRecord &record = gVerifyState.siteTable[i];
    if (record.state.loadAcquire() == kSlotReady &&
        identityEquals(record, identity, identityId)) {
      unlockTable();
      return &record;
    }
  }

  if (used >= kVerifyMaxSites) {
    unlockTable();
    return nullptr;
  }

  SiteRecord &record = gVerifyState.siteTable[used];
  record.state.storeRelease(kSlotFilling);
  record.identityId.storeRelaxed(identityId);
  record.identityLength.storeRelaxed(identityId == 0 ? identityLength : 0);
  if (identityId == 0)
    copyBounded(record.identity, sizeof(record.identity), identity);
  else
    record.identity[0] = '\0';
  copyBounded(record.name, sizeof(record.name), site);
  record.checks.storeRelaxed(0);
  record.mismatches.storeRelaxed(0);
  record.lastFrozen.storeRelaxed(0);
  record.lastActual.storeRelaxed(0);
  record.state.storeRelease(kSlotReady);
  gVerifyState.siteTableUsed.storeRelease(used + 1);
  unlockTable();
  return &record;
}

uint32_t siteCountUnlocked() {
  const uint32_t used = gVerifyState.siteTableUsed.loadAcquire();
  return used > kVerifyMaxSites ? kVerifyMaxSites : used;
}

} // namespace

void llvm::ejit::ejitVerifyGetStats(VerifyStats *out) {
  if (!out)
    return;
  beginRead();
  out->sites = gVerifyState.sites.loadRelaxed();
  out->checks = gVerifyState.checks.loadRelaxed();
  out->mismatches = gVerifyState.mismatches.loadRelaxed();
  endRead();
}

size_t llvm::ejit::ejitVerifySiteCount() {
  beginRead();
  const size_t count = siteCountUnlocked();
  endRead();
  return count;
}

bool llvm::ejit::ejitVerifyGetSite(size_t index, VerifySite *out) {
  if (!out)
    return false;
  beginRead();
  const uint32_t count = siteCountUnlocked();
  if (index >= count) {
    endRead();
    return false;
  }

  SiteRecord &record = gVerifyState.siteTable[index];
  if (record.state.loadAcquire() != kSlotReady) {
    endRead();
    return false;
  }
  copyBounded(out->site, sizeof(out->site), record.name);
  out->checks = record.checks.loadRelaxed();
  out->mismatches = record.mismatches.loadRelaxed();
  out->lastFrozen = record.lastFrozen.loadRelaxed();
  out->lastActual = record.lastActual.loadRelaxed();
  endRead();
  return true;
}

void llvm::ejit::ejitVerifyResetStats() {
  beginWrite();
  gVerifyState.sites.storeRelaxed(0);
  gVerifyState.checks.storeRelaxed(0);
  gVerifyState.mismatches.storeRelaxed(0);
  for (size_t i = 0; i < kVerifyMaxSites; ++i)
    gVerifyState.siteTable[i].state.storeRelease(kSlotEmpty);
  gVerifyState.siteTableUsed.storeRelease(0);
  endWrite();
}

void llvm::ejit::ejitVerifyNoteSite() {
  beginRead();
  gVerifyState.sites.fetchAdd(1);
  endRead();
}

extern "C" void __ejit_verify_check(const char *site, const char *identity,
                                    uint64_t identityId, uint64_t baked,
                                    uint64_t actual) {
  beginRead();
  gVerifyState.checks.fetchAdd(1);

  SiteRecord *record =
      site ? lookupSite(site, identity, identityId) : nullptr;
  if (record)
    record->checks.fetchAdd(1);

  if (baked == actual) {
    endRead();
    return;
  }

  if (record) {
    record->mismatches.fetchAdd(1);
    record->lastFrozen.storeRelaxed(baked);
    record->lastActual.storeRelaxed(actual);
  }

  // fetchAdd returns the PRE-increment value, so the first mismatch sees 0.
  const uint64_t seen = gVerifyState.mismatches.fetchAdd(1);
  if (seen < kMaxReportedMismatches) {
    EJIT_DIAG("verify MISMATCH site=%s id=0x%llx frozen=0x%llx actual=0x%llx",
              site ? site : "(unnamed)",
              static_cast<unsigned long long>(identityId),
              static_cast<unsigned long long>(baked),
              static_cast<unsigned long long>(actual));
    if (seen + 1 == kMaxReportedMismatches)
      EJIT_DIAG("verify: %llu mismatches reported, further ones counted only",
                static_cast<unsigned long long>(kMaxReportedMismatches));
  }
  endRead();
}

#endif // EJIT_VERIFY_SUBSTITUTION
