//===-- EJitDedupIndex.h - Specialization dedup fingerprint index ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception.
//
//===----------------------------------------------------------------------===//
//
//  Post-pipeline IR fingerprint + owner-private dedup index for EmbeddedJIT
//  specialization merging (design: jit_design_doc/EJIT_SPECIALIZATION_DEDUP.md).
//
//  Two specialization compiles of the same funcIndex whose post-pipeline IR
//  fingerprints match produce, under the engine's deterministic single-thread
//  compilation, identical machine code. The second compile can therefore
//  reuse the first's published fnPtr instead of consuming another code-pool
//  allocation. The index maps (funcIndex, fingerprint) -> canonical fnPtr.
//
//  Fingerprint = three words:
//    fp1     llvm::StructuralHash(M, DetailedHash=true): BB/instruction
//            structure, opcodes, operand types, constants, GlobalValue
//            references by NAME. Module name and metadata are NOT hashed.
//    fp2     FNV-1a over every module-defined global's name/linkage/
//            isConstant + INITIALIZER (StructuralHash's module-level
//            GlobalVariable update hashes only the type ID - a GlobalOpt
//            promotion baking a specialized constant into an initializer
//            would otherwise be invisible).
//    irUnits (numDefinedFuncs << 32) | numInstructions - cheap reject.
//
//  Correctness envelope (why same-funcIndex only): StructuralHash does not
//  hash function signatures, attributes, instruction flags (nsw/volatile/
//  atomic/fastmath) or metadata. Two DIFFERENT functions can collide
//  (e.g. `f(i32 %unused){ret void}` vs `g(i8 %unused){ret void}`). The key
//  therefore includes funcIndex: same funcIndex means same bitcode, so all
//  unhashed dimensions are identical by construction.
//
//  Failure direction: any nondeterminism or blind spot produces a DIFFERENT
//  fingerprint -> no merge (safe). Only a simultaneous fp1+fp2+irUnits
//  collision could wrongly merge; at the default capacity the birthday
//  probability is ~1e-15.
//
//  Memory: the index is a fixed-capacity open-addressed table owned by the
//  compile owner (EJitOrcEngine::Impl), owner-thread-only (all compilation
//  is serialized on the owner core), never shared cross-core. Full -> dedup
//  silently degrades for new entries (one diag).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITDEDUPINDEX_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITDEDUPINDEX_H

#include <cstddef>
#include <cstdint>

namespace llvm {
class Module;

namespace ejit {

/// Fingerprint of a post-specialization-pipeline Module.
struct DedupFingerprint {
  /// llvm::StructuralHash(M, /*DetailedHash=*/true).
  uint64_t fp1 = 0;
  /// FNV-1a over defined-global identity + initializers (see header comment).
  uint64_t fp2 = 0;
  /// (numDefinedFuncs << 32) | numInstructions.
  uint64_t irUnits = 0;

  bool operator==(const DedupFingerprint &O) const {
    return fp1 == O.fp1 && fp2 == O.fp2 && irUnits == O.irUnits;
  }
  bool operator!=(const DedupFingerprint &O) const { return !(*this == O); }
};

/// Compute the fingerprint of \p M. O(#globals + #instructions); must be
/// called on the OWNER compile thread only (it is pure, but like the rest of
/// the compile path it assumes serialized access to the module).
DedupFingerprint computeModuleFingerprint(const Module &M);

/// Fixed-capacity open-addressed (funcIndex, fingerprint) -> fnPtr index.
/// Owner-private; no locking (single compile thread). Zero-capacity failure
/// is a clean degrade, never an error.
class EJitDedupIndex {
public:
  static constexpr size_t kCapacity = 512;

  struct Stats {
    uint64_t inserts = 0;     ///< entries inserted (fresh compiles)
    uint64_t merges = 0;      ///< ON-mode hits served with a reused fnPtr
    uint64_t wouldMerge = 0;  ///< DryRun-mode hits counted (still compiled)
    uint32_t entries = 0;     ///< live entries
    bool full = false;        ///< capacity reached (dedup degraded)
  };

  /// Exact-match lookup. Returns the canonical fnPtr or nullptr.
  void *find(uint32_t funcIndex, const DedupFingerprint &FP);

  /// DryRun bookkeeping: record a would-be merge without affecting entries.
  void noteWouldMerge() { Stats_.wouldMerge++; }

  /// ON-mode bookkeeping: a hit was served with a reused fnPtr.
  void noteMerge() { Stats_.merges++; }

  /// Insert/refresh an entry. Returns false when the table is full (the
  /// caller logs once and keeps compiling without dedup for new entries).
  bool insert(uint32_t funcIndex, const DedupFingerprint &FP, void *fnPtr);

  /// Drop every entry (owner re-election hands the engine - and thus the
  /// index - to a fresh instance anyway; this is the explicit reset).
  void clear();

  const Stats &stats() const { return Stats_; }
  size_t size() const { return Stats_.entries; }

private:
  struct Slot {
    uint32_t funcIndex = 0;
    bool used = false;
    DedupFingerprint FP;
    void *fnPtr = nullptr;
  };
  Slot Slots_[kCapacity];
  Stats Stats_;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITDEDUPINDEX_H
