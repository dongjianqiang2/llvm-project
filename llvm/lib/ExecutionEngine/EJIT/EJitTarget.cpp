//===-- EJitTarget.cpp - Tune JIT code for the core it runs on ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitTarget.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Host.h"

#include <cstdio>

#if defined(__aarch64__) && defined(__linux__) && !defined(EJIT_FREESTANDING)
#include <sys/auxv.h>
#ifndef HWCAP_CPUID
#define HWCAP_CPUID (1UL << 11)
#endif
#endif

// Build configuration (llvm/lib/ExecutionEngine/EJIT/CMakeLists.txt). Unset
// means the default: detect the tune CPU, add no features.
#ifndef EJIT_TARGET_CPU
#define EJIT_TARGET_CPU ""
#endif
#ifndef EJIT_TARGET_FEATURES
#define EJIT_TARGET_FEATURES ""
#endif

using namespace llvm;
using namespace llvm::ejit;

bool llvm::ejit::readCpuIdRegs(EJitCpuIdRegs &Out) {
#if defined(__aarch64__)
#if defined(__linux__) && !defined(EJIT_FREESTANDING)
  // At EL0 these reads trap; Linux emulates them only when it says so.
  if (!(getauxval(AT_HWCAP) & HWCAP_CPUID))
    return false;
#endif
  __asm__ volatile("mrs %0, midr_el1" : "=r"(Out.midr));
  __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(Out.isar0));
  __asm__ volatile("mrs %0, id_aa64isar1_el1" : "=r"(Out.isar1));
  __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(Out.pfr0));
  return true;
#else
  (void)Out;
  return false;
#endif
}

std::string llvm::ejit::cpuNameFromMidr(uint64_t Midr) {
  if (Midr == 0)
    return "";
  // Hand the part to LLVM's own /proc/cpuinfo parser, so the MIDR -> name
  // table stays the one the rest of LLVM uses.
  char Info[64];
  snprintf(Info, sizeof(Info), "CPU implementer\t: 0x%02x\nCPU part\t: 0x%03x\n",
           static_cast<unsigned>((Midr >> 24) & 0xff),
           static_cast<unsigned>((Midr >> 4) & 0xfff));
  StringRef Name = sys::detail::getHostCPUNameForARM(Info);
  return Name == "generic" ? "" : Name.str();
}

std::vector<std::string>
llvm::ejit::featuresFromIdRegs(const EJitCpuIdRegs &Regs) {
  auto Field = [](uint64_t Reg, unsigned Lsb) {
    return static_cast<unsigned>((Reg >> Lsb) & 0xf);
  };
  std::vector<std::string> Features;
  if (Field(Regs.isar0, 16) >= 1) // CRC32
    Features.push_back("+crc");
  if (Field(Regs.isar0, 20) >= 2) // Atomic: 2 = LSE
    Features.push_back("+lse");
  if (Field(Regs.isar0, 28) >= 1) // RDM
    Features.push_back("+rdm");
  if (Field(Regs.isar0, 44) >= 1) // DP
    Features.push_back("+dotprod");
  if (Field(Regs.isar1, 20) >= 1) // LRCPC
    Features.push_back("+rcpc");
  if (Field(Regs.isar1, 52) >= 1) // I8MM
    Features.push_back("+i8mm");
  // FP and AdvSIMD are signed fields (0xf = absent); 1 adds half precision.
  if (Field(Regs.pfr0, 16) == 1 && Field(Regs.pfr0, 20) == 1)
    Features.push_back("+fullfp16");
  return Features;
}

bool llvm::ejit::shouldReadIdRegs(StringRef CpuCfg, StringRef FeatureCfg,
                                  bool Hosted) {
  if (CpuCfg == "detect")
    return true;
  // Without an OS to vouch for the read, the default must not risk a trap.
  if (CpuCfg == "none" || !Hosted)
    return false;
  return CpuCfg.empty() || FeatureCfg == "detect";
}

EJitJitTarget llvm::ejit::resolveJitTarget(StringRef CpuCfg,
                                           StringRef FeatureCfg,
                                           const EJitCpuIdRegs *Regs) {
  EJitJitTarget T;
  if (CpuCfg == "none")
    return T;

  if (!CpuCfg.empty() && CpuCfg != "detect")
    T.tuneCpu = CpuCfg.str();
  else if (Regs)
    T.tuneCpu = cpuNameFromMidr(Regs->midr);

  if (FeatureCfg == "detect") {
    if (Regs)
      T.features = featuresFromIdRegs(*Regs);
  } else {
    SmallVector<StringRef, 8> Parts;
    FeatureCfg.split(Parts, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
    for (StringRef P : Parts) {
      P = P.trim();
      // Only additions: a "-x" here could drop something the AOT code needs.
      if (P.size() > 1 && P.front() == '+')
        T.features.push_back(P.str());
      else if (!P.empty())
        EJIT_DIAG("jit target: ignoring feature \"%s\" (expected +name)",
                  P.str().c_str());
    }
  }
  return T;
}

EJitJitTarget llvm::ejit::resolveConfiguredJitTarget() {
#ifdef EJIT_FREESTANDING
  constexpr bool Hosted = false;
#else
  constexpr bool Hosted = true;
#endif
  EJitCpuIdRegs Regs;
  const bool HaveRegs =
      shouldReadIdRegs(EJIT_TARGET_CPU, EJIT_TARGET_FEATURES, Hosted) &&
      readCpuIdRegs(Regs);
  EJitJitTarget T = resolveJitTarget(EJIT_TARGET_CPU, EJIT_TARGET_FEATURES,
                                     HaveRegs ? &Regs : nullptr);
  // One line, so a "generic" or wrong answer is visible before anyone reads
  // performance numbers into it.
  EJIT_DIAG("jit target: midr=0x%llx tune-cpu=%s features=%s (cpu=\"%s\" "
            "features=\"%s\")",
            static_cast<unsigned long long>(Regs.midr),
            T.tuneCpu.empty() ? "(unchanged)" : T.tuneCpu.c_str(),
            T.features.empty() ? "(none)" : join(T.features, ",").c_str(),
            EJIT_TARGET_CPU, EJIT_TARGET_FEATURES);
  return T;
}

void llvm::ejit::applyJitTarget(Module &M, const EJitJitTarget &T) {
  if (T.empty())
    return;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (!T.tuneCpu.empty()) {
      Attribute Tune = F.getFnAttribute("tune-cpu");
      if (!Tune.isValid() || Tune.getValueAsString() == "generic")
        F.addFnAttr("tune-cpu", T.tuneCpu);
    }
    if (T.features.empty())
      continue;
    SmallVector<StringRef, 16> Feats;
    StringRef(F.getFnAttribute("target-features").getValueAsString())
        .split(Feats, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
    const size_t Before = Feats.size();
    for (const std::string &Add : T.features) {
      StringRef Name = StringRef(Add).drop_front();
      bool Set = any_of(make_range(Feats.begin(), Feats.begin() + Before),
                        [&](StringRef S) { return S.drop_front() == Name; });
      if (!Set)
        Feats.push_back(Add);
    }
    if (Feats.size() != Before)
      F.addFnAttr("target-features", join(Feats, ","));
  }
}
