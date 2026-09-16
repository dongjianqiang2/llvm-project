//===-- EJitPreparedCode.h - Final IR identity and owner-side emission
//------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITPREPAREDCODE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITPREPAREDCODE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Module;
namespace orc {
class LLJIT;
}
namespace ejit {

/// All effective bindings, including backend-synthesized libcalls. The caller
/// must resolve these before preparing IR; this path has no process fallback.
struct EJitCodeBinding {
  std::string name;
  uint64_t address = 0;
  bool callable = false;
  bool operator==(const EJitCodeBinding &Other) const;
};

/// Logical cell/TRP values and lifecycle versions deliberately do not belong
/// here. Policy must describe the final target/ABI/pipeline settings; source
/// is the digest of the original bitcode, not just its filename.
struct EJitCodeIdentityScope {
  std::array<uint8_t, 32> source{};
  std::string entry;
  std::string compilerPolicy;
  uint64_t bindingGeneration = 0;
  bool operator==(const EJitCodeIdentityScope &Other) const;
};

struct EJitCandidateLimits {
  uint32_t maxGroups = 128;
  uint64_t maxIdentityBytes = 16 * 1024 * 1024;
  uint64_t maxModuleBytes = 1024 * 1024;
};
struct EJitCandidateResult {
  uint64_t groupId = 0;
  bool existing = false;
};

class EJitCandidateDirectory {
public:
  using BucketHash = uint64_t (*)(ArrayRef<uint8_t>);
  explicit EJitCandidateDirectory(EJitCandidateLimits Limits = {},
                                  BucketHash Hash = nullptr);
  ~EJitCandidateDirectory();
  Expected<EJitCandidateResult> classify(const Module &CommonPrefix,
                                         EJitCodeIdentityScope Scope,
                                         ArrayRef<EJitCodeBinding> Bindings,
                                         ArrayRef<PgoFunctionSchema> Schema);
  uint32_t groupCount() const;
  uint64_t identityBytes() const;

private:
  friend class EJitCandidateCapture;
  Expected<EJitCandidateResult>
  classifyCanonical(std::string CanonicalIR, EJitCodeIdentityScope Scope,
                    ArrayRef<EJitCodeBinding> Bindings,
                    ArrayRef<PgoFunctionSchema> Schema);
  struct Impl;
  std::unique_ptr<Impl> P;
};

class EJitCandidateCapture {
public:
  EJitCandidateCapture(EJitCandidateDirectory &Directory,
                       EJitCodeIdentityScope Scope,
                       ArrayRef<EJitCodeBinding> Bindings);
  ~EJitCandidateCapture();
  /// Serialize the real optimized prefix before any PGO instrumentation.
  void capturePrefix(const Module &CommonPrefix);
  /// Add schema extracted from this same module after IR instrumentation.
  void complete(ArrayRef<PgoFunctionSchema> Schema);
  bool prefixCaptured() const { return PrefixCaptured; }
  bool completed() const { return Completed; }
  Expected<EJitCandidateResult> takeResult();

private:
  EJitCandidateDirectory &Directory;
  EJitCodeIdentityScope Scope;
  std::vector<EJitCodeBinding> Bindings;
  std::string CanonicalIR;
  std::optional<EJitCandidateResult> Result;
  Error Failure = Error::success();
  bool PrefixCaptured = false;
  bool Completed = false;
};

struct EJitPreparedCodeLimits {
  uint64_t maxModuleBytes = 1024 * 1024;
  uint64_t maxDefinedDataBytes = 1024 * 1024;
  uint64_t maxIdentityBytes = 16 * 1024 * 1024;
  uint32_t maxCodeObjects = 128;
  uint32_t maxIRNodes = 262144;
};

/// Full comparison material, never a proof based on digest alone. Kept
/// separate so an audit can use the same exact comparator without emitting.
class EJitFinalCodeIdentity {
public:
  ArrayRef<uint8_t> digest() const { return digest_; }
  StringRef canonicalIR() const { return ir_; }
  uint64_t storageBytes() const;
  bool equals(const EJitFinalCodeIdentity &Other) const;

private:
  friend class EJitPreparedCode;
  friend class EJitPreparedCodeEmitter;
  EJitCodeIdentityScope scope_;
  std::vector<EJitCodeBinding> bindings_;
  std::string ir_;
  std::array<uint8_t, 32> digest_{};
};

/// Owns a final, already-optimized module. Only its comparison clone is
/// normalized. No mutable module accessor: emission must see precisely the IR
/// whose identity was checked. Creation rejects independent mutable/private
/// address-observable state rather than silently merging it.
/// Errors distinguish operation_not_supported (independent code required),
/// no_buffer_space (stop new work, do not evade budgets by compiling
/// separately) and invalid_argument (bad input/binding). All remain cold-path
/// failures.
class EJitPreparedCode {
public:
  static Expected<std::unique_ptr<EJitPreparedCode>>
  create(orc::ThreadSafeModule FinalModule, EJitCodeIdentityScope Scope,
         ArrayRef<EJitCodeBinding> Bindings,
         const EJitPreparedCodeLimits &Limits = {});
  const EJitFinalCodeIdentity &identity() const { return identity_; }

private:
  friend class EJitPreparedCodeEmitter;
  explicit EJitPreparedCode(orc::ThreadSafeModule M) : module_(std::move(M)) {}
  orc::ThreadSafeModule module_;
  EJitFinalCodeIdentity identity_;
};

/// Experimental owner-only backend boundary. NOT wired to runtime requests,
/// PGO admission, cache publication or wrapper filling yet. Those must use the
/// logical-request/profile/borrow protocol before enabling product sharing.
///
/// LLJIT must outlive this object and all users of its code. Successful
/// physical JITDylibs remain owned by LLJIT until shutdown, never by a logical
/// cache key. Calls are serialized by the compile owner. No lock/refcount on
/// execution.
class EJitPreparedCodeEmitter {
public:
  struct LinkedCode {
    uint64_t codeId = 0;
    void *fn = nullptr;
    bool reused = false;
  };
  struct Stats {
    uint64_t codeObjects = 0;
    uint64_t reused = 0;
    uint64_t identityBytes = 0;
    uint64_t failed = 0;
    uint64_t capacityRejected = 0;
  };

  /// Bucket hash is injectable for collision tests only; exact identity is
  /// always compared. OwnerEpoch must be nonzero and unique within this LLJIT.
  using BucketHash = uint64_t (*)(ArrayRef<uint8_t>);
  EJitPreparedCodeEmitter(orc::LLJIT &J, uint64_t OwnerEpoch,
                          EJitPreparedCodeLimits Limits = {},
                          BucketHash Hash = nullptr);
  ~EJitPreparedCodeEmitter();

  /// Optimize before calling prepare, then link here exactly once. This uses
  /// IRCompileLayer directly, not IRTransformLayer: no post-identity IR passes.
  /// The pointer is LINKED, not a publication/execute-permission certificate.
  /// On SRE it may still be RW/NX. The existing codeReady/flush/range and peer
  /// preparation path must succeed before any logical cache/icache publication.
  Expected<LinkedCode> link(std::unique_ptr<EJitPreparedCode> Prepared);
  Stats stats() const;

  /// Exact full-identity compare of the object already stored under \p CodeId
  /// against \p Other: scope, effective bindings, canonical IR and digest, the
  /// same comparator link() uses - never a digest-only proof. Sharing decisions
  /// for a representative group use this instead of trusting a hash. Returns
  /// false for an unknown codeId. Audit query only: emits nothing, claims
  /// nothing and consumes no budget.
  bool linkedIdentityEquals(uint64_t CodeId,
                            const EJitFinalCodeIdentity &Other) const;

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace ejit
} // namespace llvm

#endif
