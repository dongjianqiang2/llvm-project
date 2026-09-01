//===-- EJitVerify.h - may_const substitution verifier --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Diagnostic support for Config::verifySubstitution.
//
// PASS6 replaces an ejit_may_const load with the value it reads out of process
// memory at compile time. That is correct only for as long as the memory keeps
// that value, and nothing in the pipeline can prove it will: a field written
// from live state (a load metric, a queue depth) is frozen just as readily as
// one written once at configuration time. The failure is silent — the memory
// stays correct, but the specialized code no longer reads it.
//
// Under verifySubstitution the pass keeps the load and emits a call to
// __ejit_verify_check comparing the value actually in memory against the one
// it WOULD have frozen. Every divergence reported is a field that must not
// carry ejit_may_const. Running a real workload this way classifies the marked
// fields by measurement rather than by inspection.
//
// The comparison covers more than value stability. The load reads through the
// address the source computes, from the AOT global; the frozen value comes
// from the address PASS6 derives out of the period registry. A mismatch is
// therefore also how a wrong registered base or a mis-computed field offset
// shows up — cases where substitution would bake in a value from the wrong
// place.
//
// The loads survive, so nothing folds: this validates the frozen values, it
// does not produce fast code. Coverage is limited to sites that execute.
//
// Built only under -DEJIT_VERIFY_SUBSTITUTION (default OFF). Without it nothing
// here is declared or defined and no caller is compiled — pass instrumentation,
// runtime helper, libcall entry and the ejit_verify_* C API all disappear — and
// ejit_init() refuses config.verifySubstitution.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITVERIFY_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITVERIFY_H

#include <cstddef>
#include <cstdint>

#ifdef EJIT_VERIFY_SUBSTITUTION

namespace llvm {
namespace ejit {

/// Counters accumulated by the substitution verifier. Relaxed atomics: these
/// are diagnostic totals, never a control input.
struct VerifyStats {
  uint64_t sites;      ///< instrumented load sites emitted by the pass
  uint64_t checks;     ///< instrumented loads executed
  uint64_t mismatches; ///< executions where memory != the frozen value
};

/// Longest site name retained per record, including the terminator. Names are
/// "<func>:<global>+<byteOffset>"; anything longer is truncated rather than
/// dropped, since the leading text is what locates the field.
constexpr size_t kVerifySiteNameMax = 64;

/// Number of distinct sites the runtime can account for individually. A
/// fixed table: verify mode runs on targets where a diagnostic must not
/// allocate. Sites past the limit still count toward the totals above.
constexpr size_t kVerifyMaxSites = 64;

/// Per-site accounting, the form the classification is actually read in: which
/// marked field diverged, how often, and against what. The totals alone cannot
/// separate one moving field from a genuinely unstable set, and the log lines
/// that name a site exist only in EJIT_DIAG_ENABLE builds.
struct VerifySite {
  char site[kVerifySiteNameMax]; ///< "<func>:<global>+<byteOffset>"
  uint64_t checks;               ///< executions of this site
  uint64_t mismatches;           ///< executions where memory != frozen
  uint64_t lastFrozen;           ///< frozen value at the most recent mismatch
  uint64_t lastActual;           ///< memory value at the most recent mismatch
};

/// Snapshot the counters. \p out must be non-null.
void ejitVerifyGetStats(VerifyStats *out);

/// Number of per-site records held, i.e. distinct sites that have executed.
/// Records are appended in order of first execution and never move, so an
/// index below this bound stays valid until the next reset.
size_t ejitVerifySiteCount();

/// Copy record \p index into \p out. False when the index is out of range or
/// the record is not yet fully claimed. One record at a time, deliberately:
/// a batch copy would need a table-sized buffer on the caller's stack.
bool ejitVerifyGetSite(size_t index, VerifySite *out);

/// Zero the counters and drop every per-site record. Intended for tests
/// bracketing a workload.
void ejitVerifyResetStats();

/// Record that the pass emitted one instrumented site.
void ejitVerifyNoteSite();

} // namespace ejit
} // namespace llvm

/// Called from JIT-compiled code, once per execution of an instrumented
/// may_const load. \p site names the access as "<func>:<global>+<byteOffset>".
/// \p baked is the value PASS6 would have frozen; \p actual is what the load
/// just returned. Values are zero-extended (floats bitcast) to 64 bits.
extern "C" void __ejit_verify_check(const char *site, uint64_t baked,
                                    uint64_t actual);

#endif // EJIT_VERIFY_SUBSTITUTION

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITVERIFY_H
