//===-- EJitTarget.h - Tune JIT code for the core it runs on ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  The extracted bitcode carries the AOT compile's subtarget on every function
//  ("target-cpu"="generic", "target-features"="+v8a,+neon,..."). Function
//  attributes win over the JIT TargetMachine's CPU, so the JIT would compile
//  for a generic core even though, unlike AOT, it runs on the real one.
//
//  EJitJitTarget is what the JIT adds back, at two levels:
//
//   - tuneCpu  -> "tune-cpu". Selects the scheduling model and tuning costs
//                 only; it never enables an instruction, so a wrong answer
//                 costs speed, not correctness. On by default.
//   - features -> "+feature" entries appended to "target-features". These do
//                 enable instructions, and the JIT code is shared by every
//                 core, so they are opt-in (EJIT_TARGET_FEATURES) and assume
//                 all cores match the worker core.
//
//  "target-cpu" is deliberately left alone: naming a CPU there also turns on
//  every feature LLVM assumes that CPU has, which would bypass the explicit
//  feature list above.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITTARGET_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITTARGET_H

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class Module;
class StringRef;

namespace ejit {

/// The AArch64 identification registers the JIT target is derived from.
struct EJitCpuIdRegs {
  uint64_t midr = 0;  ///< MIDR_EL1: implementer [31:24], part [15:4].
  uint64_t isar0 = 0; ///< ID_AA64ISAR0_EL1
  uint64_t isar1 = 0; ///< ID_AA64ISAR1_EL1
  uint64_t pfr0 = 0;  ///< ID_AA64PFR0_EL1
};

/// What the JIT adds to every function it compiles. Empty adds nothing.
struct EJitJitTarget {
  std::string tuneCpu;
  std::vector<std::string> features; ///< Each "+name".

  bool empty() const { return tuneCpu.empty() && features.empty(); }
};

/// Read the calling core's identification registers. Returns false where they
/// cannot be read safely: non-AArch64 hosts, and hosted Linux without
/// HWCAP_CPUID (the kernel then does not emulate the EL0 read). Freestanding
/// builds read them directly, which requires running at EL1 or above.
bool readCpuIdRegs(EJitCpuIdRegs &Out);

/// LLVM CPU name for \p Midr, or "" when LLVM does not know the part.
std::string cpuNameFromMidr(uint64_t Midr);

/// The "+feature" entries \p Regs reports, limited to the ones the JIT can use
/// on ordinary code: crc, lse, rdm, dotprod, rcpc, i8mm and fullfp16. SVE is
/// left out on purpose (scalable vectors need their own validation).
std::vector<std::string> featuresFromIdRegs(const EJitCpuIdRegs &Regs);

/// Whether \p CpuCfg / \p FeatureCfg should read the ID registers. \p Hosted
/// builds can ask the OS whether the read is safe (readCpuIdRegs), so there
/// the default detects. Freestanding builds cannot, and the read traps below
/// EL1, so only an explicit CpuCfg "detect" reads there.
bool shouldReadIdRegs(StringRef CpuCfg, StringRef FeatureCfg, bool Hosted);

/// Build \p CpuCfg / \p FeatureCfg into a target from \p Regs (null when the
/// registers were not read, which leaves the detected parts empty):
///   CpuCfg      ""        from MIDR_EL1 (hosted builds only, see above)
///               "detect"  from MIDR_EL1
///               "none"    add nothing at all
///               <name>    use that tune CPU
///   FeatureCfg  ""        no features (tune only)
///               "detect"  featuresFromIdRegs
///               "+a,+b"   exactly those
EJitJitTarget resolveJitTarget(StringRef CpuCfg, StringRef FeatureCfg,
                               const EJitCpuIdRegs *Regs);

/// resolveJitTarget with the build configuration (EJIT_TARGET_CPU /
/// EJIT_TARGET_FEATURES), reading the registers when shouldReadIdRegs allows
/// and logging the result once.
EJitJitTarget resolveConfiguredJitTarget();

/// Add \p T to every defined function in \p M. An existing non-generic
/// "tune-cpu" and any feature the AOT compile already set either way ("+x" or
/// "-x") are kept.
void applyJitTarget(Module &M, const EJitJitTarget &T);

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITTARGET_H
