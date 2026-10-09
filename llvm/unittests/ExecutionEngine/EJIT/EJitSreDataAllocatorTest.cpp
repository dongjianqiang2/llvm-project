//===-- EJitSreDataAllocatorTest.cpp - Real bounded DataOnly arena tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ExecutionEngine/EJIT/EJitSreDataAllocator.h"
#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#endif
#include "gtest/gtest.h"
#include <memory>
#include <string>

using namespace llvm;
using namespace llvm::ejit;
namespace data_detail = llvm::ejit::detail;

namespace {
class DataReservation {
public:
  explicit DataReservation(uint64_t Bytes = 32u << 20)
      : raw_(new uint8_t[Bytes + data_detail::SreDataBlockBytes]), bytes_(Bytes) {
    base_ = (reinterpret_cast<uintptr_t>(raw_.get()) +
             data_detail::SreDataBlockBytes - 1) &
            ~(uintptr_t(data_detail::SreDataBlockBytes) - 1);
  }
  static uintptr_t claim(void *Context, uint64_t Bytes) {
    auto &R = *static_cast<DataReservation *>(Context);
    ++R.calls;
    R.lastClaim = Bytes;
    if (Bytes > R.bytes_ - R.used)
      return 0;
    const uintptr_t Result = R.base_ + R.used;
    R.used += Bytes;
    return Result;
  }
  uintptr_t base() const { return base_; }
  uint64_t bytes() const { return bytes_; }
  uint64_t used = 0;
  unsigned calls = 0;
  uint64_t lastClaim = 0;
private:
  std::unique_ptr<uint8_t[]> raw_;
  uintptr_t base_;
  uint64_t bytes_;
};

class ArenaFixture {
public:
  explicit ArenaFixture(uint64_t Bytes = 32u << 20, uint64_t Identity = 1)
      : reservation(Bytes), arena(state, reservation.base(), Bytes, Identity,
                                 &DataReservation::claim, &reservation) {
    state.domainIdentity.storeRelaxed(Identity);
  }
  DataReservation reservation;
  data_detail::SreDataOnlyState state{};
  data_detail::SreDataOnlyArena arena;
};

std::string failed(Error E) {
  EXPECT_TRUE(bool(E));
  return toString(std::move(E));
}
} // namespace

TEST(SreDataOnlyAllocatorTest, UnusedArenaClaimsNothing) {
  ArenaFixture F;
  auto S = F.arena.stats();
  ASSERT_EQ(S.snapshotValid, 1u);
  EXPECT_EQ(S.claimedBytes, 0u);
  EXPECT_EQ(S.consumedBytes, 0u);
  EXPECT_EQ(F.reservation.calls, 0u);
}

TEST(SreDataOnlyAllocatorTest, PageIsolatedGenerationsShareOneExclusiveBlock) {
  ArenaFixture F;
  EJitSreDataAllocation A{}, B{};
  ASSERT_FALSE(bool(F.arena.allocate(17, 1, A)));
  ASSERT_FALSE(bool(F.arena.allocate(19, 2, B)));
  EXPECT_EQ(A.address % 4096, 0u);
  EXPECT_EQ(B.address, A.address + 4096);
  EXPECT_EQ(A.blockBase, B.blockBase);
  EXPECT_EQ(A.blockBytes, 2u << 20);
  EXPECT_EQ(A.payloadBytes, 17u);
  EXPECT_EQ(A.bytes, 4096u);
  EXPECT_NE(A.identity, B.identity);
  EXPECT_TRUE(F.arena.validate(A));
  EXPECT_TRUE(F.arena.validate(B));
  // These are actual owned byte ranges, not synthetic numeric addresses.
  auto *AP = reinterpret_cast<uint8_t *>(A.address);
  auto *BP = reinterpret_cast<uint8_t *>(B.address);
  AP[0] = 31; AP[A.bytes - 1] = 32;
  BP[0] = 41; BP[B.bytes - 1] = 42;
  EXPECT_EQ(AP[0], 31);
  EXPECT_EQ(AP[A.bytes - 1], 32);
  EXPECT_EQ(BP[0], 41);
  EXPECT_EQ(BP[B.bytes - 1], 42);
  auto S = F.arena.stats();
  EXPECT_EQ(S.claimedBytes, 2u << 20);
  EXPECT_EQ(S.consumedBytes, 8192u);
  EXPECT_EQ(S.liveBytes, 8192u);
  EXPECT_EQ(F.reservation.calls, 1u);
}

TEST(SreDataOnlyAllocatorTest, FourMiBResourceClaimsOneContiguousRange) {
  ArenaFixture F;
  EJitSreDataAllocation A{};
  ASSERT_FALSE(bool(F.arena.allocate(4u << 20, 7, A)));
  EXPECT_EQ(F.reservation.calls, 1u);
  EXPECT_EQ(F.reservation.lastClaim, 4u << 20);
  EXPECT_EQ(A.blockBytes, 4u << 20);
  EXPECT_EQ(A.address, A.blockBase);
  EXPECT_EQ(A.bytes, 4u << 20);
  EXPECT_TRUE(F.arena.validate(A));
}

TEST(SreDataOnlyAllocatorTest, CodeClaimsCannotInterleaveMultiBlockResource) {
  ArenaFixture F;
  EJitSreDataAllocation A{}, B{};
  ASSERT_FALSE(bool(F.arena.allocate(4096, 1, A)));
  const uintptr_t Code = DataReservation::claim(&F.reservation, 2u << 20);
  ASSERT_FALSE(bool(F.arena.allocate(3u << 20, 2, B)));
  EXPECT_EQ(B.blockBase, Code + (2u << 20));
  EXPECT_EQ(B.blockBytes, 4u << 20);
  EXPECT_EQ(F.reservation.calls, 3u);
  EXPECT_EQ(F.reservation.lastClaim, 4u << 20);
  EXPECT_GE(B.address, Code + (2u << 20));
  EXPECT_LE(A.blockBase + A.blockBytes, Code);
}

TEST(SreDataOnlyAllocatorTest, ResourceAndHistoricalLimitsDoNotRelaxAfterRelease) {
  ArenaFixture F;
  EJitSreDataAllocation TooBig{};
  EXPECT_NE(failed(F.arena.allocate((4u << 20) + 1, 1, TooBig)).find("invalid"),
            std::string::npos);
  EXPECT_EQ(F.reservation.calls, 0u);
  for (unsigned I = 0; I < 4; ++I) {
    EJitSreDataAllocation A{};
    ASSERT_FALSE(bool(F.arena.allocate(4u << 20, I + 1, A)));
    ASSERT_TRUE(F.arena.release(A, false));
  }
  EXPECT_NE(failed(F.arena.allocate(1, 5, TooBig)).find("historical"),
            std::string::npos);
  EXPECT_EQ(F.reservation.calls, 4u);
  auto S = F.arena.stats();
  EXPECT_EQ(S.claimedBytes, 16u << 20);
  EXPECT_EQ(S.consumedBytes, 16u << 20);
  EXPECT_EQ(S.releasedBytes, 16u << 20);
  EXPECT_EQ(S.liveBytes, 0u);
}

TEST(SreDataOnlyAllocatorTest, FixedReservationHasIndependentRemainingSpaceLimit) {
  ArenaFixture F(4u << 20);
  const uintptr_t Code = DataReservation::claim(&F.reservation, 2u << 20);
  EJitSreDataAllocation A{}, B{};
  ASSERT_FALSE(bool(F.arena.allocate(2u << 20, 1, A)));
  EXPECT_EQ(A.blockBase, Code + (2u << 20));
  EXPECT_NE(failed(F.arena.allocate(1, 2, B)).find("fixed reservation exhausted"),
            std::string::npos);
  EXPECT_EQ(B.address, 0u);
  EXPECT_EQ(F.arena.stats().claimedBytes, 2u << 20);
}

TEST(SreDataOnlyAllocatorTest, RecordBoundIsExplicitAndNeverRecycled) {
  ArenaFixture F;
  EJitSreDataAllocation A{};
  for (unsigned I = 0; I < data_detail::SreDataMaxRecords; ++I) {
    ASSERT_FALSE(bool(F.arena.allocate(1, I + 1, A)));
    ASSERT_TRUE(F.arena.release(A, false));
  }
  EXPECT_NE(failed(F.arena.allocate(1, 257, A)).find("record capacity"),
            std::string::npos);
  auto S = F.arena.stats();
  EXPECT_EQ(S.recordCount, 256u);
  EXPECT_EQ(S.consumedBytes, 256u * 4096);
  EXPECT_EQ(S.claimedBytes, 2u << 20);
}

TEST(SreDataOnlyAllocatorTest, ExactIdentityRejectsGuessedChangedAndForeignDescriptors) {
  ArenaFixture F, Foreign(32u << 20, 2);
  EJitSreDataAllocation A{};
  ASSERT_FALSE(bool(F.arena.allocate(19, 3, A)));
  ASSERT_TRUE(F.arena.validate(A));
  EXPECT_FALSE(Foreign.arena.validate(A));
  auto Changed = A;
  Changed.address += 4096;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; ++Changed.generation;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; ++Changed.bytes;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; ++Changed.payloadBytes;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; ++Changed.identity;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; ++Changed.domainIdentity;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; ++Changed.blockBytes;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; Changed.blockBase = UINTPTR_MAX;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; Changed.recordIndex = UINT32_MAX;
  EXPECT_FALSE(F.arena.validate(Changed));
  Changed = A; Changed.reserved = 1;
  EXPECT_FALSE(F.arena.validate(Changed));
}

TEST(SreDataOnlyAllocatorTest, FailedPreparationAndRetirementRemainChargedHoles) {
  ArenaFixture F;
  EJitSreDataAllocation A{}, B{}, C{};
  ASSERT_FALSE(bool(F.arena.allocate(31, 1, A)));
  ASSERT_TRUE(F.arena.release(A, true));
  EXPECT_FALSE(F.arena.validate(A));
  EXPECT_FALSE(F.arena.release(A, false));
  ASSERT_FALSE(bool(F.arena.allocate(7, 2, B)));
  ASSERT_TRUE(F.arena.release(B, false));
  ASSERT_FALSE(bool(F.arena.allocate(9, 3, C)));
  EXPECT_EQ(B.address, A.address + 4096);
  EXPECT_EQ(C.address, B.address + 4096);
  auto S = F.arena.stats();
  EXPECT_EQ(S.claimedBytes, 2u << 20);
  EXPECT_EQ(S.consumedBytes, 12288u);
  EXPECT_EQ(S.failedBytes, 4096u);
  EXPECT_EQ(S.releasedBytes, 4096u);
  EXPECT_EQ(S.liveBytes, 4096u);
  EXPECT_EQ(S.failedBytes + S.releasedBytes + S.liveBytes, S.consumedBytes);
}

TEST(SreDataOnlyAllocatorTest, MalformedClaimCannotDisappearFromHistoricalAccounting) {
  DataReservation Reservation;
  data_detail::SreDataOnlyState State{};
  State.domainIdentity.storeRelaxed(31);
  auto BadClaim = [](void *Context, uint64_t Bytes) -> uintptr_t {
    return DataReservation::claim(Context, Bytes) + 1;
  };
  data_detail::SreDataOnlyArena Arena(State, Reservation.base(), Reservation.bytes(),
                                     31, BadClaim, &Reservation);
  EJitSreDataAllocation A{};
  EXPECT_NE(failed(Arena.allocate(17, 1, A)).find("escaped"), std::string::npos);
  EXPECT_EQ(A.address, 0u);
  auto S = Arena.stats();
  EXPECT_EQ(S.claimedBytes, 2u << 20);
  EXPECT_EQ(S.failedClaimBytes, 2u << 20);
  EXPECT_EQ(S.consumedBytes, 0u);
  EXPECT_EQ(S.recordCount, 0u);
  EXPECT_EQ(Reservation.used, 2u << 20);
}

TEST(SreDataOnlyAllocatorTest, InvalidSizeGenerationAndDomainFailBeforeClaim) {
  ArenaFixture F;
  EJitSreDataAllocation A{};
  consumeError(F.arena.allocate(0, 1, A));
  consumeError(F.arena.allocate(1, 0, A));
  consumeError(F.arena.allocate(UINT64_MAX, 1, A));
  data_detail::SreDataOnlyArena Bad(F.state, UINTPTR_MAX - 1, 2u << 20, 1,
                                   &DataReservation::claim, &F.reservation);
  consumeError(Bad.allocate(1, 1, A));
  EXPECT_EQ(A.address, 0u);
  EXPECT_EQ(F.reservation.calls, 0u);
}

TEST(SreDataOnlyAllocatorTest, BoundedContentionCannotReturnUnprovedStorage) {
  ArenaFixture F;
  EJitSreDataAllocation A{};
  F.state.lock.storeRelaxed(1);
  EXPECT_NE(failed(F.arena.allocate(1, 1, A)).find("busy"), std::string::npos);
  EXPECT_FALSE(F.arena.validate(A));
  EXPECT_FALSE(F.arena.release(A, false));
  EXPECT_EQ(F.arena.stats().snapshotValid, 0u);
  EXPECT_EQ(F.reservation.calls, 0u);
  F.state.lock.storeRelaxed(0);
}

#if defined(EJIT_SRE_CODE_POOL) && !defined(EJIT_FREESTANDING)
TEST(SreDataOnlyAllocatorTest, ScopedRealDomainUsesProductionEntryAndRejectsForeignScope) {
  DataReservation Reservation;
  data_detail::SreDataOnlyState First{}, Second{};
  data_detail::ScopedSreDataOnlyTestDomain Outer(
      First, Reservation.base(), Reservation.bytes(), &DataReservation::claim,
      &Reservation);
  EJitSreDataAllocation A{}, B{};
  ASSERT_FALSE(bool(allocateSreSmallTableStorage(17, 1, A)));
  ASSERT_TRUE(validateSreSmallTableStorage(A));
  {
    data_detail::ScopedSreDataOnlyTestDomain Inner(
        Second, Reservation.base(), Reservation.bytes(), &DataReservation::claim,
        &Reservation);
    EXPECT_FALSE(validateSreSmallTableStorage(A));
    ASSERT_FALSE(bool(allocateSreSmallTableStorage(17, 1, B)));
    EXPECT_NE(A.domainIdentity, B.domainIdentity);
    EXPECT_TRUE(validateSreSmallTableStorage(B));
    EXPECT_FALSE(releaseSreSmallTableStorage(A));
    ASSERT_TRUE(releaseSreSmallTableStorage(B));
  }
  EXPECT_TRUE(validateSreSmallTableStorage(A));
  EXPECT_FALSE(validateSreSmallTableStorage(B));
  ASSERT_TRUE(releaseSreSmallTableStorage(A));
  EXPECT_EQ(getSreSmallTableStorageStats().releasedBytes, 4096u);
}

TEST(SreDataOnlyAllocatorTest, ReenteringSameHostDomainNeverResetsCursor) {
  DataReservation Reservation;
  data_detail::SreDataOnlyState State{};
  EJitSreDataAllocation A{}, B{};
  {
    data_detail::ScopedSreDataOnlyTestDomain Scope(
        State, Reservation.base(), Reservation.bytes(), &DataReservation::claim,
        &Reservation);
    ASSERT_FALSE(bool(allocateSreSmallTableStorage(17, 1, A)));
    ASSERT_TRUE(releaseSreSmallTableStorage(A));
  }
  {
    data_detail::ScopedSreDataOnlyTestDomain Scope(
        State, Reservation.base(), Reservation.bytes(), &DataReservation::claim,
        &Reservation);
    EXPECT_FALSE(validateSreSmallTableStorage(A));
    ASSERT_FALSE(bool(allocateSreSmallTableStorage(17, 2, B)));
    EXPECT_EQ(B.address, A.address + 4096);
    EXPECT_EQ(B.domainIdentity, A.domainIdentity);
    EXPECT_EQ(getSreSmallTableStorageStats().consumedBytes, 8192u);
    ASSERT_TRUE(releaseSreSmallTableStorage(B));
  }
}
#endif
