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
// Compiled out entirely without EJIT_VERIFY_SUBSTITUTION: the translation unit
// is empty, and nothing that calls into it is built either.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitVerify.h"

#ifdef EJIT_VERIFY_SUBSTITUTION

#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"

using namespace llvm::ejit;

namespace {

EJitAtomicU64 gSites;
EJitAtomicU64 gChecks;
EJitAtomicU64 gMismatches;

// A diverging field usually diverges on every call, so logging each one buries
// the run. Report the first few, then count silently — the per-site table is
// what the classification is read from.
constexpr uint64_t kMaxReportedMismatches = 32;

/// A record is readable only at Ready: the index is handed out before the name
/// is written, so a reader must not look at one still being filled in.
enum : uint32_t { kSlotEmpty = 0, kSlotFilling = 1, kSlotReady = 2 };

struct SiteRecord {
  EJitAtomicU32 state;
  EJitAtomicU64 checks;
  EJitAtomicU64 mismatches;
  EJitAtomicU64 lastFrozen;
  EJitAtomicU64 lastActual;
  // Copied, not pointed at: the pass's string lives in the JIT'd module, freed
  // when that specialization is evicted or recompiled.
  char name[kVerifySiteNameMax];
};

SiteRecord gSiteTable[kVerifyMaxSites];
EJitAtomicU32 gSiteTableUsed;

bool nameEquals(const char *a, const char *b) {
  for (size_t i = 0; i < kVerifySiteNameMax; ++i) {
    if (a[i] != b[i])
      return false;
    if (a[i] == '\0')
      return true;
  }
  return true; // both truncated to the cap and equal over it
}

void copyName(char *dst, const char *src) {
  size_t i = 0;
  for (; i + 1 < kVerifySiteNameMax && src[i] != '\0'; ++i)
    dst[i] = src[i];
  dst[i] = '\0';
}

/// Find the record for \p site, claiming a free one on first sight. Returns
/// null once the table is full — the totals still account for those sites.
SiteRecord *lookupSite(const char *site) {
  const uint32_t used = gSiteTableUsed.loadAcquire();
  for (uint32_t i = 0; i < used && i < kVerifyMaxSites; ++i) {
    SiteRecord &R = gSiteTable[i];
    if (R.state.loadAcquire() != kSlotReady)
      continue; // being filled by another core; a duplicate row is harmless
    if (nameEquals(R.name, site))
      return &R;
  }

  // Not found: claim the next index. Two cores racing on the same new site can
  // each claim one, which costs a duplicate row and nothing else.
  const uint32_t idx = gSiteTableUsed.fetchAdd(1);
  if (idx >= kVerifyMaxSites) {
    gSiteTableUsed.storeRelease(kVerifyMaxSites); // keep the count honest
    return nullptr;
  }

  SiteRecord &R = gSiteTable[idx];
  R.state.storeRelease(kSlotFilling);
  copyName(R.name, site);
  R.checks.storeRelaxed(0);
  R.mismatches.storeRelaxed(0);
  R.lastFrozen.storeRelaxed(0);
  R.lastActual.storeRelaxed(0);
  R.state.storeRelease(kSlotReady);
  return &R;
}

} // namespace

void llvm::ejit::ejitVerifyGetStats(VerifyStats *out) {
  if (!out)
    return;
  out->sites = gSites.loadRelaxed();
  out->checks = gChecks.loadRelaxed();
  out->mismatches = gMismatches.loadRelaxed();
}

size_t llvm::ejit::ejitVerifySiteCount() {
  const uint32_t used = gSiteTableUsed.loadAcquire();
  return used > kVerifyMaxSites ? kVerifyMaxSites : used;
}

bool llvm::ejit::ejitVerifyGetSite(size_t index, VerifySite *out) {
  if (!out || index >= ejitVerifySiteCount())
    return false;
  SiteRecord &R = gSiteTable[index];
  if (R.state.loadAcquire() != kSlotReady)
    return false;
  copyName(out->site, R.name);
  out->checks = R.checks.loadRelaxed();
  out->mismatches = R.mismatches.loadRelaxed();
  out->lastFrozen = R.lastFrozen.loadRelaxed();
  out->lastActual = R.lastActual.loadRelaxed();
  return true;
}

void llvm::ejit::ejitVerifyResetStats() {
  gSites.storeRelaxed(0);
  gChecks.storeRelaxed(0);
  gMismatches.storeRelaxed(0);
  // Retire the rows before the count, so a check running concurrently with the
  // reset re-claims a slot instead of reading a half-cleared one.
  for (size_t i = 0; i < kVerifyMaxSites; ++i)
    gSiteTable[i].state.storeRelease(kSlotEmpty);
  gSiteTableUsed.storeRelease(0);
}

void llvm::ejit::ejitVerifyNoteSite() { gSites.fetchAdd(1); }

extern "C" void __ejit_verify_check(const char *site, uint64_t baked,
                                    uint64_t actual) {
  gChecks.fetchAdd(1);

  SiteRecord *R = site ? lookupSite(site) : nullptr;
  if (R)
    R->checks.fetchAdd(1);

  if (baked == actual)
    return;

  if (R) {
    R->mismatches.fetchAdd(1);
    R->lastFrozen.storeRelaxed(baked);
    R->lastActual.storeRelaxed(actual);
  }

  // fetchAdd returns the PRE-increment value, so the first mismatch sees 0.
  const uint64_t seen = gMismatches.fetchAdd(1);
  if (seen < kMaxReportedMismatches) {
    EJIT_DIAG("verify MISMATCH site=%s frozen=0x%llx actual=0x%llx",
              site ? site : "(unnamed)",
              static_cast<unsigned long long>(baked),
              static_cast<unsigned long long>(actual));
    if (seen + 1 == kMaxReportedMismatches)
      EJIT_DIAG("verify: %llu mismatches reported, further ones counted only",
                static_cast<unsigned long long>(kMaxReportedMismatches));
  }
}

#endif // EJIT_VERIFY_SUBSTITUTION
