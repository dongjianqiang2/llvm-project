//===-- EJitVerifyTest.cpp - substitution verifier runtime tests ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitVerify.h"
#include "gtest/gtest.h"
#include <cstring>
#include <string>
#include <thread>

using namespace llvm::ejit;

#ifdef EJIT_VERIFY_SUBSTITUTION

namespace {

void expectRowCount(size_t expected) {
  EXPECT_EQ(ejitVerifySiteCount(), expected);
}

TEST(EJitVerifyTest, EmissionAndExecutionShareOneStore) {
  ejitVerifyResetStats();
  ejitVerifyNoteSite();
  ejitVerifyNoteSite();
  __ejit_verify_check("probe:g_cfg+0", "probe:global=g_cfg+0", 0, 4, 4);

  VerifyStats stats{};
  ejitVerifyGetStats(&stats);
  EXPECT_EQ(stats.sites, 2u);
  EXPECT_EQ(stats.checks, 1u);
  EXPECT_EQ(stats.mismatches, 0u);
}

TEST(EJitVerifyTest, LongAndSamePrefixIdentitiesDoNotMerge) {
  ejitVerifyResetStats();

  std::string name63(63, 'a');
  std::string name64(64, 'a');
  std::string nameLong(96, 'a');
  // The display is intentionally identical: the full identity is the key.
  for (unsigned i = 0; i < 70; ++i) {
    __ejit_verify_check("fn:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                        name63.c_str(), 0, 1, 1);
    __ejit_verify_check("fn:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                        name64.c_str(), 0, 1, 1);
    __ejit_verify_check("fn:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                        nameLong.c_str(), 0, 1, 1);
  }

  // Same display and offset, but distinct absolute roots. These must remain
  // separate even though a truncated display or summed GEP offset cannot tell
  // them apart.
  __ejit_verify_check("fn:<indirect>+0",
                      "fn:root=inttoptr(0x1000)+offset=0", 0, 9, 9);
  __ejit_verify_check("fn:<indirect>+0",
                      "fn:root=inttoptr(0x2000)+offset=0", 0, 9, 9);

  expectRowCount(5u);
  VerifySite row{};
  ASSERT_TRUE(ejitVerifyGetSite(1, &row));
  EXPECT_EQ(row.checks, 70u);
  ASSERT_TRUE(ejitVerifyGetSite(4, &row));
  EXPECT_EQ(row.checks, 1u);
}

TEST(EJitVerifyTest, OversizedIdentityTokensRemainDistinct) {
  ejitVerifyResetStats();
  __ejit_verify_check("same-display", "ignored", 17, 1, 1);
  __ejit_verify_check("same-display", "ignored", 17, 1, 1);
  __ejit_verify_check("same-display", "ignored", 18, 1, 1);

  VerifyStats stats{};
  ejitVerifyGetStats(&stats);
  EXPECT_EQ(stats.checks, 3u);
  expectRowCount(2u);
  VerifySite row{};
  ASSERT_TRUE(ejitVerifyGetSite(0, &row));
  EXPECT_EQ(row.checks, 2u);
  ASSERT_TRUE(ejitVerifyGetSite(1, &row));
  EXPECT_EQ(row.checks, 1u);
}

TEST(EJitVerifyTest, SiteTableCapacityDoesNotLoseGlobalTotals) {
  ejitVerifyResetStats();
  for (size_t i = 0; i < kVerifyMaxSites + 4; ++i) {
    const std::string identity = "capacity:" + std::to_string(i);
    __ejit_verify_check("capacity", identity.c_str(), 0, 1, 1);
  }

  VerifyStats stats{};
  ejitVerifyGetStats(&stats);
  EXPECT_EQ(stats.checks, kVerifyMaxSites + 4u);
  expectRowCount(kVerifyMaxSites);
}

TEST(EJitVerifyTest, ResetRetiresRowsAtAnEpochBoundary) {
  ejitVerifyResetStats();
  __ejit_verify_check("old", "old-root", 0, 1, 2);
  ejitVerifyResetStats();
  __ejit_verify_check("new", "new-root", 0, 3, 4);

  VerifyStats stats{};
  ejitVerifyGetStats(&stats);
  EXPECT_EQ(stats.checks, 1u);
  EXPECT_EQ(stats.mismatches, 1u);
  expectRowCount(1u);
  VerifySite row{};
  ASSERT_TRUE(ejitVerifyGetSite(0, &row));
  EXPECT_STREQ(row.site, "new");
}

TEST(EJitVerifyTest, ResetSynchronizesWithChecks) {
  ejitVerifyResetStats();
  std::thread worker([] {
    for (unsigned i = 0; i < 2000; ++i)
      __ejit_verify_check("probe", "probe:root", 0, i, i);
  });
  for (unsigned i = 0; i < 32; ++i)
    ejitVerifyResetStats();
  worker.join();

  VerifyStats stats{};
  ejitVerifyGetStats(&stats);
  EXPECT_LE(stats.checks, 2000u);
  EXPECT_EQ(stats.mismatches, 0u);
  EXPECT_LE(ejitVerifySiteCount(), 1u);
}

} // namespace

#else

TEST(EJitVerifyTest, DisabledBuildHasNoVerifierRuntime) { SUCCEED(); }

#endif // EJIT_VERIFY_SUBSTITUTION
