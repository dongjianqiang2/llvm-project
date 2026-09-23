#include "llvm/ExecutionEngine/EJIT/EJitReuseDiagnostics.h"
#include "gtest/gtest.h"
#include <thread>
#include <atomic>

using namespace llvm;
using namespace llvm::ejit;

namespace {
TEST(EJitFrozenValues, MultipleDifferencesAreBoundedAndMatchedBySite) {
  EJitFrozenSnapshot A, B;
  A.captured = B.captured = true;
  for (unsigned I = 1; I <= 6; ++I) A.record(I, "f:row", "i32 3");
  for (unsigned I = 6; I; --I) B.record(I, "f:row", "i32 17");
  auto D = compareFrozen(A, B);
  EXPECT_TRUE(D.available);
  EXPECT_FALSE(D.incomplete);
  EXPECT_EQ(D.compared, 6u);
  EXPECT_EQ(D.different, 6u);
  EXPECT_EQ(D.shown, 4u);
  EXPECT_EQ(D.differences[0].site, 1u);
  EXPECT_STREQ(D.differences[0].peer, "i32 3");
  EXPECT_STREQ(D.differences[0].request, "i32 17");
}
TEST(EJitFrozenValues, MissingClonedAndTruncatedSitesAreNotGuessed) {
  EJitFrozenSnapshot A, B;
  A.captured = B.captured = true;
  A.record(1, "f:g", "i32 1");
  B.record(1, "f:g", "i32 2");
  B.record(1, "f:g", "i32 3");
  A.record(2, "f:g", "i32 1");
  A.record(3, std::string(80, 'a'), "i32 1");
  B.record(3, std::string(80, 'a'), "i32 2");
  auto D = compareFrozen(A, B);
  EXPECT_TRUE(D.incomplete);
  EXPECT_EQ(D.compared, 0u);
  EXPECT_EQ(D.different, 0u);
  EXPECT_FALSE(compareFrozen(A, EJitFrozenSnapshot{}).available);
  for (unsigned I = 4; I < 100; ++I) A.record(I, "f:g", "i32 1");
  EXPECT_EQ(A.count, A.Capacity);
  EXPECT_GT(A.omitted, 0u);
}
TEST(EJitFrozenValues, EqualRecordedValuesDoNotClaimIdenticalIR) {
  EJitFrozenSnapshot A;
  A.captured = true;
  A.record(1, "f:g", "i32 1");
  auto D = compareFrozen(A, A);
  EXPECT_TRUE(D.available);
  EXPECT_EQ(D.compared, 1u);
  EXPECT_EQ(D.different, 0u);
  EXPECT_FALSE(D.incomplete);
}
EJitReuseDiagnostic event(unsigned Group = 1) {
  EJitReuseDiagnostic R;
  R.identity.groupId = Group;
  R.identity.generation = R.identity.groupGeneration = 1;
  R.identity.funcIndex = 7;
  EJitReuseDiagnosticStore::copyText(R.entry, "reuse_0");
  EJitReuseDiagnosticStore::copyText(R.stage, "CANDIDATE");
  EJitReuseDiagnosticStore::copyText(R.reason, "PREFIX_IR_DIFF");
  EJitReuseDiagnosticStore::copyText(R.action, "NEW_GROUP");
  return R;
}
TEST(EJitReuseDiagnostics, DefaultDetailsDedupButNewGenerationSurvives) {
  EJitReuseDiagnosticStore S;
  EXPECT_EQ(S.levelFor("anything"), 2u);
  auto R = event();
  R.level = 2;
  EJitReuseDiagnosticStore::copyText(R.left, "private detail");
  EXPECT_TRUE(S.record(R));
  EXPECT_STREQ(R.left, "private detail");
  R.identity.attemptToken = 99;
  EXPECT_FALSE(S.record(R));
  R.identity.groupGeneration = 2;
  EXPECT_TRUE(S.record(R));
  EJitReuseDiagnosticStore::Snapshot Snap;
  ASSERT_TRUE(S.snapshot(Snap));
  ASSERT_EQ(Snap.count, 2u);
  EXPECT_EQ(Snap.level, 2u);
  EXPECT_STREQ(Snap.records[0].left, "private detail");
  EXPECT_EQ(Snap.records[0].repeats, 1u);
  EXPECT_LT(Snap.records[0].sequence, Snap.records[1].sequence);
  R.identity.numDims = 1;
  R.identity.dims[0] = {0, 5};
  EXPECT_TRUE(S.record(R));
  R.identity.dims[0].instanceId = 6;
  EXPECT_TRUE(S.record(R));
  R.identity.versions[0] = 2;
  EXPECT_TRUE(S.record(R));
  ASSERT_TRUE(S.snapshot(Snap));
  EXPECT_EQ(Snap.count, 5u);
}
TEST(EJitReuseDiagnostics, FilterLevelResetAndTextLimits) {
  EJitReuseDiagnosticStore S;
  ASSERT_TRUE(S.configure("reuse_0", 2));
  EXPECT_EQ(S.levelFor("reuse_0"), 2u);
  EXPECT_EQ(S.levelFor("reuse_01"), 0u);
  EXPECT_FALSE(S.configure(std::string(96, 'a'), 2));
  EXPECT_FALSE(S.configure("reuse_0", 3));
  EXPECT_FALSE(S.configure("bad\nname", 1));
  auto R = event();
  R.level = 2;
  R.truncated = EJitReuseDiagnosticStore::copyText(R.left, std::string(1000, 'x'));
  EXPECT_TRUE(R.truncated);
  EXPECT_EQ(std::strlen(R.left), 256u);
  EJitReuseDiagnosticStore::copyText(R.right, "a\nb\tc");
  EXPECT_STREQ(R.right, "a b c");
  ASSERT_TRUE(S.record(R));
  EJitReuseDiagnosticStore::Snapshot Snap;
  ASSERT_TRUE(S.snapshot(Snap));
  EXPECT_EQ(Snap.records[0].level, 2u);
  EXPECT_EQ(std::strlen(Snap.records[0].left), 256u);
  ASSERT_TRUE(S.reset());
  ASSERT_TRUE(S.snapshot(Snap));
  EXPECT_EQ(Snap.count, 0u);
  EXPECT_EQ(S.levelFor("reuse_0"), 2u);
  ASSERT_TRUE(S.configure("*", 1));
  EXPECT_TRUE(S.record(R));
  ASSERT_TRUE(S.snapshot(Snap));
  EXPECT_EQ(Snap.records[0].level, 1u);
  EXPECT_EQ(Snap.records[0].left[0], 0);
  EXPECT_EQ(Snap.records[0].right[0], 0);
  ASSERT_TRUE(S.configure("*", 0));
  EXPECT_FALSE(S.record(R));
}
TEST(EJitReuseDiagnostics, RingBoundAndEvictionAreVisible) {
  EJitReuseDiagnosticStore S;
  for (unsigned I = 1; I <= 20; ++I) {
    auto R = event(I);
    EXPECT_TRUE(S.record(R));
  }
  EJitReuseDiagnosticStore::Snapshot Snap;
  ASSERT_TRUE(S.snapshot(Snap));
  EXPECT_EQ(Snap.count, 16u);
  EXPECT_EQ(Snap.evicted, 4u);
  EXPECT_EQ(Snap.records[0].identity.groupId, 5u);
  EXPECT_EQ(Snap.records[15].identity.groupId, 20u);
}
TEST(EJitReuseDiagnostics, ConcurrentCaptureAndSnapshotStayBounded) {
  EJitReuseDiagnosticStore S;
  std::thread Producer([&] {
    for (unsigned I = 1; I <= 2000; ++I) {
      auto R = event(I);
      S.record(R);
    }
  });
  for (unsigned I = 0; I < 1000; ++I) {
    EJitReuseDiagnosticStore::Snapshot Snap;
    if (S.snapshot(Snap)) {
      EXPECT_LE(Snap.count, 16u);
      for (unsigned J = 1; J < Snap.count; ++J)
        EXPECT_LT(Snap.records[J - 1].sequence, Snap.records[J].sequence);
    }
  }
  Producer.join();
}
} // namespace
