//===-- EJitRepresentativeDiagnostics.h - bounded diagnostic vocabulary -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header contains the ABI-neutral vocabulary and fixed record shape used
// by representative-PGO cold-path diagnostics. It deliberately does not add a
// shared-memory field or choose the product's retention capacity. A future
// versioned query can copy this record through an owner mailbox without ever
// exposing a registry, pointer, STL object or LLVM state.
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITREPRESENTATIVEDIAGNOSTICS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITREPRESENTATIVEDIAGNOSTICS_H

#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#include <cstdint>
#include <type_traits>

namespace llvm {
namespace ejit {

enum class EJitRepresentativeDiagStage : uint32_t {
  Admission = 1,
  Candidate = 2,
  ProfilePublish = 3,
  FinalCompare = 4,
};

enum class EJitRepresentativeDiagOutcome : uint32_t {
  Pending = 1,
  SharedReuse = 2,
  Independent = 3,
  Failure = 4,
  Cancelled = 5,
  Deferred = 6,
};

/// Values are explicit because logs, tests and a future additive query must
/// share one vocabulary even when a new value is appended later.
enum class EJitRepresentativeDiagReason : uint32_t {
  Unclassified = 0,
  PgoDisabled = 1,
  NotAsync = 2,
  AuditOnly = 3,
  ModeChange = 4,
  ZeroQuota = 5,
  GroupBudget = 6,
  MemberBudget = 7,
  DiagnosticBudget = 8,
  PrefixIR = 9,
  Schema = 10,
  Binding = 11,
  WritableGlobal = 12,
  FunctionPointerTable = 13,
  PrivateState = 14,
  TLS = 15,
  AddressSpace = 16,
  AliasIFunc = 17,
  InlineAsm = 18,
  BlockAddress = 19,
  IRBudget = 20,
  IdentityBudget = 21,
  BitcodeMissing = 22,
  InvalidInput = 23,
  NewGroup = 24,
  ProfileUnavailable = 25,
  ProfileSchema = 26,
  ProfileBudget = 27,
  NotReady = 28,
  QueueFull = 29,
  AttemptTable = 30,
  Publish = 31,
  OwnerUnavailable = 32,
  Timeout = 33,
  Cancelled = 34,
  GenerationChanged = 35,
  IdentityEqual = 36,
  FinalIR = 37,
  FinalBinding = 38,
  FinalScope = 39,
  LateSplit = 40,
  CodeBudget = 41,
  Link = 42,
  CandidateMatch = 43,
  DuplicatePublish = 44,
};

constexpr uint32_t kEJitRepresentativeDiagReasonCount = 45;

enum class EJitRepresentativeDiagFirstDiff : uint32_t {
  None = 0,
  IRFunctionOrSite = 1,
  BindingSymbolOrKind = 2,
  ScopeField = 3,
};

enum EJitRepresentativeDiagFlag : uint32_t {
  EJitDiagFlagNone = 0,
  EJitDiagFlagTruncated = 1u << 0,
  EJitDiagFlagUnknown = 1u << 1,
  EJitDiagFlagEvicted = 1u << 2,
  EJitDiagFlagIncomplete = 1u << 3,
};

inline const char *ejitRepresentativeDiagStageToken(
    EJitRepresentativeDiagStage Stage) {
  switch (Stage) {
  case EJitRepresentativeDiagStage::Admission:
    return "ADMISSION";
  case EJitRepresentativeDiagStage::Candidate:
    return "CANDIDATE";
  case EJitRepresentativeDiagStage::ProfilePublish:
    return "PROFILE";
  case EJitRepresentativeDiagStage::FinalCompare:
    return "FINAL_COMPARE";
  }
  return "UNKNOWN_STAGE";
}

inline const char *ejitRepresentativeDiagOutcomeToken(
    EJitRepresentativeDiagOutcome Outcome) {
  switch (Outcome) {
  case EJitRepresentativeDiagOutcome::Pending:
    return "PENDING";
  case EJitRepresentativeDiagOutcome::SharedReuse:
    return "SHARED_REUSE";
  case EJitRepresentativeDiagOutcome::Independent:
    return "INDEPENDENT";
  case EJitRepresentativeDiagOutcome::Failure:
    return "FAILURE";
  case EJitRepresentativeDiagOutcome::Cancelled:
    return "CANCELLED";
  case EJitRepresentativeDiagOutcome::Deferred:
    return "DEFER";
  }
  return "UNKNOWN_OUTCOME";
}

inline const char *ejitRepresentativeDiagReasonToken(
    EJitRepresentativeDiagReason Reason) {
  switch (Reason) {
  case EJitRepresentativeDiagReason::Unclassified:
    return "UNCLASSIFIED";
  case EJitRepresentativeDiagReason::PgoDisabled:
    return "PGO_DISABLED";
  case EJitRepresentativeDiagReason::NotAsync:
    return "NOT_ASYNC";
  case EJitRepresentativeDiagReason::AuditOnly:
    return "AUDIT_ONLY";
  case EJitRepresentativeDiagReason::ModeChange:
    return "MODE_CHANGE";
  case EJitRepresentativeDiagReason::ZeroQuota:
    return "ZERO_QUOTA";
  case EJitRepresentativeDiagReason::GroupBudget:
    return "GROUP_BUDGET";
  case EJitRepresentativeDiagReason::MemberBudget:
    return "MEMBER_BUDGET";
  case EJitRepresentativeDiagReason::DiagnosticBudget:
    return "DIAG_BUDGET";
  case EJitRepresentativeDiagReason::PrefixIR:
    return "PREFIX_IR";
  case EJitRepresentativeDiagReason::Schema:
    return "SCHEMA";
  case EJitRepresentativeDiagReason::Binding:
    return "BINDING";
  case EJitRepresentativeDiagReason::WritableGlobal:
    return "WRITABLE_GLOBAL";
  case EJitRepresentativeDiagReason::FunctionPointerTable:
    return "FN_PTR_TABLE";
  case EJitRepresentativeDiagReason::PrivateState:
    return "PRIVATE_STATE";
  case EJitRepresentativeDiagReason::TLS:
    return "TLS";
  case EJitRepresentativeDiagReason::AddressSpace:
    return "ADDRESS_SPACE";
  case EJitRepresentativeDiagReason::AliasIFunc:
    return "ALIAS_IFUNC";
  case EJitRepresentativeDiagReason::InlineAsm:
    return "INLINE_ASM";
  case EJitRepresentativeDiagReason::BlockAddress:
    return "BLOCKADDRESS";
  case EJitRepresentativeDiagReason::IRBudget:
    return "IR_BUDGET";
  case EJitRepresentativeDiagReason::IdentityBudget:
    return "IDENTITY_BUDGET";
  case EJitRepresentativeDiagReason::BitcodeMissing:
    return "BITCODE_MISSING";
  case EJitRepresentativeDiagReason::InvalidInput:
    return "INVALID_INPUT";
  case EJitRepresentativeDiagReason::NewGroup:
    return "NEW_GROUP";
  case EJitRepresentativeDiagReason::ProfileUnavailable:
    return "PROFILE_UNAVAILABLE";
  case EJitRepresentativeDiagReason::ProfileSchema:
    return "PROFILE_SCHEMA";
  case EJitRepresentativeDiagReason::ProfileBudget:
    return "PROFILE_BUDGET";
  case EJitRepresentativeDiagReason::NotReady:
    return "NOT_READY";
  case EJitRepresentativeDiagReason::QueueFull:
    return "QUEUE_FULL";
  case EJitRepresentativeDiagReason::AttemptTable:
    return "ATTEMPT_TABLE";
  case EJitRepresentativeDiagReason::Publish:
    return "PUBLISH";
  case EJitRepresentativeDiagReason::OwnerUnavailable:
    return "OWNER_UNAVAILABLE";
  case EJitRepresentativeDiagReason::Timeout:
    return "TIMEOUT";
  case EJitRepresentativeDiagReason::Cancelled:
    return "CANCELLED";
  case EJitRepresentativeDiagReason::GenerationChanged:
    return "GENERATION_CHANGED";
  case EJitRepresentativeDiagReason::IdentityEqual:
    return "IDENTITY_EQUAL";
  case EJitRepresentativeDiagReason::FinalIR:
    return "FINAL_IR";
  case EJitRepresentativeDiagReason::FinalBinding:
    return "FINAL_BINDING";
  case EJitRepresentativeDiagReason::FinalScope:
    return "FINAL_SCOPE";
  case EJitRepresentativeDiagReason::LateSplit:
    return "LATE_SPLIT";
  case EJitRepresentativeDiagReason::CodeBudget:
    return "CODE_BUDGET";
  case EJitRepresentativeDiagReason::Link:
    return "LINK";
  case EJitRepresentativeDiagReason::CandidateMatch:
    return "CANDIDATE_MATCH";
  case EJitRepresentativeDiagReason::DuplicatePublish:
    return "DUPLICATE_PUBLISH";
  }
  return "UNCLASSIFIED";
}

/// Complete logical identity for a future owner-side record. It contains no
/// pointer, string or dynamic storage, and keeps request attempt separate from
/// group and sampling session identity.
struct EJitRepresentativeDiagIdentity {
  uint32_t funcIndex = 0;
  uint32_t numDims = 0;
  EJitDimPair dims[kEJitMaxRequestDims] = {};
  uint32_t versions[kEJitMaxRequestDims] = {};
  uint32_t generation = 0;
  uint64_t attemptToken = 0;
  uint64_t samplingSessionId = 0;
  uint64_t groupId = 0;
  uint64_t groupGeneration = 0;
};

/// Fixed record material copied by value by a future bounded snapshot. The
/// optional first difference is a locator/hash only; full IR and bundles stay
/// in owner-private storage.
struct EJitRepresentativeDiagRecord {
  uint64_t recordSeq = 0;
  EJitRepresentativeDiagIdentity identity;
  uint32_t stage = 0;
  uint32_t outcome = 0;
  uint32_t reasonCode = 0;
  uint32_t firstDiffKind = 0;
  uint32_t firstDiffOrdinal = 0;
  uint32_t flags = EJitDiagFlagNone;
  uint64_t codeId = 0;
  uint64_t firstDiffHash = 0;
};

static_assert(std::is_standard_layout<EJitRepresentativeDiagIdentity>::value &&
                  std::is_trivially_copyable<
                      EJitRepresentativeDiagIdentity>::value,
              "diagnostic identity must stay fixed-layout and copyable");
static_assert(std::is_standard_layout<EJitRepresentativeDiagRecord>::value &&
                  std::is_trivially_copyable<EJitRepresentativeDiagRecord>::value,
              "diagnostic record must stay fixed-layout and copyable");

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITREPRESENTATIVEDIAGNOSTICS_H
