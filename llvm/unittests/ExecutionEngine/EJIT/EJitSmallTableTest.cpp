//===-- EJitSmallTableTest.cpp - small-table specialization unit tests ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// These tests drive the real pass, the real optimizer pipeline and the real
// ORC engine. The only test-only input is the plan itself: milestone A has no
// production plan construction, row publication or admission path yet, so the
// tests build the plan from a borrowed host array and install it through the
// documented engine seam. Nothing here replaces the compiler with a model.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTable.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableHost.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableRuntime.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

//===----------------------------------------------------------------------===//
// Host SRE platform primitives
//
// LLVMEJIT is compiled with EJIT_SRE_CODE_POOL, and the real platform
// primitives (SRE_MemDbgAlloc / split_2m_to_4k / enable_ex / enable_rw) are
// supplied by the freestanding SRE link environment, never by LLVMEJIT itself
// (see EJitSrePlatform.cpp). A host gtest binary that drives the real ORC
// engine must therefore provide them; without these definitions the link fails
// on the first makeSreCodePoolManager reference. The implementations below use
// the host VM API and keep the SRE contract: 2 MiB-aligned raw memory, true
// RW<->RX transitions, 4 KiB granularity. They are test harness support only;
// the freestanding link still gets the real strong definitions.
//===----------------------------------------------------------------------===//
#if defined(EJIT_SRE_CODE_POOL) && !defined(_WIN32)
#include <cstdlib>
#include <sys/mman.h>

extern "C" void *SRE_MemDbgAlloc(unsigned int, unsigned char,
                                 unsigned long Size, const char *,
                                 unsigned int) {
  void *P = nullptr;
  if (::posix_memalign(&P, static_cast<size_t>(2) << 20,
                       Size != 0 ? Size : 1) != 0)
    return nullptr;
  return P;
}

extern "C" unsigned split_2m_to_4k(unsigned long long, unsigned long long) {
  return 0; // Host mappings already have 4 KiB granularity.
}

extern "C" unsigned enable_ex(unsigned, unsigned long long Va) {
  void *Page = reinterpret_cast<void *>(static_cast<uintptr_t>(Va));
  if (::mprotect(Page, 4096, PROT_READ | PROT_EXEC) != 0)
    return 1;
  __builtin___clear_cache(reinterpret_cast<char *>(Page),
                          reinterpret_cast<char *>(Page) + 4096);
  return 0;
}

extern "C" unsigned enable_rw(unsigned, unsigned long long Va) {
  void *Page = reinterpret_cast<void *>(static_cast<uintptr_t>(Va));
  return ::mprotect(Page, 4096, PROT_READ | PROT_WRITE) == 0 ? 0u : 1u;
}
#elif defined(EJIT_SRE_CODE_POOL) && defined(_WIN32)
// Windows host shim: the counterpart of the POSIX block above, with the same
// SRE contract (2 MiB-aligned raw memory, real RW<->RX page transitions, 4 KiB
// granularity) expressed through the real Windows VM API. Test-harness support
// only: it is compiled into this gtest binary so a Windows host can drive the
// real ORC engine, and it never contributes to the freestanding product link
// (which supplies the real strong definitions, see EJitSrePlatform.cpp).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" void *SRE_MemDbgAlloc(unsigned int, unsigned char,
                                 unsigned long Size, const char *,
                                 unsigned int) {
  const size_t Align = static_cast<size_t>(2) << 20;
  const size_t Need = Size != 0 ? Size : 1;
  // The product's SRE link places the code pool in a fixed low region of the
  // image (EJIT_FIXED_CODE_POOL), not wherever the host allocator happens to put
  // it. On this COFF host that matters for more than address stability: JITLink
  // lowers the COFF .pdata unwind RVAs (IMAGE_REL_AMD64_ADDR32NB) against a zero
  // image base, i.e. as 32-bit absolute references, so a pool above 4 GiB cannot
  // be linked ("relocation target ... is out of range of Pointer32 fixup").
  // Pools are NO_RECLAIM, so reserve one large low 2 MiB-aligned arena on the
  // first request and carve committed chunks out of it instead of racing for a
  // fresh low address per pool; if no low reservation is possible, fall back to
  // an unconstrained allocation (the caller's link then decides).
  static uintptr_t ArenaNext = 0;
  static uintptr_t ArenaEnd = 0;
  if (ArenaNext == 0 && ArenaEnd == 0) {
    const size_t Reserve = static_cast<size_t>(256) << 20;
    for (uintptr_t Base :
         {uintptr_t{0x40000000}, uintptr_t{0x30000000}, uintptr_t{0x20000000},
          uintptr_t{0x10000000}, uintptr_t{0x08000000}}) {
      void *R = ::VirtualAlloc(reinterpret_cast<void *>(Base), Reserve,
                               MEM_RESERVE, PAGE_READWRITE);
      if (R) {
        ArenaNext = reinterpret_cast<uintptr_t>(R);
        ArenaEnd = ArenaNext + Reserve;
        break;
      }
    }
  }
  if (ArenaNext != 0) {
    const uintptr_t Aligned = (ArenaNext + (Align - 1)) & ~(Align - 1);
    if (Aligned + Need <= ArenaEnd) {
      void *P = ::VirtualAlloc(reinterpret_cast<void *>(Aligned), Need,
                               MEM_COMMIT, PAGE_READWRITE);
      if (P) {
        ArenaNext = Aligned + Need;
        return P;
      }
    }
  }
  char *Base = static_cast<char *>(
      ::VirtualAlloc(nullptr, Need + Align, MEM_RESERVE, PAGE_READWRITE));
  if (!Base)
    return nullptr;
  const uintptr_t Aligned =
      (reinterpret_cast<uintptr_t>(Base) + (Align - 1)) & ~(Align - 1);
  void *P = ::VirtualAlloc(reinterpret_cast<void *>(Aligned), Need, MEM_COMMIT,
                           PAGE_READWRITE);
  if (!P) {
    ::VirtualFree(Base, 0, MEM_RELEASE);
    return nullptr;
  }
  return P;
}

extern "C" unsigned split_2m_to_4k(unsigned long long, unsigned long long) {
  return 0; // Host mappings already have 4 KiB granularity.
}

extern "C" unsigned enable_ex(unsigned, unsigned long long Va) {
  void *Page = reinterpret_cast<void *>(static_cast<uintptr_t>(Va));
  DWORD Old = 0;
  if (!::VirtualProtect(Page, 4096, PAGE_EXECUTE_READ, &Old))
    return 1;
  ::FlushInstructionCache(::GetCurrentProcess(), Page, 4096);
  return 0;
}

extern "C" unsigned enable_rw(unsigned, unsigned long long Va) {
  void *Page = reinterpret_cast<void *>(static_cast<uintptr_t>(Va));
  DWORD Old = 0;
  return ::VirtualProtect(Page, 4096, PAGE_READWRITE, &Old) ? 0u : 1u;
}
#endif

using namespace llvm;
using namespace llvm::ejit;

namespace {

constexpr unsigned kCells = 6;
constexpr unsigned kTrps = 2;
constexpr unsigned kPhases = 10;
constexpr uint64_t kElementBytes = 1024;

/// A 1 KiB configuration element with the four authorized/scattered fields at
/// deliberately distant offsets, matching the IR type below.
struct alignas(4) BigElement {
  int32_t gain;      // offset 0
  int32_t pad0[124];
  int32_t mode;      // offset 500
  int32_t pad1[124];
  float scale;       // offset 1000
  int32_t pad2[3];
  int32_t live;      // offset 1016, ordinary (not may_const)
  int32_t pad3;      // offset 1020
};
static_assert(sizeof(BigElement) == kElementBytes, "element must be 1 KiB");

BigElement g_cfg[kCells][kTrps][kPhases];
int32_t g_out[kCells][kTrps];

/// Data-layout fallback for the fixture IR when the host target machine cannot
/// be queried (it normally is: see moduleTargetHeader()).
const char *kDataLayout =
    "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:"
    "64-S128";

/// The `target datalayout` / `target triple` header of every module this suite
/// compiles. The real engine compiles with detectHost()'s target machine
/// (EJitOrcEngine::Create), and LLJIT rejects a module whose data layout
/// differs from the JIT's: on the Windows COFF host an ELF `m:e` fixture fails
/// addIRModule with "Added modules have incompatible data layouts". Deriving
/// both from the same host target keeps one fixture correct on an ELF and a
/// COFF host; the two layouts differ only in mangling (`m:e` vs `m:w`) and every
/// symbol in these tests is a plain C name. This is test-harness support, not a
/// product behavior.
std::string moduleTargetHeader() {
  static const std::string Header = [] {
    InitializeNativeTarget();
    const std::string Triple = sys::getDefaultTargetTriple();
    std::string DL = kDataLayout;
    auto JTMB = orc::JITTargetMachineBuilder::detectHost();
    if (!JTMB) {
      consumeError(JTMB.takeError());
    } else if (auto HostDL = JTMB->getDefaultDataLayoutForTarget()) {
      DL = HostDL->getStringRepresentation();
    } else {
      consumeError(HostDL.takeError());
    }
    return "    target datalayout = \"" + DL + "\"\n    target triple = \"" +
           Triple + "\"\n";
  }();
  return Header;
}

float floatFromBits(uint32_t Bits) {
  float F = 0.0f;
  std::memcpy(&F, &Bits, sizeof(F));
  return F;
}

uint32_t bitsFromFloat(float F) {
  uint32_t Bits = 0;
  std::memcpy(&Bits, &F, sizeof(Bits));
  return Bits;
}

void fillConfig() {
  std::memset(g_cfg, 0, sizeof(g_cfg));
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T)
      for (unsigned P = 0; P < kPhases; ++P) {
        BigElement &E = g_cfg[C][T][P];
        E.gain = static_cast<int32_t>(7 + C * 3 + T * 5 + P);
        // Some rows take the other branch, so a per-row difference is real.
        E.mode = (P % 4 == 0) ? 2 : 1;
        E.live = static_cast<int32_t>(1000 + (C * kTrps + T) * kPhases + P);
        uint32_t Bits =
            0x3f800000u + static_cast<uint32_t>((C * kTrps + T) * 10 + P);
        if (C == 0 && T == 0 && P == 0)
          Bits = 0x80000000u; // -0.0
        if (C == 5 && T == 1 && P == 9)
          Bits = 0x7fc00001u; // NaN with a payload
        E.scale = floatFromBits(Bits);
      }
}

/// The AOT reference for the module below, using the same 1 KiB layout. \p Mod
/// is the real `slotNo % Mod` phase expression the module under test uses.
int32_t aotResultMod(unsigned C, unsigned T, unsigned SlotNo, int32_t X,
                     unsigned Mod) {
  const unsigned P = SlotNo % Mod;
  const BigElement &E = g_cfg[C][T][P];
  const int32_t H =
      static_cast<int32_t>(C * 1000 + T * 100 + (SlotNo % 100)) + X * 17;
  const int32_t FBits = static_cast<int32_t>(bitsFromFloat(E.scale));
  const int32_t Sum = X * E.gain + E.live + H + FBits;
  return E.mode == 1 ? Sum : 0;
}

int32_t aotResult(unsigned C, unsigned T, unsigned SlotNo, int32_t X) {
  return aotResultMod(C, T, SlotNo, X, kPhases);
}

//===----------------------------------------------------------------------===//
// Fully ready uniform-contract fixture
//
// A domain where one authorized field is a genuine invariant (explicitly
// contracted by the caller) and the other two vary per row. The folded value
// stays observable in the JIT result, so the uniform path is proven on a
// complete plan rather than inferred from visible rows.
//===----------------------------------------------------------------------===//

constexpr unsigned kUniformCells = 2;

struct alignas(4) UniformElement {
  int32_t mode; // offset 0, contracted to 7 for every row
  int32_t gain; // offset 4, varies
  float scale;  // offset 8, varies
};
static_assert(sizeof(UniformElement) == 12, "layout must match the IR type");

UniformElement g_u[kUniformCells][kPhases];
int32_t gu_out[kUniformCells];

void fillUniformConfig() {
  for (unsigned C = 0; C < kUniformCells; ++C)
    for (unsigned P = 0; P < kPhases; ++P) {
      g_u[C][P].mode = 7;
      g_u[C][P].gain = static_cast<int32_t>(100 + C * 10 + P);
      g_u[C][P].scale =
          floatFromBits(0x40000000u + static_cast<uint32_t>(C * 10 + P));
    }
  std::memset(gu_out, 0, sizeof(gu_out));
}

int32_t aotUniform(unsigned C, unsigned P, int32_t X) {
  const UniformElement &E = g_u[C][P];
  return X * E.gain + static_cast<int32_t>(bitsFromFloat(E.scale));
}

std::string uniformModuleText() {
  return moduleTargetHeader() + R"(

    %U = type { i32, i32, float }

    @g_u = external global [2 x [10 x %U]]
    @gu_out = external global [2 x i32]

    define i32 @u_entry(i32 %cell, i32 %slotNo, i32 %x) !ejit.metadata !0 {
    entry:
      %ph = urem i32 %slotNo, 10
      %row = getelementptr inbounds [2 x [10 x %U]], ptr @g_u, i64 0, i32 %cell, i32 %ph
      %p0 = getelementptr inbounds %U, ptr %row, i32 0, i32 0
      %mode = load i32, ptr %p0, align 4, !ejit.may_const !1
      %p1 = getelementptr inbounds %U, ptr %row, i32 0, i32 1
      %gain = load i32, ptr %p1, align 4, !ejit.may_const !1
      %p2 = getelementptr inbounds %U, ptr %row, i32 0, i32 2
      %scale = load float, ptr %p2, align 4, !ejit.may_const !1
      %outp = getelementptr inbounds [2 x i32], ptr @gu_out, i64 0, i32 %cell
      store i32 %slotNo, ptr %outp, align 4
      %m = mul i32 %x, %gain
      %fb = bitcast float %scale to i32
      %s = add i32 %m, %fb
      %ok = icmp eq i32 %mode, 7
      %res = select i1 %ok, i32 %s, i32 -1
      ret i32 %res
    }

    !0 = !{!2}
    !1 = !{}
    !2 = !{!"ejit_entry"}
  )";
}

std::string moduleText() {
  return moduleTargetHeader() + R"(

    %Big = type { i32, [124 x i32], i32, [124 x i32], float, [3 x i32], i32, i32 }

    @g_cfg = external global [6 x [2 x [10 x %Big]]], !ejit.metadata !10
    @g_out = external global [6 x [2 x i32]]

    define internal i32 @stab_helper(i32 %c, i32 %t, i32 %s, i32 %x) {
    entry:
      %a = mul i32 %c, 1000
      %b = mul i32 %t, 100
      %c2 = urem i32 %s, 100
      %d = mul i32 %x, 17
      %e = add i32 %a, %b
      %f = add i32 %e, %c2
      %g = add i32 %f, %d
      ret i32 %g
    }

    define i32 @stab_entry(i32 %cell, i32 %trp, i32 %slotNo, i32 %x) !ejit.metadata !0 {
    entry:
      %phase = urem i32 %slotNo, 10
      %row = getelementptr inbounds [6 x [2 x [10 x %Big]]], ptr @g_cfg, i64 0, i32 %cell, i32 %trp, i32 %phase
      %gainp = getelementptr inbounds %Big, ptr %row, i32 0, i32 0
      %gain = load i32, ptr %gainp, align 4, !ejit.may_const !1
      %modep = getelementptr inbounds %Big, ptr %row, i32 0, i32 2
      %mode = load i32, ptr %modep, align 4, !ejit.may_const !1
      %scalep = getelementptr inbounds %Big, ptr %row, i32 0, i32 4
      %scale = load float, ptr %scalep, align 4, !ejit.may_const !1
      %livep = getelementptr inbounds %Big, ptr %row, i32 0, i32 6
      %live = load i32, ptr %livep, align 4
      %outp = getelementptr inbounds [6 x [2 x i32]], ptr @g_out, i64 0, i32 %cell, i32 %trp
      store i32 %slotNo, ptr %outp, align 4
      %h = call i32 @stab_helper(i32 %cell, i32 %trp, i32 %slotNo, i32 %x)
      %mul = mul i32 %x, %gain
      %sum = add i32 %mul, %live
      %sum2 = add i32 %sum, %h
      %fbits = bitcast float %scale to i32
      %sum3 = add i32 %sum2, %fbits
      %isone = icmp eq i32 %mode, 1
      %res = select i1 %isone, i32 %sum3, i32 0
      ret i32 %res
    }

    !0 = !{!2, !3}
    !1 = !{}
    !2 = !{!"ejit_entry"}
    !3 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !4 = !{!"ejit_period_arr", !"cell", i64 6}
    !5 = !{!"ejit_may_const_field", i64 0}
    !6 = !{!"ejit_may_const_field", i64 500}
    !7 = !{!"ejit_may_const_field", i64 1000}
    !10 = !{!4, !5, !6, !7}
  )";
}

/// A module whose entry contains the shapes the pass must refuse: volatile,
/// atomic, non-inbounds dynamic address, a pointer-typed load, a non-zero
/// address-space load and an `inttoptr`-rooted load, plus one ordinary
/// may_const load that must still be replaced.
std::string badShapesText() {
  return moduleTargetHeader() + R"(

    %Big = type { i32, [124 x i32], i32, [124 x i32], float, [3 x i32], i32, i32 }
    @g_cfg = external global [6 x [2 x [10 x %Big]]]
    @g_cfg_as1 = external addrspace(1) global [6 x [2 x [10 x %Big]]]

    define i32 @stab_entry(i32 %cell, i32 %trp, i32 %slotNo) !ejit.metadata !0 {
    entry:
      %phase = urem i32 %slotNo, 10
      %row = getelementptr inbounds [6 x [2 x [10 x %Big]]], ptr @g_cfg, i64 0, i32 %cell, i32 %trp, i32 %phase
      %gp = getelementptr inbounds %Big, ptr %row, i32 0, i32 0
      %good = load i32, ptr %gp, align 4, !ejit.may_const !1
      %vol = load volatile i32, ptr %gp, align 4, !ejit.may_const !1
      %atm = load atomic i32, ptr %gp monotonic, align 4, !ejit.may_const !1
      %np = getelementptr [6 x [2 x [10 x %Big]]], ptr @g_cfg, i64 0, i32 %cell, i32 %trp, i32 %phase
      %g2 = getelementptr inbounds %Big, ptr %np, i32 0, i32 0
      %nv = load i32, ptr %g2, align 4, !ejit.may_const !1
      %pp = getelementptr inbounds %Big, ptr %row, i32 0, i32 0
      %pv = load ptr, ptr %pp, align 8, !ejit.may_const !1
      %asp = getelementptr inbounds [6 x [2 x [10 x %Big]]], ptr addrspace(1) @g_cfg_as1, i64 0, i32 %cell, i32 %trp, i32 %phase
      %asv = load i32, ptr addrspace(1) %asp, align 4, !ejit.may_const !1
      %ip = inttoptr i64 0 to ptr
      %iv = load i32, ptr %ip, align 4, !ejit.may_const !1
      %s1 = add i32 %good, %vol
      %s2 = add i32 %s1, %atm
      %s3 = add i32 %s2, %nv
      %c = ptrtoint ptr %pv to i32
      %s4 = add i32 %s3, %c
      %s5 = add i32 %s4, %asv
      %s6 = add i32 %s5, %iv
      ret i32 %s6
    }

    !0 = !{!2}
    !1 = !{}
    !2 = !{!"ejit_entry"}
  )";
}

//===----------------------------------------------------------------------===//
// Automatic per-field specialization fixture (spec §4.1 default path)
//
// Four authorized fields over one (cell, TRP) domain: `mode` is a genuine
// invariant of the whole domain, `byCell` varies with the cell axis only,
// `byTrp` with the TRP axis only, `joint` with both. The required automatic
// planner must fold the first with no column/payload/load, keep one-axis
// projections for the next two, and the joint table for the last — with no
// caller-supplied uniform value anywhere.
//===----------------------------------------------------------------------===//

constexpr unsigned kAutoCells = 4;
constexpr unsigned kAutoTrps = 3;

struct alignas(4) AutoElement {
  int32_t mode;   // offset 0, identical on the whole domain
  int32_t byCell; // offset 4, differs per cell only
  int32_t byTrp;  // offset 8, differs per TRP only
  int32_t joint;  // offset 12, differs jointly
};
static_assert(sizeof(AutoElement) == 16, "layout must match the IR type");

AutoElement g_auto[kAutoCells][kAutoTrps];
AutoElement g_sparse[2][2];
AutoElement g_one[1][1];
int32_t g_auto_out[kAutoCells];
int32_t g_sparse_out[2];
int32_t g_one_out[1];

/// The product-shape fixture (16 cells x 32 TRPs, the stated ceiling for this
/// entry class): the runtime, planner and resource must not be hardcoded to the
/// tiny 4x3 test module.
AutoElement g_big[16][32];
int32_t g_big_out[16];

/// The exact-float-bits fixture's source and output (see
/// `AutomaticSolverFoldsExactFloatBits`).
struct alignas(4) FloatElement {
  float f;
  int32_t i;
};
FloatElement g_f[2][2];
int32_t g_f_out[2];

void fillAutoConfig() {
  for (unsigned C = 0; C < kAutoCells; ++C)
    for (unsigned T = 0; T < kAutoTrps; ++T) {
      AutoElement &E = g_auto[C][T];
      E.mode = 1;
      E.byCell = static_cast<int32_t>(7 + C);
      E.byTrp = static_cast<int32_t>(2 + T);
      E.joint = static_cast<int32_t>(C * 3 + T);
    }
  std::memset(g_auto_out, 0, sizeof(g_auto_out));
}

/// The 2x2 sparse/checkerboard fixture. Default values are the checkerboard
/// `joint = C*3 + T`; individual tests overwrite it.
void fillSparseConfig() {
  for (unsigned C = 0; C < 2; ++C)
    for (unsigned T = 0; T < 2; ++T) {
      AutoElement &E = g_sparse[C][T];
      E.mode = 1;
      E.byCell = static_cast<int32_t>(7 + C * 2);
      E.byTrp = 5;
      E.joint = static_cast<int32_t>(C * 3 + T);
    }
  std::memset(g_sparse_out, 0, sizeof(g_sparse_out));
}

/// The 16x32 product-shape fixture (see `g_big`).
void fillBigConfig() {
  for (unsigned C = 0; C < 16; ++C)
    for (unsigned T = 0; T < 32; ++T) {
      AutoElement &E = g_big[C][T];
      E.mode = 1;
      E.byCell = static_cast<int32_t>(7 + C);
      E.byTrp = static_cast<int32_t>(2 + T);
      E.joint = static_cast<int32_t>(C * 32 + T);
    }
  std::memset(g_big_out, 0, sizeof(g_big_out));
}

/// One entry over `[Cells x [Trps x %A]]`, whose result is a positional mix of
/// all four authorized fields plus real dynamic work, so a wrong projected index
/// or a wrongly folded field changes the observable result.
int32_t aotAuto(const AutoElement &E, int32_t X) {
  return E.mode * 1000 + E.byCell * 100 + E.byTrp * 10 + E.joint + X * 3;
}

std::string autoModuleText(StringRef Entry, StringRef Global, StringRef Out,
                           unsigned Cells, unsigned Trps) {
  const std::string CA = Twine(Cells).str();
  const std::string TA = Twine(Trps).str();
  std::string Text = moduleTargetHeader();
  Text += "\n    %A = type { i32, i32, i32, i32 }\n";
  Text += "    @" + Global.str() + " = external global [" + CA + " x [" + TA +
          " x %A]]\n";
  Text += "    @" + Out.str() + " = external global [" + CA + " x i32]\n";
  Text += "\n    define i32 @" + Entry.str() +
          "(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {\n";
  Text += "    entry:\n";
  Text += "      %row = getelementptr inbounds [" + CA + " x [" + TA +
          " x %A]], ptr @" + Global.str() +
          ", i64 0, i32 %cell, i32 %trp\n";
  Text += "      %p0 = getelementptr inbounds %A, ptr %row, i32 0, i32 0\n";
  Text += "      %mode = load i32, ptr %p0, align 4, !ejit.may_const !1\n";
  Text += "      %p1 = getelementptr inbounds %A, ptr %row, i32 0, i32 1\n";
  Text += "      %bycell = load i32, ptr %p1, align 4, !ejit.may_const !1\n";
  Text += "      %p2 = getelementptr inbounds %A, ptr %row, i32 0, i32 2\n";
  Text += "      %bytrp = load i32, ptr %p2, align 4, !ejit.may_const !1\n";
  Text += "      %p3 = getelementptr inbounds %A, ptr %row, i32 0, i32 3\n";
  Text += "      %joint = load i32, ptr %p3, align 4, !ejit.may_const !1\n";
  Text += "      %outp = getelementptr inbounds [" + CA + " x i32], ptr @" +
          Out.str() + ", i64 0, i32 %cell\n";
  Text += "      store i32 %x, ptr %outp, align 4\n";
  Text += "      %m1 = mul i32 %mode, 1000\n";
  Text += "      %m2 = mul i32 %bycell, 100\n";
  Text += "      %m3 = mul i32 %bytrp, 10\n";
  Text += "      %s1 = add i32 %m1, %m2\n";
  Text += "      %s2 = add i32 %s1, %m3\n";
  Text += "      %s3 = add i32 %s2, %joint\n";
  Text += "      %xm = mul i32 %x, 3\n";
  Text += "      %sum = add i32 %s3, %xm\n";
  Text += "      %isone = icmp eq i32 %mode, 1\n";
  Text += "      %res = select i1 %isone, i32 %sum, i32 -1\n";
  Text += "      ret i32 %res\n";
  Text += "    }\n\n    !0 = !{!2}\n    !1 = !{}\n";
  Text += "    !2 = !{!\"ejit_entry\"}\n";
  return Text;
}

/// Every (cell, trp) row of \p Cells x \p Trps confirmed ready.
SmallVector<EJitSmallTableRowKey, 16> autoRows(unsigned Cells, unsigned Trps) {
  SmallVector<EJitSmallTableRowKey, 16> Rows;
  for (unsigned C = 0; C < Cells; ++C)
    for (unsigned T = 0; T < Trps; ++T)
      Rows.push_back({{C, T}});
  return Rows;
}

SmallVector<EJitSmallTableDim, 2> autoDims(unsigned Cells, unsigned Trps) {
  SmallVector<EJitSmallTableDim, 2> Dims;
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, Cells});
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 1, 0, Trps});
  return Dims;
}

/// A clearly labeled compiler-test readiness provider. Milestone A has no
/// production configuration completion point (B0), so every A1 test states the
/// provider explicitly; nothing here claims a product capability.
EJitSmallTableReadiness testReadiness() {
  EJitSmallTableReadiness R;
  R.domainEpoch = 0x5EED20260914ull;
  R.providerLabel = "test.provider.compiler-boundary";
  R.coversDeclaredDomain = true;
  R.borrowedStable = true;
  return R;
}

SmallVector<EJitSmallTableDim, 4> planDims() {
  SmallVector<EJitSmallTableDim, 4> Dims;
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, kCells});
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 1, 0, kTrps});
  Dims.push_back(
      {EJitSmallTableDim::Kind::ModuloArgument, 2, kPhases, kPhases});
  return Dims;
}

/// Every (cell, trp, phase) row of the six active members is confirmed ready.
SmallVector<EJitSmallTableRowKey, 128> allRows() {
  SmallVector<EJitSmallTableRowKey, 128> Rows;
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T)
      for (unsigned P = 0; P < kPhases; ++P)
        Rows.push_back({{C, T, P}});
  return Rows;
}

const GlobalVariable *rootGVOf(const Value *V) {
  V = V->stripPointerCasts();
  while (const auto *GEP = dyn_cast<GEPOperator>(V))
    V = GEP->getPointerOperand()->stripPointerCasts();
  return dyn_cast<GlobalVariable>(V);
}

unsigned countLoadsRootedAt(const Function &F, StringRef GVName) {
  unsigned Count = 0;
  for (const Instruction &I : instructions(F)) {
    const auto *LI = dyn_cast<LoadInst>(&I);
    if (!LI)
      continue;
    if (const GlobalVariable *GV = rootGVOf(LI->getPointerOperand()))
      if (GV->getName() == GVName)
        ++Count;
  }
  return Count;
}

unsigned countTableLoads(const Function &F, StringRef Prefix) {
  unsigned Count = 0;
  for (const Instruction &I : instructions(F)) {
    const auto *LI = dyn_cast<LoadInst>(&I);
    if (!LI)
      continue;
    if (const GlobalVariable *GV = rootGVOf(LI->getPointerOperand()))
      if (GV->getName().starts_with(Prefix))
        ++Count;
  }
  return Count;
}

unsigned countAllLoads(const Function &F) {
  unsigned Count = 0;
  for (const Instruction &I : instructions(F))
    Count += isa<LoadInst>(&I);
  return Count;
}

/// Loads carrying the small-table provenance tag (§9).
unsigned countTaggedTableLoads(const Function &F) {
  unsigned Count = 0;
  for (const Instruction &I : instructions(F))
    if (const auto *LI = dyn_cast<LoadInst>(&I))
      Count += LI->hasMetadata("ejit.smalltable.load");
  return Count;
}

/// Scalar width N of an integer column: the storage element is the promoted
/// type, while the typed value is the low N bits.
uint64_t truncateToBits(uint64_t V, unsigned Bits) {
  return Bits >= 64 ? V : (V & ((uint64_t{1} << Bits) - 1));
}

//===----------------------------------------------------------------------===//
// Scalar-width fixture (R1 F1/F2 regression)
//
// One element whose first byte is read through three different integer widths
// at the same offset (i1, i8, i16) and whose offset 8 is read through a
// non-byte-aligned width (i9, two bytes of storage). The bytes are deliberately
// non-canonical: byte 0 is 0xFE, so an i1 read must yield 1 and not 0xFE, an i9
// read must drop the padding bits above bit 8, and an i8 read must keep them.
//===----------------------------------------------------------------------===//

constexpr unsigned kWidthCells = 2;
constexpr unsigned kWidthPhases = 4;

struct alignas(4) WidthElement {
  uint8_t bits;   // offset 0: load i1, load i8, load i16
  uint8_t pad;    // offset 1
  uint16_t small; // offset 2
  uint16_t sub;   // offset 4, read as i9 through [2 x i8] at offset 4
  uint32_t live;  // offset 8, ordinary (not may_const)
  uint32_t pad2;  // offset 12
};
static_assert(sizeof(WidthElement) == 16, "layout must match the IR type");

WidthElement g_width[kWidthCells][kWidthPhases];
int32_t g_width_free[kWidthPhases];

void fillWidthConfig() {
  for (unsigned C = 0; C < kWidthCells; ++C)
    for (unsigned P = 0; P < kWidthPhases; ++P) {
      WidthElement &E = g_width[C][P];
      // All rows share the low bit of `bits`, so that bit is itself a genuine
      // per-row invariant an explicit i1 contract can name and be confirmed by;
      // the padding byte, the i8/i16 views and the i9 view still carry
      // non-canonical bits that a raw byte copy would leak.
      E.bits = 0xFE;
      E.pad = static_cast<uint8_t>(0x5A + P);
      E.small = static_cast<uint16_t>(0x12FF + P);
      E.sub = static_cast<uint16_t>(0x0014 + P * 0x20);
      E.live = static_cast<uint32_t>(0x700 + C * 10 + P);
      E.pad2 = 0;
    }
  // A different value per phase, so a frozen compile-time fold is observable.
  for (unsigned P = 0; P < kWidthPhases; ++P)
    g_width_free[P] = static_cast<int32_t>(0x500 + P);
}

/// One entry over a [2 x [4 x %W]] source. Offset 4 is read as an i9 through a
/// `[2 x i8]` view, so the load's storage is two bytes while its typed value is
/// nine bits (the alignment on the GEP is explicit: the source field is 2-byte
/// aligned and i9's ABI alignment is 1, so a default-aligned i9 load there would
/// be over-aligned).
std::string widthModuleText() {
  return moduleTargetHeader() + R"(

    %W = type { i8, i8, i16, [2 x i8], i32, i32 }

    @g_width = external global [2 x [4 x %W]]
    @g_width_free = external global [4 x i32], !ejit.metadata !10

    define i32 @w_entry(i32 %cell, i32 %phase) !ejit.metadata !0 {    entry:
      %row = getelementptr inbounds [2 x [4 x %W]], ptr @g_width, i64 0, i32 %cell, i32 %phase
      %b0 = getelementptr inbounds %W, ptr %row, i32 0, i32 0
      %v0 = load i1, ptr %b0, align 1, !ejit.may_const !1
      %v1 = load i8, ptr %b0, align 1, !ejit.may_const !1
      %v2 = load i16, ptr %b0, align 2, !ejit.may_const !1
      %b3 = getelementptr inbounds %W, ptr %row, i32 0, i32 3
      %v3 = load i9, ptr %b3, align 2, !ejit.may_const !1
      %livep = getelementptr inbounds %W, ptr %row, i32 0, i32 4
      %live = load i32, ptr %livep, align 4
      %fp = getelementptr inbounds [4 x i32], ptr @g_width_free, i64 0, i64 0
      %free = load i32, ptr %fp, align 4, !ejit.may_const !1
      %z0 = zext i1 %v0 to i32
      %z1 = zext i8 %v1 to i32
      %z2 = zext i16 %v2 to i32
      %z3 = zext i9 %v3 to i32
      %a = add i32 %z0, %z1
      %b = add i32 %a, %z2
      %c = add i32 %b, %z3
      %d = add i32 %c, %live
      %e = add i32 %d, %free
      ret i32 %e
    }

    !0 = !{!2, !3}
    !1 = !{}
    !2 = !{!"ejit_entry"}
    !3 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !4 = !{!"ejit_period_arr", !"free", i64 4}
    !5 = !{!"ejit_may_const_field", i64 0}
    !10 = !{!4, !5}
  )";
}

/// Extract the scalar constant an aggregate element holds, whether the
/// initializer folded to ConstantDataArray or stayed a ConstantArray.
uint64_t elementBits(Constant *Aggregate, unsigned Index) {
  Constant *Elt = Aggregate->getAggregateElement(Index);
  if (auto *CI = dyn_cast_or_null<ConstantInt>(Elt))
    return CI->getValue().getZExtValue();
  if (auto *CF = dyn_cast_or_null<ConstantFP>(Elt))
    return CF->getValueAPF().bitcastToAPInt().getZExtValue();
  return std::numeric_limits<uint64_t>::max();
}

class SmallTableTest : public testing::Test {
protected:
  LLVMContext Ctx;
  EJitRuntimeState State;
  /// The specialization context must outlive every lookup that can trigger
  /// materialization, because the engine keeps a pointer to it while compiling.
  SpecializationContext compileCtx_;

  std::unique_ptr<Module> parseModule() {
    SMDiagnostic Err;
    auto M = parseAssemblyString(moduleText(), Err, Ctx);
    if (!M)
      Err.print("SmallTableTest", errs());
    return M;
  }

  std::unique_ptr<Module> parseBadShapes() {
    SMDiagnostic Err;
    auto M = parseAssemblyString(badShapesText(), Err, Ctx);
    if (!M)
      Err.print("SmallTableTest", errs());
    return M;
  }

  void SetUp() override {
    fillConfig();
    fillUniformConfig();
    fillWidthConfig();
    fillAutoConfig();
    fillSparseConfig();
    fillBigConfig();
    // Outputs are process-wide; a previous test's stores must not leak into
    // this test's address checks.
    std::memset(g_out, 0, sizeof(g_out));
  }

  std::shared_ptr<const EJitSmallTablePlanSet>
  makePlanSet(const Module &M,
              ArrayRef<std::optional<uint64_t>> Contracts = {},
              ArrayRef<EJitSmallTableRowKey> Rows = {},
              EJitSmallTablePlanMode Mode = EJitSmallTablePlanMode::Automatic) {
    std::string Error;
    SmallVector<EJitSmallTableRowKey, 128> DefaultRows = allRows();
    ArrayRef<EJitSmallTableRowKey> UseRows =
        Rows.empty() ? ArrayRef<EJitSmallTableRowKey>(DefaultRows) : Rows;
    SmallVector<EJitSmallTableDim, 4> Dims = planDims();
    EJitSmallTableRequest Req;
    Req.module = &M;
    Req.entryName = "stab_entry";
    Req.sourceVarName = "g_cfg";
    Req.dims = Dims;
    Req.source = EJitSmallTableSource{
        reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]), sizeof(g_cfg)};
    Req.authorizedRows = UseRows;
    Req.uniformContracts = Contracts;
    Req.mode = Contracts.empty() ? Mode : EJitSmallTablePlanMode::ExplicitContracts;
    auto Plan = EJitSmallTablePlanner::plan(Req, Error);
    EXPECT_TRUE(Plan.has_value()) << Error;
    auto Set = std::make_shared<EJitSmallTablePlanSet>();
    if (Plan)
      Set->add(std::make_shared<const EJitSmallTablePlan>(std::move(*Plan)));
    return Set;
  }

  /// The scalar-width fixture's plan: two argument axes, one fully ready row per
  /// (cell, phase), explicitly selected pure-table mode (every declared axis
  /// retained) because these tests pin the width-exact column identity rather
  /// than the automatic solver. The automatic solver's own width behavior is
  /// covered by `AutomaticWidthsKeepTheirTypedWidths`.
  ///
  /// The second axis is a plain argument rather than `urem(slot, 4)`: the
  /// planner only accepts the `urem` spelling, and InstCombine canonicalizes a
  /// power-of-two `urem` into `and`, so the real pipeline's post-InstCombine
  /// matcher would refuse the axis. That power-of-two modulo/AND gap is the
  /// deferred R2 item; this fixture deliberately does not depend on it.
  std::shared_ptr<const EJitSmallTablePlanSet> makeWidthPlanSet(const Module &M) {
    SmallVector<EJitSmallTableDim, 2> Dims;
    Dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, kWidthCells});
    Dims.push_back({EJitSmallTableDim::Kind::Argument, 1, 0, kWidthPhases});
    SmallVector<EJitSmallTableRowKey, 8> WRows;
    for (unsigned C = 0; C < kWidthCells; ++C)
      for (unsigned P = 0; P < kWidthPhases; ++P)
        WRows.push_back({{C, P}});
    std::string Error;
    EJitSmallTableRequest Req;
    Req.module = &M;
    Req.entryName = "w_entry";
    Req.sourceVarName = "g_width";
    Req.dims = Dims;
    Req.source = EJitSmallTableSource{
        reinterpret_cast<const uint8_t *>(&g_width[0][0]), sizeof(g_width)};
    Req.authorizedRows = WRows;
    Req.mode = EJitSmallTablePlanMode::ExplicitContracts;
    auto Plan = EJitSmallTablePlanner::plan(Req, Error);
    EXPECT_TRUE(Plan.has_value()) << Error;
    auto Set = std::make_shared<EJitSmallTablePlanSet>();
    if (Plan)
      Set->add(std::make_shared<const EJitSmallTablePlan>(std::move(*Plan)));
    return Set;
  }

  SpecializationContext baselineCtx(StringRef EntryName = "stab_entry") {
    SpecializationContext C;
    C.fnName = EntryName.str();
    C.cacheKey = 0x57ab1e;
    C.optLevel = OptimizationLevel::L2;
    C.tier = CompileTier::Baseline;
    return C;
  }

  /// Compile \p M through the real engine exactly like
  /// `EJitCompileDriver::compile`: install the context, load the module, then
  /// resolve the entry *while the context is still active*. LLJIT materializes
  /// modules lazily, and the engine's IR transform (the specialization
  /// pipeline, including the small-table pass) runs inside that materialization,
  /// so the entry lookup is what actually compiles the module. \p Plans may be
  /// null (feature OFF).
  std::unique_ptr<EJitOrcEngine>
  compileWithEngine(Module &M,
                    std::shared_ptr<const EJitSmallTablePlanSet> Plans,
                    PeriodArrayRegistry &Registry, uint64_t Key,
                    StringRef EntryName = "stab_entry") {
    std::string Bitcode;
    raw_string_ostream OS(Bitcode);
    WriteBitcodeToFile(M, OS);
    OS.flush();

    Config Cfg;
    auto EngineOrErr = EJitOrcEngine::Create(Cfg, Registry, State);
    EXPECT_TRUE(static_cast<bool>(EngineOrErr));
    if (!EngineOrErr)
      return nullptr;
    auto Engine = std::move(*EngineOrErr);
    if (Plans)
      Engine->setSmallTablePlans(std::move(Plans));
    compileCtx_ = baselineCtx(EntryName);
    compileCtx_.cacheKey = Key;
    Engine->setActiveContext(&compileCtx_);
    EXPECT_FALSE(
        errorToBool(Engine->loadBitcodeModule(Bitcode, Key, compileCtx_.fnName)));
    auto FnOrErr = Engine->lookup(Key, compileCtx_.fnName);
    if (!FnOrErr) {
      ADD_FAILURE() << "materialize/compile failed: "
                    << toString(FnOrErr.takeError());
      Engine->setActiveContext(nullptr);
      return nullptr;
    }
    // The shared taskpool's batch-flush thunk seals the deferred 4K pages at
    // publication; a direct engine user must do the same before calling.
#if defined(EJIT_SRE_CODE_POOL)
    EXPECT_FALSE(errorToBool(Engine->flushPendingCode()));
#endif
    Engine->setActiveContext(nullptr);
    return Engine;
  }

  /// Re-resolve \p Name from an already compiled module and flush any pending
  /// batch seals. The first compile happened in compileWithEngine.
  template <typename FnT>
  FnT lookupSealed(EJitOrcEngine &Engine, uint64_t Key, StringRef Name) {
    auto FnOrErr = Engine.lookup(Key, Name.str());
    EXPECT_TRUE(static_cast<bool>(FnOrErr)) << toString(FnOrErr.takeError());
    if (!FnOrErr)
      return nullptr;
#if defined(EJIT_SRE_CODE_POOL)
    EXPECT_FALSE(errorToBool(Engine.flushPendingCode()));
#endif
    return reinterpret_cast<FnT>(*FnOrErr);
  }

  PeriodArrayRegistry &makeRegistry() {
    PeriodArrayRegistry &Registry = State.getRegistry();
    Registry.registerArray("cell", "g_cfg",
                           reinterpret_cast<void *>(&g_cfg[0][0][0]),
                           sizeof(g_cfg));
    Registry.registerStaticVar("g_out", &g_out[0][0]);
    // The scalar-width fixture's own source and free/global source. The free
    // array is what the legacy period-registry fold can resolve, so the
    // whole-entry readiness test can prove the fold is blocked.
    Registry.registerArray("cell", "g_width",
                           reinterpret_cast<void *>(&g_width[0][0]),
                           sizeof(g_width));
    Registry.registerStaticVar("g_width_free", &g_width_free[0]);
    // Also as a period array: the legacy period fold resolves an array base by
    // its period name, so this is what makes the whole-entry readiness test's
    // `g_width_free` load foldable in the baseline pipeline.
    Registry.registerArray("free", "g_width_free",
                           reinterpret_cast<void *>(&g_width_free[0]),
                           sizeof(g_width_free));
    // The uniform-contract fixture's own source and output globals.
    Registry.registerArray("cell", "g_u",
                           reinterpret_cast<void *>(&g_u[0][0]), sizeof(g_u));
    Registry.registerStaticVar("gu_out", &gu_out[0]);
    // The automatic-solver fixtures.
    Registry.registerArray("cell", "g_auto",
                           reinterpret_cast<void *>(&g_auto[0][0]),
                           sizeof(g_auto));
    Registry.registerStaticVar("g_auto_out", &g_auto_out[0]);
    Registry.registerArray("cell", "g_sparse",
                           reinterpret_cast<void *>(&g_sparse[0][0]),
                           sizeof(g_sparse));
    Registry.registerStaticVar("g_sparse_out", &g_sparse_out[0]);
    Registry.registerArray("cell", "g_one",
                           reinterpret_cast<void *>(&g_one[0][0]),
                           sizeof(g_one));
    Registry.registerStaticVar("g_one_out", &g_one_out[0]);
    // The 16x32 product-shape fixture (the runtime must not be hardcoded to
    // the tiny module).
    Registry.registerArray("cell", "g_big",
                           reinterpret_cast<void *>(&g_big[0][0]),
                           sizeof(g_big));
    Registry.registerStaticVar("g_big_out", &g_big_out[0]);
    return Registry;
  }
};

//===----------------------------------------------------------------------===//
// Planner
//===----------------------------------------------------------------------===//

TEST_F(SmallTableTest, PlannerBuildsColumnsAndRowsFromMemory) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]),
                           sizeof(g_cfg)},
      allRows(), {}, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;

  EXPECT_EQ(Plan->entryName, "stab_entry");
  EXPECT_EQ(Plan->elementBytes, kElementBytes);
  ASSERT_EQ(Plan->fields.size(), 3u);
  EXPECT_EQ(Plan->fields[0].sourceOffset, 0u);
  EXPECT_EQ(Plan->fields[1].sourceOffset, 500u);
  EXPECT_EQ(Plan->fields[2].sourceOffset, 1000u);
  EXPECT_EQ(Plan->fields[2].kind, EJitSmallTableKind::Float);
  EXPECT_EQ(Plan->numRows(), kCells * kTrps * kPhases);
  EXPECT_TRUE(Plan->allRowsReady());
  ASSERT_EQ(Plan->rows.size(), kCells * kTrps * kPhases);

  // Row (4, 1, 3): the copied bits must equal the source values, and the float
  // column must carry the exact bit pattern.
  const uint64_t Row = (4 * kTrps + 1) * kPhases + 3;
  EXPECT_TRUE(Plan->rows[Row].ready);
  EXPECT_EQ(Plan->rows[Row].bits[0],
            static_cast<uint64_t>(g_cfg[4][1][3].gain));
  EXPECT_EQ(Plan->rows[Row].bits[1],
            static_cast<uint64_t>(g_cfg[4][1][3].mode));
  EXPECT_EQ(Plan->rows[Row].bits[2],
            static_cast<uint64_t>(bitsFromFloat(g_cfg[4][1][3].scale)));
  EXPECT_TRUE(Plan->isConsistent(&Error)) << Error;

  // Condensation: the emitted payload is only the scales and the axis products
  // each field actually needs, instead of one 1 KiB element per row. `mode`
  // depends on the phase alone, so its column keeps one axis (10 rows) while
  // `gain` and `scale` differ jointly and keep all three.
  EXPECT_EQ(Plan->fields[0].retainedAxes.size(), 3u);
  EXPECT_EQ(Plan->fields[1].retainedAxes.size(), 1u);
  EXPECT_EQ(Plan->fields[1].retainedAxes[0], 2u);
  EXPECT_EQ(Plan->fields[1].tableRows, kPhases);
  EXPECT_EQ(Plan->fields[1].tableBytes, kPhases * 4);
  EXPECT_EQ(Plan->fields[2].retainedAxes.size(), 3u);
  uint64_t FullBytes = 0;
  for (const EJitSmallTableField &Field : Plan->fields)
    FullBytes += Plan->numRows() * Field.accessSize;
  EXPECT_EQ(FullBytes, kCells * kTrps * kPhases * (4 + 4 + 4));
  EXPECT_EQ(Plan->tableBytes(), FullBytes - (Plan->numRows() - kPhases) * 4);
  EXPECT_LT(Plan->tableBytes(), sizeof(g_cfg) / 8);
}

TEST_F(SmallTableTest, PlannerRefusesOutOfRegionAndUnknownShape) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  const auto *Base = reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]);

  std::string Error;
  // A region that cannot hold the last row's last field must be refused, not
  // truncated.
  auto Short = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg) - 64}, allRows(), {}, Error);
  EXPECT_FALSE(Short.has_value());
  EXPECT_NE(Error.find("source region"), std::string::npos) << Error;

  // A row index outside the declared extent must be refused.
  Error.clear();
  SmallVector<EJitSmallTableRowKey, 2> BadRows = {{{kCells, 0, 0}}};
  auto Bad = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(), EJitSmallTableSource{Base,
                                                                 sizeof(g_cfg)},
      BadRows, {}, Error);
  EXPECT_FALSE(Bad.has_value());
  EXPECT_NE(Error.find("extent"), std::string::npos) << Error;

  // An unknown source global names no shape at all.
  Error.clear();
  auto Unknown = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_missing", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg)}, allRows(), {}, Error);
  EXPECT_FALSE(Unknown.has_value());
  EXPECT_NE(Error.find("not found"), std::string::npos) << Error;

  // A duplicated authorized row is a configuration error, not a silent merge.
  Error.clear();
  SmallVector<EJitSmallTableRowKey, 2> Dup = {{{0, 0, 0}}, {{0, 0, 0}}};
  auto DupPlan = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg)}, Dup, {}, Error);
  EXPECT_FALSE(DupPlan.has_value());
  EXPECT_NE(Error.find("twice"), std::string::npos) << Error;
}

TEST_F(SmallTableTest, UniformContractIsExplicitAndFolds) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  const auto *Base = reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]);

  // mode is 1 in most rows but not all of them, so a contract claiming a
  // single value over the visible domain must be refused: the rows we can
  // already read contradict it. This is the "do not freeze an open domain"
  // gate — equality observed across visible rows is never a contract.
  std::string Error;
  std::vector<std::optional<uint64_t>> Bad = {std::nullopt, uint64_t{1},
                                              std::nullopt};
  auto Refused = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg)}, allRows(), Bad, Error);
  EXPECT_FALSE(Refused.has_value());
  EXPECT_NE(Error.find("contract"), std::string::npos) << Error;

  // gain is unique per row, so it can never be a uniform contract.
  Error.clear();
  std::vector<std::optional<uint64_t>> BadGain = {uint64_t{7}, std::nullopt,
                                                  std::nullopt};
  auto RefusedGain = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg)}, allRows(), BadGain, Error);
  EXPECT_FALSE(RefusedGain.has_value());

  // An explicit contract that holds for every authorized row is accepted at
  // planning time even when the domain is still partial, and the folded field
  // loses its table payload. Execution, however, is refused until the plan
  // covers the whole domain: the contract alone cannot make unready rows safe.
  Error.clear();
  SmallVector<EJitSmallTableRowKey, 1> OneRow = {{{0, 0, 3}}};
  uint64_t Mode = static_cast<uint64_t>(g_cfg[0][0][3].mode);
  std::vector<std::optional<uint64_t>> Good = {std::nullopt, Mode,
                                               std::nullopt};
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg)}, OneRow, Good, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  EXPECT_TRUE(Plan->fields[1].uniformValue.has_value());
  EXPECT_TRUE(Plan->fields[1].columnName.empty());
  EXPECT_EQ(Plan->uniformFieldCount(), 1u);
  EXPECT_FALSE(Plan->allRowsReady());
  EXPECT_EQ(Plan->readyRowCount(), 1u);
  // The ready mask still records exactly which row was observed.
  EXPECT_TRUE(Plan->rows[(0 * kTrps + 0) * kPhases + 3].ready);
  EXPECT_FALSE(Plan->rows[0].ready);
  EXPECT_EQ(Plan->rows.size(), kCells * kTrps * kPhases);

  // Executable lowering of the partial plan is refused (conservative
  // compiler-stage refusal until ST-B2); the fully ready uniform-folding test
  // below covers the folded IR itself.
  Error.clear();
  EXPECT_FALSE(EJitSmallTablePass::materialize(*M, *Plan, &Error));
  EXPECT_NE(Error.find("rows ready"), std::string::npos) << Error;
}

//===----------------------------------------------------------------------===//
// Pass: replacement shape and refusals
//===----------------------------------------------------------------------===//

TEST_F(SmallTableTest, PassReplacesWithDynamicIndexAndKeepsArguments) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  auto Set = makePlanSet(*M);
  const EJitSmallTablePlan *Plan = Set->find("stab_entry");
  ASSERT_NE(Plan, nullptr);

  std::string Error;
  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("stab_entry"), FAM);

  Function &F = *M->getFunction("stab_entry");
  EXPECT_EQ(countLoadsRootedAt(F, "g_cfg"), 1u) << "only the live load remains";
  EXPECT_EQ(countTableLoads(F, EJitSmallTablePlan::TableGlobalPrefix), 3u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 3u);
  EXPECT_EQ(Pass.getStats().uniformFolded, 0u);

  // Every synthesized table read carries the §9 provenance tag and none of
  // them inherits may_const authorization, so a later round cannot re-extract
  // it as an original source load.
  EXPECT_EQ(countTaggedTableLoads(F), 3u);
  for (const Instruction &I : instructions(F))
    if (const auto *LI = dyn_cast<LoadInst>(&I))
      if (LI->hasMetadata("ejit.smalltable.load"))
        EXPECT_FALSE(LI->hasMetadata(MD_EJIT_MAY_CONST));

  // Every real parameter is still live: the row index and the surrounding
  // dynamic work must not have been replaced by a compiled member's values.
  EXPECT_GT(F.getArg(0)->getNumUses(), 0u);
  EXPECT_GT(F.getArg(1)->getNumUses(), 0u);
  EXPECT_GT(F.getArg(2)->getNumUses(), 0u);
  EXPECT_GT(F.getArg(3)->getNumUses(), 0u);

  // The emitted column is mutable, directly addressable data: the runtime may
  // publish a later row into it, so it must never be an LLVM constant.
  GlobalVariable *Column = M->getNamedGlobal("__ejit_stab_stab_entry_c0");
  ASSERT_NE(Column, nullptr);
  EXPECT_FALSE(Column->isConstant());
  // Direct (dso_local) binding is what makes the AArch64 product object
  // adrp+add without a GOT; the x86-64 host keeps the symbol preemptible so the
  // JIT can use a PC-relative access (see wantDSOLocal).
  EXPECT_EQ(Column->isDSOLocal(),
            M->getTargetTriple().isAArch64());
  EXPECT_EQ(Column->getValueType(),
            ArrayType::get(Type::getInt32Ty(Ctx), kCells * kTrps * kPhases));
  EXPECT_TRUE(Column->hasMetadata("ejit.smalltable.column"));

  // Running the pass again in a later round must not duplicate or re-replace.
  EJitSmallTablePass Again(*Plan);
  Again.run(F, FAM);
  EXPECT_EQ(Again.getStats().mayConstSites, 0u);
  EXPECT_EQ(countTableLoads(F, EJitSmallTablePlan::TableGlobalPrefix), 3u);
}

TEST_F(SmallTableTest, PassRefusesVolatileAtomicAndUnsupportedShapes) {
  auto PlanM = parseModule();
  ASSERT_TRUE(PlanM);
  auto Set = makePlanSet(*PlanM);
  const EJitSmallTablePlan *Plan = Set->find("stab_entry");
  ASSERT_NE(Plan, nullptr);

  auto M = parseBadShapes();
  ASSERT_TRUE(M);
  std::string Error;
  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;

  Function *F = M->getFunction("stab_entry");
  ASSERT_NE(F, nullptr);
  const unsigned LoadsBefore = countAllLoads(*F);
  const unsigned RootedBefore = countLoadsRootedAt(*F, "g_cfg");

  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*F, FAM);

  // Exactly one load is a legal table read; the volatile, atomic, non-inbounds,
  // pointer-typed, address-space-1 and inttoptr-rooted loads keep their
  // original form.
  EXPECT_EQ(Pass.getStats().tableReplaced, 1u);
  EXPECT_EQ(Pass.getStats().uniformFolded, 0u);
  EXPECT_EQ(countTableLoads(*F, EJitSmallTablePlan::TableGlobalPrefix), 1u);
  EXPECT_EQ(countAllLoads(*F), LoadsBefore);
  EXPECT_EQ(countLoadsRootedAt(*F, "g_cfg"), RootedBefore - 1);
}

TEST_F(SmallTableTest, PassCoversScalarWidthsAndFloatBits) {
  std::string Text = moduleTargetHeader() + R"(
    %W = type { i1, i8, i16, i32, i64, float, double, i32 }
    @g_w = external global [2 x [10 x %W]]
    define i8 @w_entry(i32 %cell, i32 %slotNo) !ejit.metadata !0 {
    entry:
      %phase = urem i32 %slotNo, 10
      %row = getelementptr inbounds [2 x [10 x %W]], ptr @g_w, i64 0, i32 %cell, i32 %phase
      %b = getelementptr inbounds %W, ptr %row, i32 0, i32 0
      %v0 = load i1, ptr %b, align 1, !ejit.may_const !1
      %c = getelementptr inbounds %W, ptr %row, i32 0, i32 1
      %v1 = load i8, ptr %c, align 1, !ejit.may_const !1
      %s = getelementptr inbounds %W, ptr %row, i32 0, i32 2
      %v2 = load i16, ptr %s, align 2, !ejit.may_const !1
      %i = getelementptr inbounds %W, ptr %row, i32 0, i32 3
      %v3 = load i32, ptr %i, align 4, !ejit.may_const !1
      %l = getelementptr inbounds %W, ptr %row, i32 0, i32 4
      %v4 = load i64, ptr %l, align 8, !ejit.may_const !1
      %f = getelementptr inbounds %W, ptr %row, i32 0, i32 5
      %v5 = load float, ptr %f, align 4, !ejit.may_const !1
      %d = getelementptr inbounds %W, ptr %row, i32 0, i32 6
      %v6 = load double, ptr %d, align 8, !ejit.may_const !1
      %p = getelementptr inbounds %W, ptr %row, i32 0, i32 7
      %v7 = load i32, ptr %p, align 4
      %t0 = zext i1 %v0 to i8
      %t1 = add i8 %t0, %v1
      %t2 = trunc i16 %v2 to i8
      %t3 = add i8 %t1, %t2
      %t4 = trunc i32 %v3 to i8
      %t5 = add i8 %t3, %t4
      %t6 = trunc i64 %v4 to i8
      %t7 = add i8 %t5, %t6
      %fb = bitcast float %v5 to i32
      %t8 = trunc i32 %fb to i8
      %t9 = add i8 %t7, %t8
      %db = bitcast double %v6 to i64
      %t10 = trunc i64 %db to i8
      %t11 = add i8 %t9, %t10
      %t12 = trunc i32 %v7 to i8
      %t13 = add i8 %t11, %t12
      ret i8 %t13
    }
    !0 = !{!2}
    !1 = !{}
    !2 = !{!"ejit_entry"}
  )";
  SMDiagnostic Err;
  auto M = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(M) << "scalar-width module failed to parse";

  struct WElement {
    uint8_t b;
    uint8_t c;
    uint16_t s;
    uint32_t i;
    uint64_t l;
    float f;
    double d;
    uint32_t tail;
  };
  static_assert(sizeof(WElement) == 40, "layout must match the IR type");
  static WElement W[2][10];
  std::memset(W, 0, sizeof(W));
  for (unsigned C = 0; C < 2; ++C)
    for (unsigned P = 0; P < 10; ++P) {
      W[C][P].b = (P & 1) != 0;
      W[C][P].c = static_cast<uint8_t>(0x80u + P);
      W[C][P].s = static_cast<uint16_t>(0x8000u + P);
      W[C][P].i = 0x80000000u + P;
      W[C][P].l = 0x8000000000000000ull + P;
      W[C][P].f = floatFromBits(C == 0 && P == 0
                                   ? 0x80000000u
                                   : 0x7fc00000u + (C * 10 + P));
      uint64_t DBits = 0x7ff8000000000001ull + P;
      std::memcpy(&W[C][P].d, &DBits, sizeof(DBits));
    }

  SmallVector<EJitSmallTableDim, 2> Dims;
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, 2});
  Dims.push_back({EJitSmallTableDim::Kind::ModuloArgument, 1, 10, 10});
  SmallVector<EJitSmallTableRowKey, 20> Rows;
  for (unsigned C = 0; C < 2; ++C)
    for (unsigned P = 0; P < 10; ++P)
      Rows.push_back({{C, P}});

  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "w_entry", "g_w", Dims,
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&W[0][0]),
                           sizeof(W)},
      Rows, {}, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  ASSERT_EQ(Plan->fields.size(), 7u);
  EXPECT_EQ(Plan->fields[0].kind, EJitSmallTableKind::Integer);
  EXPECT_EQ(Plan->fields[0].bitWidth, 1u);
  EXPECT_EQ(Plan->fields[4].bitWidth, 64u);
  EXPECT_EQ(Plan->fields[5].kind, EJitSmallTableKind::Float);
  EXPECT_EQ(Plan->fields[6].kind, EJitSmallTableKind::Double);

  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("w_entry"), FAM);
  EXPECT_EQ(Pass.getStats().tableReplaced, 7u);

  // The emitted float/double initializers keep the exact bit patterns,
  // including -0.0 and the NaN payload.
  GlobalVariable *FCol = M->getNamedGlobal("__ejit_stab_w_entry_c5");
  GlobalVariable *DCol = M->getNamedGlobal("__ejit_stab_w_entry_c6");
  ASSERT_NE(FCol, nullptr);
  ASSERT_NE(DCol, nullptr);
  EXPECT_EQ(elementBits(FCol->getInitializer(), 0), 0x80000000u);
  EXPECT_EQ(elementBits(FCol->getInitializer(), 1), 0x7fc00001u);
  EXPECT_EQ(elementBits(FCol->getInitializer(), 11), 0x7fc0000bu);
  EXPECT_EQ(elementBits(DCol->getInitializer(), 0), 0x7ff8000000000001ull);
  // The i1 column keeps a one-byte element with the exact boolean value.
  GlobalVariable *BCol = M->getNamedGlobal("__ejit_stab_w_entry_c0");
  ASSERT_NE(BCol, nullptr);
  EXPECT_EQ(elementBits(BCol->getInitializer(), 0), 0u);
  EXPECT_EQ(elementBits(BCol->getInitializer(), 1), 1u);
}

//===----------------------------------------------------------------------===//
// Optimizer pipeline integration
//===----------------------------------------------------------------------===//

TEST_F(SmallTableTest, PipelineKeepsTableLoadsThroughAllReplaceRounds) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  auto Set = makePlanSet(*M);

  PeriodArrayRegistry &Registry = makeRegistry();
  EJitOptimizer Opt(Registry);
  Opt.setSmallTablePlans(Set);
  SpecializationContext C = baselineCtx();
  Opt.runPipeline(*M, C);

  Function &F = *M->getFunction("stab_entry");
  // The may_const loads are gone, replaced by table loads; the ordinary live
  // load is untouched.
  EXPECT_EQ(countTableLoads(F, EJitSmallTablePlan::TableGlobalPrefix), 3u);
  EXPECT_EQ(countLoadsRootedAt(F, "g_cfg"), 1u);
  GlobalVariable *Column = M->getNamedGlobal("__ejit_stab_stab_entry_c0");
  ASSERT_NE(Column, nullptr);
  EXPECT_FALSE(Column->isConstant());
}

TEST_F(SmallTableTest, PipelineFeatureOffKeepsBaseline) {
  auto M = parseModule();
  ASSERT_TRUE(M);

  PeriodArrayRegistry &Registry = makeRegistry();
  EJitOptimizer Opt(Registry);
  // No plan installed: the small-table pass must never run.
  SpecializationContext C = baselineCtx();
  Opt.runPipeline(*M, C);

  Function &F = *M->getFunction("stab_entry");
  EXPECT_EQ(countTableLoads(F, EJitSmallTablePlan::TableGlobalPrefix), 0u);
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_stab_entry_c0"), nullptr);
  // Dynamic addresses keep the may_const loads in place, so the baseline can
  // only have left them alone.
  EXPECT_EQ(countLoadsRootedAt(F, "g_cfg"), 4u);
}

TEST_F(SmallTableTest, PlanKeepsDeclaredDimensionDynamic) {
  PeriodArrayRegistry &Registry = makeRegistry();

  // Without a plan the ejit_period_arr_ind parameter is substituted with the
  // compiled member's value, exactly as before.
  auto Base = parseModule();
  ASSERT_TRUE(Base);
  {
    EJitOptimizer Opt(Registry);
    SpecializationContext C = baselineCtx();
    C.dimensions.push_back({"cell", 3});
    Opt.runPipeline(*Base, C);
    EXPECT_EQ(Base->getFunction("stab_entry")->getArg(0)->getNumUses(), 0u);
  }

  // With a plan that declares cell as a dynamic table dimension, the parameter
  // stays live and the table index keeps using it.
  auto M = parseModule();
  ASSERT_TRUE(M);
  auto Set = makePlanSet(*M);
  EJitOptimizer Opt(Registry);
  Opt.setSmallTablePlans(Set);
  SpecializationContext C = baselineCtx();
  C.dimensions.push_back({"cell", 3});
  Opt.runPipeline(*M, C);
  Function &F = *M->getFunction("stab_entry");
  EXPECT_GT(F.getArg(0)->getNumUses(), 0u);
  EXPECT_EQ(countTableLoads(F, EJitSmallTablePlan::TableGlobalPrefix), 3u);
}

//===----------------------------------------------------------------------===//
// Real JIT execution
//===----------------------------------------------------------------------===//

TEST_F(SmallTableTest, JitDynamicRowsMatchAotAcrossTwoSlotWraps) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  auto Set = makePlanSet(*M);
  PeriodArrayRegistry &Registry = makeRegistry();

  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab01);
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab01, "stab_entry");
  ASSERT_NE(Fn, nullptr);

  // Two full wraps of the real 0..1023 slot number, every cell and trp.
  for (unsigned Wrap = 0; Wrap < 2; ++Wrap)
    for (unsigned C = 0; C < kCells; ++C)
      for (unsigned T = 0; T < kTrps; ++T)
        for (unsigned S = 0; S < 1024; ++S) {
          const int32_t X = static_cast<int32_t>(S % 37);
          EXPECT_EQ(Fn(C, T, S, X), aotResult(C, T, S, X))
              << "cell=" << C << " trp=" << T << " slot=" << S;
        }
}

TEST_F(SmallTableTest, JitLateDifferentRowValuesAreObserved) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  auto Set = makePlanSet(*M);
  PeriodArrayRegistry &Registry = makeRegistry();

  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab02);
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab02, "stab_entry");
  ASSERT_NE(Fn, nullptr);
  // The engine claims the transform-created column globals in the
  // materialization responsibility, exactly like the PGO counters, so the
  // runtime can resolve the table's stable address. Published row data is then
  // plain mutable memory: no per-load table pointer and no recompilation.
  auto ColOrErr = Engine->lookup(0x57ab02, "__ejit_stab_stab_entry_c0");
  ASSERT_TRUE(static_cast<bool>(ColOrErr)) << toString(ColOrErr.takeError());
  auto *Column = reinterpret_cast<int32_t *>(*ColOrErr);
  ASSERT_NE(Column, nullptr);

  const unsigned C = 3, T = 1, P = 7, S = 7;
  const int32_t X = 5;
  EXPECT_EQ(Fn(C, T, S, X), aotResult(C, T, S, X));

  // Publish a different value into the already emitted table for exactly one
  // row. Pure-table mode must observe it without any recompilation, which is
  // only possible because the column is mutable data and the load is real.
  const uint64_t Row = (C * kTrps + T) * kPhases + P;
  const int32_t NewGain = 4242;
  Column[Row] = NewGain;

  const int32_t H =
      static_cast<int32_t>(C * 1000 + T * 100 + (S % 100)) + X * 17;
  const int32_t FBits = static_cast<int32_t>(bitsFromFloat(g_cfg[C][T][P].scale));
  const int32_t Expected = X * NewGain + g_cfg[C][T][P].live + H + FBits;
  EXPECT_EQ(Fn(C, T, S, X), Expected);
  // Neighbouring rows are unchanged.
  EXPECT_EQ(Fn(C, T, S + 1, X), aotResult(C, T, S + 1, X));
}

TEST_F(SmallTableTest, JitLiveLoadStoreAndHelperStayDynamic) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  auto Set = makePlanSet(*M);
  PeriodArrayRegistry &Registry = makeRegistry();

  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab03);
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab03, "stab_entry");
  ASSERT_NE(Fn, nullptr);

  const unsigned C = 2, T = 0, S = 21;
  const unsigned P = S % kPhases;
  const int32_t X = 3;

  EXPECT_EQ(Fn(C, T, S, X), aotResult(C, T, S, X));
  // The store target was the real output address for the real member.
  EXPECT_EQ(g_out[C][T], static_cast<int32_t>(S));
  EXPECT_EQ(g_out[C + 1][T], 0);

  // The ordinary live field is still read from the source object at call time.
  g_cfg[C][T][P].live += 99;
  EXPECT_EQ(Fn(C, T, S, X), aotResult(C, T, S, X));
  EXPECT_EQ(g_out[C][T], static_cast<int32_t>(S));

  // The helper consumes the real cell/trp/slotNo arguments: changing only the
  // slot number changes the result through the helper term as well.
  EXPECT_NE(Fn(C, T, S + 1, X), Fn(C, T, S + 11, X));
}

TEST_F(SmallTableTest, JitFeatureOffKeepsBaselineCorrect) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  PeriodArrayRegistry &Registry = makeRegistry();

  // No plan installed: the feature is OFF and the baseline pipeline runs on
  // exactly the same module.
  auto Engine = compileWithEngine(*M, nullptr, Registry, 0x57ab04);
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab04, "stab_entry");
  ASSERT_NE(Fn, nullptr);

  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T)
      for (unsigned S = 0; S < 100; ++S) {
        const int32_t X = static_cast<int32_t>(S % 13);
        EXPECT_EQ(Fn(C, T, S, X), aotResult(C, T, S, X));
      }
}

//===----------------------------------------------------------------------===//
// Readiness holes, uniform-contract honesty and refusal boundaries
//===----------------------------------------------------------------------===//

/// A partial plan must keep its ready mask, and with no runtime per-row
/// admission gate (ST-B2) both lowering entry points must refuse it: spec §5
/// forbids emitting a missing row as 0/undef or as a representative cell, and
/// §6.5 requires `tableReady(row)` before dispatch. This test pins the
/// conservative compiler-stage refusal so a runtime integration cannot silently
/// assume the whole column is initialized.
TEST_F(SmallTableTest, PartiallyReadyPlanIsRefusedUntilRuntimeAdmission) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableRowKey, 16> ReadyRows;
  for (unsigned P = 0; P < kPhases; ++P)
    ReadyRows.push_back({{0, 0, P}});
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]),
                           sizeof(g_cfg)},
      ReadyRows, {}, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  EXPECT_FALSE(Plan->allRowsReady());
  EXPECT_EQ(Plan->readyRowCount(), kPhases);
  EXPECT_EQ(Plan->numRows(), kCells * kTrps * kPhases);

  // materialize() refuses and leaves the module on its original loads: no
  // column global, no placeholder table.
  Error.clear();
  EXPECT_FALSE(EJitSmallTablePass::materialize(*M, *Plan, &Error));
  EXPECT_NE(Error.find("rows ready"), std::string::npos) << Error;
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_stab_entry_c0"), nullptr);
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("stab_entry"), "g_cfg"), 4u);

  // A direct pass call enforces the same rule, so even a caller that skips
  // materialize() cannot fold a contract from an incomplete domain.
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("stab_entry"), FAM);
  EXPECT_EQ(Pass.getStats().tableReplaced, 0u);
  EXPECT_EQ(Pass.getStats().uniformFolded, 0u);
  EXPECT_EQ(Pass.getStats().refusedNotReady, 1u);
  EXPECT_EQ(countTableLoads(*M->getFunction("stab_entry"),
                            EJitSmallTablePlan::TableGlobalPrefix),
            0u);
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("stab_entry"), "g_cfg"), 4u);
}

/// With no runtime admission path (ST-B2), a partial plan must not change the
/// compiled entry at all: every member, ready or not, keeps the AOT behavior.
/// This is the executable counterpart of the refusal test above.
TEST_F(SmallTableTest, JitPartiallyReadyPlanKeepsAotBehavior) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableRowKey, 16> ReadyRows;
  for (unsigned P = 0; P < kPhases; ++P)
    ReadyRows.push_back({{0, 0, P}});
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]),
                           sizeof(g_cfg)},
      ReadyRows, {}, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  ASSERT_FALSE(Plan->allRowsReady());
  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(std::move(*Plan)));
  PeriodArrayRegistry &Registry = makeRegistry();

  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab05);
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab05, "stab_entry");
  ASSERT_NE(Fn, nullptr);

  // The refused plan produced no table columns for the engine to claim.
  EXPECT_TRUE(Engine->getLastSmallTableColumnNames().empty());
  auto ColOrErr = Engine->lookup(0x57ab05, "__ejit_stab_stab_entry_c0");
  EXPECT_FALSE(static_cast<bool>(ColOrErr))
      << "a partial plan must not publish a table";
  if (!ColOrErr)
    consumeError(ColOrErr.takeError());

  const int32_t X = 9;
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T)
      for (unsigned S = 0; S < 100; ++S) {
        const int32_t XX = X + static_cast<int32_t>(S % 7);
        EXPECT_EQ(Fn(C, T, S, XX), aotResult(C, T, S, XX))
            << "cell=" << C << " trp=" << T << " slot=" << S;
      }
}

/// The default mode decides on the proven domain, and the proven domain is the
/// whole safety argument: equality over three ready rows is a real automatic
/// constant *for that domain*, but a domain that does not cover the declared
/// schema can never be lowered executably (spec §4.1 step 1, §5, §6.6). On the
/// complete domain the same field is genuinely not equal and stays a real
/// column, so the automatic path never freezes "the rows we happened to see"
/// for a domain it cannot cover.
TEST_F(SmallTableTest, AutomaticEqualityIsProvenOnTheDomainNotOnVisibleRows) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  // Rows (0,0,1..3): all three visible `mode` values are 1 in fillConfig(), so
  // the solver does fold mode for exactly this proven domain.
  SmallVector<EJitSmallTableRowKey, 4> Rows = {{{0, 0, 1}}, {{0, 0, 2}},
                                               {{0, 0, 3}}};
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]),
                           sizeof(g_cfg)},
      Rows, {}, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  EXPECT_EQ(Plan->mode, EJitSmallTablePlanMode::Automatic);
  EXPECT_EQ(Plan->uniformFieldCount(), 1u);
  ASSERT_EQ(Plan->fields[1].strategy, EJitSmallTableStrategy::Uniform);
  ASSERT_TRUE(Plan->fields[1].uniformValue.has_value());
  EXPECT_EQ(*Plan->fields[1].uniformValue, 1u);
  EXPECT_TRUE(Plan->fields[1].columnName.empty());
  EXPECT_FALSE(Plan->fields[1].uniformFromContract);
  // gain and scale differ per phase on this domain, so they keep the axis that
  // explains the difference rather than a full-dimensional table.
  EXPECT_EQ(Plan->fields[0].retainedAxes.size(), 1u);
  EXPECT_EQ(Plan->fields[0].retainedAxes[0], 2u);
  EXPECT_EQ(Plan->fields[2].retainedAxes.size(), 1u);

  // A plan that does not cover its declared schema is planned but never lowered,
  // so the folded constant cannot reach a business call: no column global, and
  // every load keeps its original form.
  Error.clear();
  EXPECT_FALSE(EJitSmallTablePass::materialize(*M, *Plan, &Error));
  EXPECT_NE(Error.find("rows ready"), std::string::npos) << Error;
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_stab_entry_c0"), nullptr);
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("stab_entry"), FAM);
  EXPECT_EQ(Pass.getStats().uniformFolded, 0u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 0u);
  EXPECT_EQ(Pass.getStats().refusedNotReady, 1u);
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("stab_entry"), "g_cfg"), 4u);

  // On the complete domain mode is not equal at all (phase 0 and 4 take the
  // other branch), so the automatic solver keeps a real, phase-indexed column.
  auto FullM = parseModule();
  ASSERT_TRUE(FullM);
  auto FullSet = makePlanSet(*FullM);
  const EJitSmallTablePlan *Full = FullSet->find("stab_entry");
  ASSERT_NE(Full, nullptr);
  EXPECT_EQ(Full->uniformFieldCount(), 0u);
  EXPECT_EQ(Full->fields[1].strategy, EJitSmallTableStrategy::Table);
  ASSERT_EQ(Full->fields[1].retainedAxes.size(), 1u);
  EXPECT_EQ(Full->fields[1].retainedAxes[0], 2u);
  Error.clear();
  ASSERT_TRUE(EJitSmallTablePass::materialize(*FullM, *Full, &Error)) << Error;
  EJitSmallTablePass FullPass(*Full);
  FunctionAnalysisManager FullFAM;
  FullPass.run(*FullM->getFunction("stab_entry"), FullFAM);
  EXPECT_EQ(FullPass.getStats().uniformFolded, 0u);
  EXPECT_EQ(FullPass.getStats().tableReplaced, 3u);
}

/// A contract is an explicit caller assertion. It is checked against every row
/// the planner can already read, and a contract with no observed row is
/// recorded but stays unvalidated; only the runtime admission path may rely on
/// it.
TEST_F(SmallTableTest, UniformContractIsCheckedAgainstEveryReadyRow) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  const auto *Base = reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]);
  std::string Error;

  // Contract count must match the discovered field count.
  std::vector<std::optional<uint64_t>> ShortContract = {std::nullopt};
  auto BadCount = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg)}, allRows(), ShortContract, Error);
  EXPECT_FALSE(BadCount.has_value());
  EXPECT_NE(Error.find("contract count"), std::string::npos) << Error;

  // A contract that holds for every ready row is accepted and recorded. With
  // zero ready rows there is no observation at all, so the contract is refused
  // rather than frozen into the schema (do not fold from an empty set).
  Error.clear();
  std::vector<std::optional<uint64_t>> Mode1 = {std::nullopt, uint64_t{1},
                                                std::nullopt};
  SmallVector<EJitSmallTableRowKey, 2> NoRows;
  auto Blind = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg)}, NoRows, Mode1, Error);
  EXPECT_FALSE(Blind.has_value());
  EXPECT_NE(Error.find("no confirmed ready row"), std::string::npos) << Error;

  // A contract contradicted by a ready row is refused.
  Error.clear();
  std::vector<std::optional<uint64_t>> Mode2 = {std::nullopt, uint64_t{2},
                                                std::nullopt};
  SmallVector<EJitSmallTableRowKey, 1> OneRow = {{{0, 0, 3}}};
  auto Contradicted = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{Base, sizeof(g_cfg)}, OneRow, Mode2, Error);
  EXPECT_FALSE(Contradicted.has_value());
  EXPECT_NE(Error.find("contract"), std::string::npos) << Error;
}

/// The compiler half of the §6.6 admission contract, on a plan that covers its
/// whole declared domain: the contracted field folds to the exact recorded bit
/// pattern, the varying fields stay real table loads, and the contract is
/// recorded on the entry. The runtime cold-path validation of a *later* member
/// is ST-B2 and is not claimed here.
TEST_F(SmallTableTest, UniformContractFoldsOnFullyReadyPlan) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(uniformModuleText(), Err, Ctx);
  ASSERT_TRUE(M) << "uniform module failed to parse";

  SmallVector<EJitSmallTableDim, 2> Dims;
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, kUniformCells});
  Dims.push_back(
      {EJitSmallTableDim::Kind::ModuloArgument, 1, kPhases, kPhases});
  SmallVector<EJitSmallTableRowKey, 32> Rows;
  for (unsigned C = 0; C < kUniformCells; ++C)
    for (unsigned P = 0; P < kPhases; ++P)
      Rows.push_back({{C, P}});
  // Explicit caller contract: mode is 7 for every member of this domain. The
  // planner confirms it against every ready row, it is never inferred.
  std::vector<std::optional<uint64_t>> Contract = {uint64_t{7}, std::nullopt,
                                                   std::nullopt};
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "u_entry", "g_u", Dims,
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_u[0][0]),
                           sizeof(g_u)},
      Rows, Contract, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  EXPECT_TRUE(Plan->allRowsReady());
  EXPECT_EQ(Plan->readyRowCount(), Plan->numRows());
  EXPECT_EQ(Plan->uniformFieldCount(), 1u);
  EXPECT_TRUE(Plan->fields[0].uniformValue.has_value());
  EXPECT_TRUE(Plan->fields[0].columnName.empty());
  EXPECT_EQ(Plan->fields[1].columnName, "__ejit_stab_u_entry_c1");
  EXPECT_EQ(Plan->fields[2].columnName, "__ejit_stab_u_entry_c2");

  // Compiler half on a scratch module: the uniform field has no table payload,
  // the varying fields do, and the entry carries the contract metadata.
  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_u_entry_c0"), nullptr);
  ASSERT_NE(M->getNamedGlobal("__ejit_stab_u_entry_c1"), nullptr);
  ASSERT_NE(M->getNamedGlobal("__ejit_stab_u_entry_c2"), nullptr);
  ASSERT_TRUE(
      M->getFunction("u_entry")->hasMetadata("ejit.smalltable.contract"));
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("u_entry"), FAM);
  EXPECT_EQ(Pass.getStats().uniformFolded, 1u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 2u);
  // The contracted load is gone (folded), so no source load remains at all.
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("u_entry"), "g_u"), 0u);
  EXPECT_EQ(countTableLoads(*M->getFunction("u_entry"),
                            EJitSmallTablePlan::TableGlobalPrefix),
            2u);

  // JIT half on a fresh module: the folded contract and the real table loads
  // agree with AOT for every row across two full slot wraps, and the live
  // store still targets the real output address.
  auto JitM = parseAssemblyString(uniformModuleText(), Err, Ctx);
  ASSERT_TRUE(JitM) << "uniform module failed to re-parse";
  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(*Plan));
  PeriodArrayRegistry &Registry = makeRegistry();
  auto Engine = compileWithEngine(*JitM, Set, Registry, 0x57ab07, "u_entry");
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab07, "u_entry");
  ASSERT_NE(Fn, nullptr);

  // The engine claimed exactly the two transform-created varying columns; the
  // folded field has no table to publish into.
  ArrayRef<std::string> Names = Engine->getLastSmallTableColumnNames();
  ASSERT_EQ(Names.size(), 2u);
  EXPECT_EQ(Names[0], "__ejit_stab_u_entry_c1");
  EXPECT_EQ(Names[1], "__ejit_stab_u_entry_c2");

  for (unsigned Wrap = 0; Wrap < 2; ++Wrap)
    for (unsigned C = 0; C < kUniformCells; ++C)
      for (unsigned S = 0; S < 1024; ++S) {
        const int32_t X = static_cast<int32_t>(S % 29);
        EXPECT_EQ(Fn(C, S, X), aotUniform(C, S % kPhases, X))
            << "cell=" << C << " slot=" << S;
      }
  EXPECT_EQ(gu_out[0], 1023);
  EXPECT_EQ(gu_out[1], 1023);

  // Pure-table publication still works for the varying field: a new value
  // written into the published column is observed with no recompilation.
  auto ColOrErr = Engine->lookup(0x57ab07, "__ejit_stab_u_entry_c1");
  ASSERT_TRUE(static_cast<bool>(ColOrErr)) << toString(ColOrErr.takeError());
  auto *GainCol = reinterpret_cast<int32_t *>(*ColOrErr);
  const unsigned C = 1, S = 7;
  const unsigned P = S % kPhases;
  const int32_t X = 5;
  EXPECT_EQ(Fn(C, S, X), aotUniform(C, P, X));
  GainCol[C * kPhases + P] = 9999;
  EXPECT_EQ(Fn(C, S, X),
            X * 9999 + static_cast<int32_t>(bitsFromFloat(g_u[C][P].scale)));
}

/// A domain that cannot be materialized is refused before anything is
/// allocated, and a schema with two axes on one argument is not a schema.
TEST_F(SmallTableTest, PlannerRefusesOversizedAndDuplicateDomains) {
  std::string Text = moduleTargetHeader() + R"(
    @g_huge = external global [2000000 x i32]
    define i32 @h_entry(i32 %i) !ejit.metadata !0 {
    entry:
      %p = getelementptr inbounds [2000000 x i32], ptr @g_huge, i64 0, i32 %i
      %v = load i32, ptr %p, align 4, !ejit.may_const !1
      ret i32 %v
    }
    !0 = !{!2}
    !1 = !{}
    !2 = !{!"ejit_entry"}
  )";
  SMDiagnostic Err;
  auto M = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 1> Huge = {
      {EJitSmallTableDim::Kind::Argument, 0, 0, 2000000}};
  std::string Error;
  auto Big = EJitSmallTablePlanner::planShape(*M, "h_entry", "g_huge", Huge,
                                              Error);
  EXPECT_FALSE(Big.has_value());
  EXPECT_NE(Error.find("row bound"), std::string::npos) << Error;

  // Two dimensions on the same argument describe no reachable row key.
  auto Real = parseModule();
  ASSERT_TRUE(Real);
  SmallVector<EJitSmallTableDim, 3> Dup = {
      {EJitSmallTableDim::Kind::Argument, 0, 0, kCells},
      {EJitSmallTableDim::Kind::Argument, 1, 0, kTrps},
      {EJitSmallTableDim::Kind::Argument, 0, 0, kPhases}};
  Error.clear();
  auto Plan = EJitSmallTablePlanner::planShape(*Real, "stab_entry", "g_cfg", Dup,
                                               Error);
  EXPECT_FALSE(Plan.has_value());
  EXPECT_NE(Error.find("same argument"), std::string::npos) << Error;

  // A declared extent that disagrees with the source array level is refused
  // rather than clamped to the level (audit §2 "declared extent" item).
  Error.clear();
  SmallVector<EJitSmallTableDim, 1> WrongExtent = {
      {EJitSmallTableDim::Kind::Argument, 0, 0, 3}};
  auto WrongLevel = EJitSmallTablePlanner::planShape(*Real, "stab_entry",
                                                     "g_cfg", WrongExtent,
                                                     Error);
  EXPECT_FALSE(WrongLevel.has_value());
  EXPECT_NE(Error.find("does not match"), std::string::npos) << Error;
}

/// materialize() promises to leave the module untouched on refusal, to fill a
/// pre-declared external slot itself, and to never reuse a foreign definition.
TEST_F(SmallTableTest, MaterializeFillsDeclaredSlotsAndRefusesForeignOnes) {
  auto PlanM = parseModule();
  ASSERT_TRUE(PlanM);
  // This test pins materialize()'s slot handling on the full-dimensional column
  // shape, so it explicitly selects the comparison mode instead of the
  // automatic default (which would project `mode` onto the phase axis).
  auto Set = makePlanSet(*PlanM, {}, {}, EJitSmallTablePlanMode::ExplicitContracts);
  const EJitSmallTablePlan *Plan = Set->find("stab_entry");
  ASSERT_NE(Plan, nullptr);
  const std::string Anchor = "@g_out = external global [6 x [2 x i32]]";

  // A pre-declared external slot is a table this pass defines: it must be
  // filled with the plan's values before any load can read it.
  std::string DeclText = moduleText();
  size_t Pos = DeclText.find(Anchor);
  ASSERT_NE(Pos, std::string::npos);
  DeclText.insert(
      Pos + Anchor.size(),
      "\n    @__ejit_stab_stab_entry_c1 = external global [120 x i32]");
  SMDiagnostic Err;
  auto DeclM = parseAssemblyString(DeclText, Err, Ctx);
  ASSERT_TRUE(DeclM) << "declaration module failed to parse";
  std::string Error;
  ASSERT_TRUE(EJitSmallTablePass::materialize(*DeclM, *Plan, &Error)) << Error;
  GlobalVariable *Filled = DeclM->getNamedGlobal("__ejit_stab_stab_entry_c1");
  ASSERT_NE(Filled, nullptr);
  EXPECT_TRUE(Filled->hasInitializer());
  EXPECT_EQ(Filled->isDSOLocal(),
            DeclM->getTargetTriple().isAArch64());
  EXPECT_FALSE(Filled->isConstant());
  EXPECT_EQ(elementBits(Filled->getInitializer(), 0),
            static_cast<uint64_t>(g_cfg[0][0][0].mode));
  EXPECT_NE(DeclM->getNamedGlobal("__ejit_stab_stab_entry_c0"), nullptr);

  // A constant definition of the same type is not a table this pass may
  // publish into, and refusing must not leave the first column behind.
  std::string ConstText = moduleText();
  Pos = ConstText.find(Anchor);
  ASSERT_NE(Pos, std::string::npos);
  ConstText.insert(Pos + Anchor.size(),
                   "\n    @__ejit_stab_stab_entry_c1 = constant [120 x i32] "
                   "zeroinitializer");
  auto ConstM = parseAssemblyString(ConstText, Err, Ctx);
  ASSERT_TRUE(ConstM) << "constant module failed to parse";
  Error.clear();
  EXPECT_FALSE(EJitSmallTablePass::materialize(*ConstM, *Plan, &Error));
  EXPECT_FALSE(Error.empty());
  EXPECT_EQ(ConstM->getNamedGlobal("__ejit_stab_stab_entry_c0"), nullptr)
      << "a refused materialize must not leave partial columns";

  // A hand-built plan that violates its own invariants (float kind with an
  // 8-bit width) is refused even though every field is public.
  auto Clean = parseModule();
  ASSERT_TRUE(Clean);
  EJitSmallTablePlan Bad = *Plan;
  Bad.fields[0].kind = EJitSmallTableKind::Float;
  Bad.fields[0].bitWidth = 8;
  Error.clear();
  EXPECT_FALSE(EJitSmallTablePass::materialize(*Clean, Bad, &Error));
  EXPECT_FALSE(Error.empty());
  EXPECT_EQ(Clean->getNamedGlobal("__ejit_stab_stab_entry_c0"), nullptr);
}

/// InstCombine rewrites `bitcast (load float)` into `load i32`. The pass must
/// still recognize the field and reinterpret bit-exactly.
/// A constant index that hops whole source elements addresses a different row
/// than the plan describes, so it must keep its original load instead of being
/// aliased onto a same-offset field of the planned row (audit N2).
TEST_F(SmallTableTest, PassRefusesWholeElementConstantHop) {
  auto PlanM = parseModule();
  ASSERT_TRUE(PlanM);
  auto Set = makePlanSet(*PlanM);
  const EJitSmallTablePlan *Plan = Set->find("stab_entry");
  ASSERT_NE(Plan, nullptr);

  std::string Text = moduleText();
  const std::string Live = "%live = load i32, ptr %livep, align 4";
  ASSERT_NE(Text.find(Live), std::string::npos);
  Text.replace(Text.find(Live), Live.size(),
               Live +
                   "\n      %hopp = getelementptr inbounds %Big, ptr %row, i32 1"
                   "\n      %hopped = load i32, ptr %hopp, align 4, "
                   "!ejit.may_const !1");
  const std::string Sum = "%sum = add i32 %mul, %live";
  ASSERT_NE(Text.find(Sum), std::string::npos);
  Text.replace(Text.find(Sum), Sum.size(),
               Sum + "\n      %sumh = add i32 %sum, %hopped");
  const std::string Sum2 = "%sum2 = add i32 %sum, %h";
  ASSERT_NE(Text.find(Sum2), std::string::npos);
  Text.replace(Text.find(Sum2), Sum2.size(), "%sum2 = add i32 %sumh, %h");

  SMDiagnostic Err;
  auto M = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(M) << "hop module failed to parse";
  std::string Error;
  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("stab_entry"), FAM);

  EXPECT_EQ(Pass.getStats().tableReplaced, 3u);
  EXPECT_EQ(Pass.getStats().refusedShape, 1u);
  // The live load and the whole-element-hop load stay on the source global.
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("stab_entry"), "g_cfg"), 2u);
}

/// InstCombine rewrites `bitcast (load float)` into `load i32`. The pass must
/// still recognize the field and reinterpret bit-exactly.
TEST_F(SmallTableTest, PassMatchesBitcastPunnedScalarView) {
  auto PlanM = parseModule();
  ASSERT_TRUE(PlanM);
  auto Set = makePlanSet(*PlanM);
  const EJitSmallTablePlan *Plan = Set->find("stab_entry");
  ASSERT_NE(Plan, nullptr);

  std::string Text = moduleText();
  const std::string FloatLoad =
      "%scale = load float, ptr %scalep, align 4, !ejit.may_const !1";
  const std::string IntLoad =
      "%scale = load i32, ptr %scalep, align 4, !ejit.may_const !1";
  ASSERT_NE(Text.find(FloatLoad), std::string::npos);
  Text.replace(Text.find(FloatLoad), FloatLoad.size(), IntLoad);
  const std::string FloatCast = "%fbits = bitcast float %scale to i32";
  ASSERT_NE(Text.find(FloatCast), std::string::npos);
  Text.replace(Text.find(FloatCast), FloatCast.size(),
               "%fbits = add i32 %scale, 0");
  SMDiagnostic Err;
  auto M = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(M) << "punned module failed to parse";

  std::string Error;
  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("stab_entry"), FAM);
  EXPECT_EQ(Pass.getStats().tableReplaced, 3u);

  // The scale column keeps the declared float type; the load is the i32 view
  // InstCombine produced, so the value is bit-identical to the AOT bitcast.
  GlobalVariable *ScaleCol = M->getNamedGlobal("__ejit_stab_stab_entry_c2");
  ASSERT_NE(ScaleCol, nullptr);
  EXPECT_EQ(ScaleCol->getValueType(),
            ArrayType::get(Type::getFloatTy(Ctx), kCells * kTrps * kPhases));
  EXPECT_EQ(elementBits(ScaleCol->getInitializer(), 1),
            static_cast<uint64_t>(bitsFromFloat(g_cfg[0][0][1].scale)));
}

/// A nested helper is a boundary: the plan describes the entry's own arguments,
/// and the EJIT optimizer pipeline does not inline in the Baseline path, so a
/// callee's may_const loads must keep their original form. Fields that the AOT
/// inliner already folded into the entry are ordinary entry loads and are
/// covered by the plan.
TEST_F(SmallTableTest, NestedCalleeLoadsStayOutsideThePlan) {
  std::string Text = moduleTargetHeader() + R"(
    %Big = type { i32, [124 x i32], i32, [124 x i32], float, [3 x i32], i32, i32 }
    @g_cfg = external global [6 x [2 x [10 x %Big]]]

    define internal i32 @stab_callee(i32 %c, i32 %t, i32 %s) {
    entry:
      %ph = urem i32 %s, 10
      %r = getelementptr inbounds [6 x [2 x [10 x %Big]]], ptr @g_cfg, i64 0, i32 %c, i32 %t, i32 %ph
      %gp = getelementptr inbounds %Big, ptr %r, i32 0, i32 0
      %g = load i32, ptr %gp, align 4, !ejit.may_const !1
      %up = getelementptr inbounds %Big, ptr %r, i32 0, i32 5, i32 2
      %u = load i32, ptr %up, align 4, !ejit.may_const !1
      %sum = add i32 %g, %u
      ret i32 %sum
    }

    define i32 @stab_entry(i32 %cell, i32 %trp, i32 %slotNo, i32 %x) !ejit.metadata !0 {
    entry:
      %h = call i32 @stab_callee(i32 %cell, i32 %trp, i32 %slotNo)
      %r = add i32 %h, %x
      ret i32 %r
    }

    !0 = !{!2}
    !1 = !{}
    !2 = !{!"ejit_entry"}
  )";
  SMDiagnostic Err;
  auto M = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(M) << "nested-callee module failed to parse";

  // The plan comes from the entry-only module: fields at offsets 0, 500, 1000.
  auto PlanM = parseModule();
  ASSERT_TRUE(PlanM);
  auto Set = makePlanSet(*PlanM);

  PeriodArrayRegistry &Registry = makeRegistry();
  EJitOptimizer Opt(Registry);
  Opt.setSmallTablePlans(Set);
  SpecializationContext C = baselineCtx();
  Opt.runPipeline(*M, C);

  Function &Entry = *M->getFunction("stab_entry");
  Function &Callee = *M->getFunction("stab_callee");
  EXPECT_EQ(countTableLoads(Entry, EJitSmallTablePlan::TableGlobalPrefix), 0u);
  EXPECT_EQ(countLoadsRootedAt(Entry, "g_cfg"), 0u);
  // The callee keeps both may_const loads; its parameters are not the entry's
  // args, so the plan must not claim them.
  EXPECT_EQ(countLoadsRootedAt(Callee, "g_cfg"), 2u);
}

/// The %5 form (real slotNo 0..1023, wrapping to 0) over two full wraps.
TEST_F(SmallTableTest, JitSlotModuloFiveTwoFullWrapsMatchAot) {
  std::string Text = moduleText();
  const std::string Ten = "%phase = urem i32 %slotNo, 10";
  ASSERT_NE(Text.find(Ten), std::string::npos);
  Text.replace(Text.find(Ten), Ten.size(), "%phase = urem i32 %slotNo, 5");
  SMDiagnostic Err;
  auto M = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(M);

  SmallVector<EJitSmallTableDim, 4> Dims;
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, kCells});
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 1, 0, kTrps});
  Dims.push_back({EJitSmallTableDim::Kind::ModuloArgument, 2, 5, 5});
  SmallVector<EJitSmallTableRowKey, 64> Rows;
  for (unsigned C = 0; C < kCells; ++C)
    for (unsigned T = 0; T < kTrps; ++T)
      for (unsigned P = 0; P < 5; ++P)
        Rows.push_back({{C, T, P}});
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", Dims,
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]),
                           sizeof(g_cfg)},
      Rows, {}, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  EXPECT_EQ(Plan->numRows(), kCells * kTrps * 5);
  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(std::move(*Plan)));

  PeriodArrayRegistry &Registry = makeRegistry();
  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab06);
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab06, "stab_entry");
  ASSERT_NE(Fn, nullptr);

  for (unsigned Wrap = 0; Wrap < 2; ++Wrap)
    for (unsigned C = 0; C < kCells; ++C)
      for (unsigned T = 0; T < kTrps; ++T)
        for (unsigned S = 0; S < 1024; ++S) {
          const int32_t X = static_cast<int32_t>(S % 41);
          EXPECT_EQ(Fn(C, T, S, X), aotResultMod(C, T, S, X, 5))
              << "cell=" << C << " trp=" << T << " slot=" << S;
        }
}

//===----------------------------------------------------------------------===//
// Scalar-width regressions (R1 F1/F2/F3)
//===----------------------------------------------------------------------===//

/// R1 F1: a sub-byte integer load reads `accessSize` bytes of storage, but its
/// typed value is the low `bitWidth` bits. The bytes in this fixture are
/// non-canonical (byte 0 is 0xFE), so before the mask the i1 column element was
/// built from 0xFE and the i9 column from 0x0014 -> `APInt(1, 254)` /
/// `APInt(9, 0x114)` aborted the planner under assertions and were silently
/// truncated under NDEBUG. The masked read is also what makes the emitted
/// column a faithful copy of the source's typed value.
TEST_F(SmallTableTest, PlannerMasksSubByteFieldsToTheirWidth) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(widthModuleText(), Err, Ctx);
  ASSERT_TRUE(M);
  auto Set = makeWidthPlanSet(*M);
  const EJitSmallTablePlan *Plan = Set->find("w_entry");
  ASSERT_NE(Plan, nullptr);

  // Four authorized loads, three of them sharing the same one- or two-byte
  // storage: i1(off 0), i8(off 0), i16(off 0), i9(off 4); the ordinary live
  // load at offset 8 stays original. The i16 load at offset 0 covers bytes
  // 0..1, i.e. the `bits` byte and the `pad` byte (0x5AFE), not the `small`
  // field at offset 2.
  ASSERT_EQ(Plan->fields.size(), 4u);
  EXPECT_EQ(Plan->fields[0].bitWidth, 1u);
  EXPECT_EQ(Plan->fields[0].accessSize, 1u);
  EXPECT_EQ(Plan->fields[1].bitWidth, 8u);
  EXPECT_EQ(Plan->fields[1].accessSize, 1u);
  EXPECT_EQ(Plan->fields[2].bitWidth, 16u);
  EXPECT_EQ(Plan->fields[2].accessSize, 2u);
  EXPECT_EQ(Plan->fields[3].bitWidth, 9u);
  EXPECT_EQ(Plan->fields[3].accessSize, 2u);
  EXPECT_EQ(Plan->fields[3].sourceOffset, 4u);
  EXPECT_TRUE(Plan->allRowsReady());

  // The copied row bits are typed values: the i1 row is 0/1 and the i9 row has
  // no bit above bit 8. A raw byte copy would carry the padding bits.
  const uint64_t Row0 = 0;
  const uint64_t Row1 = 1;
  EXPECT_EQ(Plan->rows[Row0].bits[0], 0u) << "0xFE & 1";
  EXPECT_EQ(Plan->rows[Row1].bits[0], 0u) << "the same low bit in every row";
  EXPECT_EQ(Plan->rows[Row0].bits[1], 0xFEu) << "i8 keeps every bit";
  EXPECT_EQ(Plan->rows[Row0].bits[2], 0x5AFEu) << "i16 keeps every bit";
  EXPECT_EQ(Plan->rows[Row0].bits[3], 0x0014u);
  EXPECT_EQ(Plan->rows[Row1].bits[3], 0x0034u);
  EXPECT_EQ(Plan->rows[Row1].bits[3], truncateToBits(g_width[0][1].sub, 9));

  std::string Error;
  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("w_entry"), FAM);

  Function &F = *M->getFunction("w_entry");
  EXPECT_EQ(Pass.getStats().tableReplaced, 4u);
  EXPECT_EQ(Pass.getStats().keptOriginal, 1u)
      << "the same-row live i32 load is not a planned field";
  EXPECT_EQ(countTableLoads(F, EJitSmallTablePlan::TableGlobalPrefix), 4u);
  EXPECT_EQ(countLoadsRootedAt(F, "g_width"), 1u);
  EXPECT_EQ(countLoadsRootedAt(F, "g_width_free"), 1u)
      << "the plan does not cover g_width_free";

  // The emitted i1 and i9 columns hold the masked values, and the i8/i16
  // columns still hold the full bytes: the widths did not collapse into one
  // column.
  GlobalVariable *BCol = M->getNamedGlobal("__ejit_stab_w_entry_c0");
  GlobalVariable *CCol = M->getNamedGlobal("__ejit_stab_w_entry_c1");
  GlobalVariable *SCol = M->getNamedGlobal("__ejit_stab_w_entry_c2");
  GlobalVariable *NCol = M->getNamedGlobal("__ejit_stab_w_entry_c3");
  ASSERT_NE(BCol, nullptr);
  ASSERT_NE(CCol, nullptr);
  ASSERT_NE(SCol, nullptr);
  ASSERT_NE(NCol, nullptr);
  EXPECT_EQ(BCol->getValueType(),
            ArrayType::get(Type::getInt1Ty(Ctx), kWidthCells * kWidthPhases));
  EXPECT_EQ(CCol->getValueType(),
            ArrayType::get(Type::getInt8Ty(Ctx), kWidthCells * kWidthPhases));
  EXPECT_EQ(SCol->getValueType(),
            ArrayType::get(Type::getInt16Ty(Ctx), kWidthCells * kWidthPhases));
  EXPECT_EQ(NCol->getValueType(),
            ArrayType::get(Type::getIntNTy(Ctx, 9), kWidthCells * kWidthPhases));
  EXPECT_EQ(elementBits(BCol->getInitializer(), 0), 0u);
  EXPECT_EQ(elementBits(BCol->getInitializer(), 1), 0u);
  EXPECT_EQ(elementBits(CCol->getInitializer(), 0), 0xFEu);
  EXPECT_EQ(elementBits(SCol->getInitializer(), 0), 0x5AFEu);
  EXPECT_EQ(elementBits(NCol->getInitializer(), 0), 0x0014u);
  EXPECT_EQ(elementBits(NCol->getInitializer(), 1), 0x0034u);
  // The ninth bit is real: the phase-3 row is 0x74 and stays 9 bits wide.
  EXPECT_EQ(elementBits(NCol->getInitializer(), 3), 0x0074u);
}

/// R1 F2: site identity is (offset, accessSize, bitWidth). Before the fix the
/// planner deduplicated by (offset, accessSize) keeping the first load's width,
/// so the i8 and i16 loads at offset 0 shared the i1 column and were coerced
/// with zext: every value became 0 or 1 (silent wrong code, order-dependent
/// because it depended on which load came first). The three columns must now
/// carry three different values and the compiled entry must reproduce the
/// source's typed values exactly.
TEST_F(SmallTableTest, JitMixedWidthSitesKeepTheirOwnColumns) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(widthModuleText(), Err, Ctx);
  ASSERT_TRUE(M);
  auto Set = makeWidthPlanSet(*M);

  PeriodArrayRegistry &Registry = makeRegistry();
  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab0a, "w_entry");
  ASSERT_NE(Engine, nullptr);
  auto Fn =
      lookupSealed<int32_t (*)(uint32_t, uint32_t)>(*Engine, 0x57ab0a, "w_entry");
  ASSERT_NE(Fn, nullptr);

  EXPECT_EQ(g_width_free[0], 0x500);
  for (unsigned Wrap = 0; Wrap < 2; ++Wrap)
    for (unsigned C = 0; C < kWidthCells; ++C)
      for (unsigned P = 0; P < kWidthPhases; ++P) {
        const WidthElement &E = g_width[C][P];
        // The JIT reads the typed value of each load: the i1 load is the low
        // bit, the i8 load the whole byte, the i16 load the two bytes at offset
        // 0 (bits and pad), the i9 load nine bits; the live field and the
        // unplanned g_width_free[0] are ordinary loads.
        const uint64_t I16 = truncateToBits(
            static_cast<uint64_t>(E.bits) |
                (static_cast<uint64_t>(E.pad) << 8),
            16);
        const uint64_t Expected = (E.bits & 1u) + static_cast<uint64_t>(E.bits) +
                                  I16 + truncateToBits(E.sub, 9) + E.live +
                                  static_cast<uint64_t>(g_width_free[0]);
        EXPECT_EQ(Fn(C, P), static_cast<int32_t>(Expected))
            << "cell=" << C << " phase=" << P;
      }
}

/// R1 F3: the uniform admission contract is a typed bit value of exactly the
/// field's width. A raw storage byte (0xFE for an i1 field) is not a value of
/// that type, so it is refused instead of truncated; an in-range value that
/// every ready row confirms is still accepted and folds to the same masked
/// typed value the table path would have read.
TEST_F(SmallTableTest, UniformContractIsRefusedOutsideTheFieldWidth) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(widthModuleText(), Err, Ctx);
  ASSERT_TRUE(M);

  SmallVector<EJitSmallTableDim, 2> Dims;
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, kWidthCells});
  Dims.push_back({EJitSmallTableDim::Kind::Argument, 1, 0, kWidthPhases});
  SmallVector<EJitSmallTableRowKey, 8> WRows;
  for (unsigned C = 0; C < kWidthCells; ++C)
    for (unsigned P = 0; P < kWidthPhases; ++P)
      WRows.push_back({{C, P}});
  const auto *Base = reinterpret_cast<const uint8_t *>(&g_width[0][0]);

  // 0xFE does not fit an i1 field: 0xFE != 1 as an i1 value, so the contract
  // must be refused rather than silently truncated to i1 0.
  std::string Error;
  std::vector<std::optional<uint64_t>> RawByte = {uint64_t{0xFE}, std::nullopt,
                                                  std::nullopt, std::nullopt};
  auto Refused = EJitSmallTablePlanner::plan(
      *M, "w_entry", "g_width", Dims, EJitSmallTableSource{Base, sizeof(g_width)},
      WRows, RawByte, Error);
  EXPECT_FALSE(Refused.has_value());
  // 0xFE is not the i1 value 1, so the refusal may be reported either as a raw
  // value that does not fit the field width or as a contract the masked rows
  // contradict; both are refusals of the same non-typed value and neither is a
  // silent truncation to i1 0.
  EXPECT_TRUE(Error.find("width") != std::string::npos ||
              Error.find("violated") != std::string::npos)
      << Error;

  // i1 value 0 is in range and every row's masked read confirms it (every
  // `bits` byte has a clear low bit), so it is admitted and has no table
  // payload.
  Error.clear();
  std::vector<std::optional<uint64_t>> Bool = {uint64_t{0}, std::nullopt,
                                               std::nullopt, std::nullopt};
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "w_entry", "g_width", Dims, EJitSmallTableSource{Base, sizeof(g_width)},
      WRows, Bool, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  ASSERT_TRUE(Plan->fields[0].uniformValue.has_value());
  EXPECT_EQ(*Plan->fields[0].uniformValue, 0u);
  EXPECT_TRUE(Plan->fields[0].columnName.empty());

  // The folded IR is the i1 typed value the table would have read: the i1 load
  // is gone and every `zext i1 ... to i32` that consumed it folded to the
  // 32-bit zero the pass produced from the admitted i1 contract. The raw 0xFE
  // byte never appears as that value.
  std::string MErr;
  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &MErr)) << MErr;
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("w_entry"), FAM);
  EXPECT_EQ(Pass.getStats().uniformFolded, 1u);
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_w_entry_c0"), nullptr)
      << "a uniform field has no table payload";
  Function &F = *M->getFunction("w_entry");
  // InstCombine folds the replaced load's `zext i1 %v0 to i32` into the
  // constant itself, so the folded value is visible as an i1 operand of the
  // surviving `zext`/`add`. Look at operands, not at instructions.
  unsigned FoldedI1 = 0;
  bool SawRawByte = false;
  for (Instruction &I : instructions(F))
    for (Value *Op : I.operands()) {
      auto *CI = dyn_cast<ConstantInt>(Op);
      if (!CI)
        continue;
      if (CI->getType()->isIntegerTy(1)) {
        ++FoldedI1;
        EXPECT_EQ(CI->getZExtValue(), 0u);
      }
      if (CI->getZExtValue() == 0xFEu)
        SawRawByte = true;
    }
  EXPECT_GE(FoldedI1, 1u)
      << "the admitted i1 contract folded to the typed i1 constant";
  EXPECT_FALSE(SawRawByte)
      << "the raw 0xFE storage byte must never be the folded value";
  // The remaining three table columns still exist, and the folded i1 load no
  // longer roots at g_width.
  EXPECT_NE(M->getNamedGlobal("__ejit_stab_w_entry_c1"), nullptr);
  EXPECT_NE(M->getNamedGlobal("__ejit_stab_w_entry_c2"), nullptr);
  EXPECT_NE(M->getNamedGlobal("__ejit_stab_w_entry_c3"), nullptr);
  EXPECT_EQ(countLoadsRootedAt(F, "g_width"), 1u) << "only the live i32 load";
}

//===----------------------------------------------------------------------===//
// Whole-entry readiness: the legacy fold behind a lowered plan
//===----------------------------------------------------------------------===//

/// PR231 whole-entry readiness contract. `g_width_free` is a registered period
/// static variable with a declared may_const field, so the legacy compile-time
/// fold can and does freeze it. When a small-table plan is lowered for the
/// entry, the compiled body is shared across the plan's declared domain and the
/// plan records no contract for that load: keeping the fold would leave a
/// hidden constant dependency that neither !ejit.smalltable.column nor
/// !ejit.smalltable.contract describes. The block must therefore keep it as a
/// real load, and an entry without a lowered plan must keep the baseline fold.
TEST_F(SmallTableTest, LoweredPlanBlocksTheLegacyFoldItDoesNotDescribe) {
  // Count the surviving loads rooted at the unplanned global. The legacy fold
  // replaces the load with a constant read out of the registered array, so a
  // successful fold leaves no load behind; a blocked fold leaves exactly one.
  auto CountFreeLoads = [&](bool WithPlan) -> unsigned {
    SMDiagnostic Err;
    auto M = parseAssemblyString(widthModuleText(), Err, Ctx);
    if (!M)
      return 0;
    PeriodArrayRegistry &Registry = makeRegistry();
    EJitOptimizer Opt(Registry);
    if (WithPlan)
      Opt.setSmallTablePlans(makeWidthPlanSet(*M));
    SpecializationContext C = baselineCtx("w_entry");
    Opt.runPipeline(*M, C);
    Function &F = *M->getFunction("w_entry");
    return countLoadsRootedAt(F, "g_width_free");
  };

  EXPECT_EQ(CountFreeLoads(/*WithPlan=*/false), 0u)
      << "without a plan the legacy fold must still apply";
  EXPECT_GE(CountFreeLoads(/*WithPlan=*/true), 1u)
      << "a lowered plan must not freeze a load it does not describe";
}

//===----------------------------------------------------------------------===//
// Per-compile table identity and the uniform-contract handoff (PR231 -> B)
//===----------------------------------------------------------------------===//

/// The compiler hands B a name, not an address: the column symbol does not exist
/// before the specialization is materialized, is defined by exactly the compile
/// that was handed the same name, and stays stable for later row publication
/// inside that compile. Each compile has its own JITDylib, so the same spelling
/// is a different object/address in another compile and B must publish into the
/// address of the compile it is admitting rows to.
TEST_F(SmallTableTest, TableIdentityIsPerCompileAndHandedOffByName) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(widthModuleText(), Err, Ctx);
  ASSERT_TRUE(M);
  auto Set = makeWidthPlanSet(*M);
  const EJitSmallTablePlan *Plan = Set->find("w_entry");
  ASSERT_NE(Plan, nullptr);

  // The compiler-side handoff: the column names of the lowered plan are exactly
  // the non-uniform fields (the uniform field has no payload), in plan order.
  SmallVector<std::string, 4> ExpectedNames;
  for (const EJitSmallTableField &Field : Plan->fields)
    if (!Field.uniformValue)
      ExpectedNames.push_back(Field.columnName);
  ASSERT_EQ(ExpectedNames.size(), 4u);
  EXPECT_EQ(ExpectedNames[0], "__ejit_stab_w_entry_c0");
  EXPECT_TRUE(std::all_of(ExpectedNames.begin(), ExpectedNames.end(),
                          [](const std::string &N) {
                            return StringRef(N).starts_with(
                                EJitSmallTablePlan::TableGlobalPrefix);
                          }));

  PeriodArrayRegistry &Registry = makeRegistry();
  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab0b, "w_entry");
  ASSERT_NE(Engine, nullptr);
  ASSERT_EQ(Engine->getLastSmallTableColumnNames().size(), 4u);
  for (unsigned I = 0; I < 4; ++I)
    EXPECT_EQ(Engine->getLastSmallTableColumnNames()[I], ExpectedNames[I]);

  // The handed-off name resolves inside this compile, and only inside it: a
  // different cache key has its own JITDylib, so the same spelling is not a
  // process-wide symbol the runtime could publish into by name alone.
  auto Resolved = Engine->lookup(0x57ab0b, ExpectedNames[0]);
  EXPECT_TRUE(static_cast<bool>(Resolved))
      << "the just-compiled table must resolve by its handed-off name";
  if (!Resolved)
    consumeError(Resolved.takeError());
  auto Foreign = Engine->lookup(0xDEADBEEF, ExpectedNames[0]);
  EXPECT_FALSE(static_cast<bool>(Foreign))
      << "a column name is not a process-wide symbol";
  if (!Foreign)
    consumeError(Foreign.takeError());

  auto AddrOrErr = Engine->lookup(0x57ab0b, ExpectedNames[0]);
  ASSERT_TRUE(static_cast<bool>(AddrOrErr)) << toString(AddrOrErr.takeError());
  void *TableAddr = *AddrOrErr;
  EXPECT_NE(TableAddr, nullptr);
  // Stable within the compile: the runtime's later row publication targets this
  // one address, not a per-lookup or per-materialization copy.
  auto AgainOrErr = Engine->lookup(0x57ab0b, ExpectedNames[0]);
  ASSERT_TRUE(static_cast<bool>(AgainOrErr));
  EXPECT_EQ(*AgainOrErr, TableAddr);
  EXPECT_FALSE(errorToBool(Engine->flushPendingCode()));

  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t)>(*Engine, 0x57ab0b,
                                                         "w_entry");
  ASSERT_NE(Fn, nullptr);
  const WidthElement &E3 = g_width[0][3];
  const uint64_t I16 = truncateToBits(
      static_cast<uint64_t>(E3.bits) | (static_cast<uint64_t>(E3.pad) << 8), 16);
  EXPECT_EQ(Fn(0, 3),
            static_cast<int32_t>((E3.bits & 1u) + static_cast<uint64_t>(E3.bits) +
                                 I16 + truncateToBits(E3.sub, 9) + E3.live +
                                 static_cast<uint64_t>(g_width_free[0])));
}

//===----------------------------------------------------------------------===//
// A1: automatic per-field specialization, axis elimination and the exported
// admission contract (spec §4.1/§6.6; no caller-supplied uniform value)
//===----------------------------------------------------------------------===//

/// The dynamic index of the first table load rooted at \p ColumnName, with
/// integer casts stripped, plus how many dynamic indices the address has.
const Value *tableLoadIndex(const Function &F, StringRef ColumnName,
                            unsigned *DynamicCount = nullptr) {
  for (const Instruction &I : instructions(F)) {
    const auto *LI = dyn_cast<LoadInst>(&I);
    if (!LI)
      continue;
    if (rootGVOf(LI->getPointerOperand()) == nullptr ||
        rootGVOf(LI->getPointerOperand())->getName() != ColumnName)
      continue;
    SmallVector<const Value *, 4> Dyn;
    const Value *V = LI->getPointerOperand()->stripPointerCasts();
    while (const auto *GEP = dyn_cast<GEPOperator>(V)) {
      for (auto GTI = gep_type_begin(GEP), GTE = gep_type_end(GEP); GTI != GTE;
           ++GTI) {
        if (GTI.getStructTypeOrNull())
          continue;
        if (!isa<ConstantInt>(GTI.getOperand()))
          Dyn.push_back(GTI.getOperand());
      }
      V = GEP->getPointerOperand()->stripPointerCasts();
    }
    if (DynamicCount)
      *DynamicCount = Dyn.size();
    if (Dyn.empty())
      return nullptr;
    const Value *Idx = Dyn.back();
    while (const auto *CI = dyn_cast<CastInst>(Idx))
      Idx = CI->getOperand(0);
    return Idx;
  }
  return nullptr;
}

/// The typed member values the contract's fields describe, read out of a real
/// AutoElement (the same masked typed values the planner copies).
EJitSmallTableMember autoMember(const AutoElement &E,
                                ArrayRef<uint64_t> Indices) {
  EJitSmallTableMember M;
  M.indices.append(Indices.begin(), Indices.end());
  M.bits.push_back(static_cast<uint32_t>(E.mode));
  M.bits.push_back(static_cast<uint32_t>(E.byCell));
  M.bits.push_back(static_cast<uint32_t>(E.byTrp));
  M.bits.push_back(static_cast<uint32_t>(E.joint));
  return M;
}

/// The §4.1 solver on a domain where each strategy case occurs: one field equal
/// over the whole domain, one that differs per cell only, one per TRP only, one
/// jointly. No caller-supplied uniform value is involved anywhere.
TEST_F(SmallTableTest, AutomaticSolverFoldsUniformAndEliminatesAxes) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("a_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "auto module failed to parse";

  EJitSmallTableRequest Req;
  Req.module = M.get();
  Req.entryName = "a_entry";
  Req.sourceVarName = "g_auto";
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  Req.dims = Dims;
  Req.source = EJitSmallTableSource{
      reinterpret_cast<const uint8_t *>(&g_auto[0][0]), sizeof(g_auto)};
  SmallVector<EJitSmallTableRowKey, 16> Rows = autoRows(kAutoCells, kAutoTrps);
  Req.authorizedRows = Rows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness = testReadiness();
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(Req, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  ASSERT_EQ(Plan->fields.size(), 4u);
  EXPECT_TRUE(Plan->allRowsReady());
  EXPECT_TRUE(Plan->verifyProjections(&Error)) << Error;

  // Field 0 is bit-exactly equal on the proven domain: constant, and NO column,
  // no payload and no table load (spec §4.1 row 1).
  const EJitSmallTableField &Mode = Plan->fields[0];
  EXPECT_EQ(Mode.strategy, EJitSmallTableStrategy::Uniform);
  ASSERT_TRUE(Mode.uniformValue.has_value());
  EXPECT_EQ(*Mode.uniformValue, 1u);
  EXPECT_TRUE(Mode.columnName.empty());
  EXPECT_TRUE(Mode.retainedAxes.empty());
  EXPECT_EQ(Mode.tableRows, 0u);
  EXPECT_EQ(Mode.tableBytes, 0u);
  EXPECT_FALSE(Mode.uniformFromContract)
      << "the automatic solver must not be recorded as a caller contract";

  // Field 1 differs with cell only: cell retained, TRP eliminated.
  const EJitSmallTableField &ByCell = Plan->fields[1];
  EXPECT_EQ(ByCell.strategy, EJitSmallTableStrategy::Table);
  ASSERT_EQ(ByCell.retainedAxes.size(), 1u);
  EXPECT_EQ(ByCell.retainedAxes[0], 0u);
  EXPECT_EQ(ByCell.tableRows, kAutoCells);
  EXPECT_EQ(ByCell.tableBytes, kAutoCells * 4);
  EXPECT_EQ(ByCell.columnName, "__ejit_stab_a_entry_c1");

  // Field 2 differs with TRP only: TRP retained, cell eliminated.
  const EJitSmallTableField &ByTrp = Plan->fields[2];
  EXPECT_EQ(ByTrp.strategy, EJitSmallTableStrategy::Table);
  ASSERT_EQ(ByTrp.retainedAxes.size(), 1u);
  EXPECT_EQ(ByTrp.retainedAxes[0], 1u);
  EXPECT_EQ(ByTrp.tableRows, kAutoTrps);
  EXPECT_EQ(ByTrp.tableBytes, kAutoTrps * 4);
  EXPECT_EQ(ByTrp.columnName, "__ejit_stab_a_entry_c2");

  // Field 3 differs jointly: both axes kept, no representative row invented.
  const EJitSmallTableField &Joint = Plan->fields[3];
  EXPECT_EQ(Joint.strategy, EJitSmallTableStrategy::Table);
  ASSERT_EQ(Joint.retainedAxes.size(), 2u);
  EXPECT_EQ(Joint.retainedAxes[0], 0u);
  EXPECT_EQ(Joint.retainedAxes[1], 1u);
  EXPECT_EQ(Joint.tableRows, kAutoCells * kAutoTrps);

  // Accounting (§13): three columns over the axis products instead of four full
  // tables, and the folded field occupies nothing at all.
  EXPECT_EQ(Plan->tableFieldCount(), 3u);
  EXPECT_EQ(Plan->uniformFieldCount(), 1u);
  EXPECT_EQ(Plan->tableBytes(),
            (kAutoCells + kAutoTrps + kAutoCells * kAutoTrps) * 4);
  EXPECT_LT(Plan->tableBytes(), Plan->numRows() * 4 * 4);

  // The projected coordinate of the TRP-only field is the real TRP index, and
  // the cell coordinate does not enter it.
  EXPECT_EQ(Plan->projectRow(ByTrp, 2 * kAutoTrps + 1), 1u);
  EXPECT_EQ(Plan->projectRow(ByCell, 2 * kAutoTrps + 1), 2u);
  EXPECT_EQ(Plan->fieldRowStride(Joint, 0), kAutoTrps);
  EXPECT_EQ(Plan->fieldRowStride(Joint, 1), 1u);

  // The readiness identity is recorded, never invented.
  EXPECT_EQ(Plan->mode, EJitSmallTablePlanMode::Automatic);
  EXPECT_EQ(Plan->readiness.domainEpoch, 0x5EED20260914ull);
  EXPECT_EQ(Plan->readiness.providerLabel, "test.provider.compiler-boundary");
  EXPECT_TRUE(Plan->readiness.coversDeclaredDomain);
  EXPECT_TRUE(Plan->readiness.borrowedStable);

  // Every coordinate of a lowered column carries its own projected value, taken
  // from the real source rows.
  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  GlobalVariable *ByTrpCol = M->getNamedGlobal("__ejit_stab_a_entry_c2");
  ASSERT_NE(ByTrpCol, nullptr);
  EXPECT_EQ(ByTrpCol->getValueType(), ArrayType::get(Type::getInt32Ty(Ctx), 3));
  for (unsigned T = 0; T < kAutoTrps; ++T)
    EXPECT_EQ(elementBits(ByTrpCol->getInitializer(), T),
              static_cast<uint64_t>(g_auto[0][T].byTrp));
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_a_entry_c0"), nullptr)
      << "the uniform field must not get a column";
}

/// The IR half: each field is lowered with only its own axes, the uniform field
/// folds to a constant with no load, and the branch that constant decides really
/// disappears — checked on real IR through the real optimizer pipeline.
TEST_F(SmallTableTest, AutomaticSolverLowersEachFieldWithItsOwnAxes) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("a_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "auto module failed to parse";

  EJitSmallTableRequest Req;
  Req.module = M.get();
  Req.entryName = "a_entry";
  Req.sourceVarName = "g_auto";
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  Req.dims = Dims;
  Req.source = EJitSmallTableSource{
      reinterpret_cast<const uint8_t *>(&g_auto[0][0]), sizeof(g_auto)};
  SmallVector<EJitSmallTableRowKey, 16> Rows = autoRows(kAutoCells, kAutoTrps);
  Req.authorizedRows = Rows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness = testReadiness();
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(Req, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;

  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("a_entry"), FAM);

  Function &F = *M->getFunction("a_entry");
  EXPECT_EQ(Pass.getStats().uniformFolded, 1u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 3u);
  // No authorized load survives on the source object: the folded field has no
  // load at all and the other three read their own projected column.
  EXPECT_EQ(countLoadsRootedAt(F, "g_auto"), 0u);
  EXPECT_EQ(countTableLoads(F, EJitSmallTablePlan::TableGlobalPrefix), 3u);
  EXPECT_EQ(countTaggedTableLoads(F), 3u);

  // The cell-only column is addressed by the cell argument alone (its TRP axis
  // was eliminated), and the TRP-only column by the TRP argument alone.
  EXPECT_EQ(tableLoadIndex(F, "__ejit_stab_a_entry_c1"), F.getArg(0));
  EXPECT_EQ(tableLoadIndex(F, "__ejit_stab_a_entry_c2"), F.getArg(1));
  // The joint column keeps the real computed row index, which uses both.
  unsigned JointDyn = 0;
  const Value *JointIdx = tableLoadIndex(F, "__ejit_stab_a_entry_c3", &JointDyn);
  ASSERT_NE(JointIdx, nullptr);
  EXPECT_EQ(JointDyn, 1u) << "one computed row index, not two raw axes";
  EXPECT_FALSE(isa<Argument>(JointIdx));
  EXPECT_GT(F.getArg(0)->getNumUses(), 0u);
  EXPECT_GT(F.getArg(1)->getNumUses(), 0u);

  // The emitted columns have the projected sizes, not the full domain size.
  GlobalVariable *CellCol = M->getNamedGlobal("__ejit_stab_a_entry_c1");
  GlobalVariable *JointCol = M->getNamedGlobal("__ejit_stab_a_entry_c3");
  ASSERT_NE(CellCol, nullptr);
  ASSERT_NE(JointCol, nullptr);
  EXPECT_EQ(CellCol->getValueType(), ArrayType::get(Type::getInt32Ty(Ctx), 4));
  EXPECT_EQ(JointCol->getValueType(), ArrayType::get(Type::getInt32Ty(Ctx), 12));
  EXPECT_FALSE(CellCol->isConstant());
  EXPECT_FALSE(JointCol->isConstant());
  EXPECT_TRUE(
      F.hasMetadata("ejit.smalltable.contract"))
      << "the entry must carry the exported admission contract record";

  // Whole-pipeline proof that the constants fold: the may_const load is gone (it
  // is the constant), so the `icmp eq %mode, 1` that decided the branch and the
  // select itself fold away, while the real arguments, store and helper-style
  // arithmetic stay.
  auto PipeM = parseAssemblyString(autoModuleText("a_entry", "g_auto",
                                                  "g_auto_out", kAutoCells,
                                                  kAutoTrps),
                                   Err, Ctx);
  ASSERT_TRUE(PipeM) << "auto module failed to re-parse";
  PeriodArrayRegistry &Registry = makeRegistry();
  EJitOptimizer Opt(Registry);
  auto PipeSet = std::make_shared<EJitSmallTablePlanSet>();
  PipeSet->add(std::make_shared<const EJitSmallTablePlan>(*Plan));
  Opt.setSmallTablePlans(PipeSet);
  SpecializationContext C = baselineCtx("a_entry");
  Opt.runPipeline(*PipeM, C);
  Function &PF = *PipeM->getFunction("a_entry");
  EXPECT_EQ(countLoadsRootedAt(PF, "g_auto"), 0u);
  for (const Instruction &I : instructions(PF)) {
    EXPECT_FALSE(isa<SelectInst>(&I))
        << "the branch the folded constant decides must be gone";
    if (const auto *IC = dyn_cast<ICmpInst>(&I))
      EXPECT_NE(IC->getOperand(1), ConstantInt::get(Type::getInt32Ty(Ctx), 1))
          << "the invariant comparison must have folded";
  }
  EXPECT_GT(PF.getArg(0)->getNumUses(), 0u);
  EXPECT_GT(PF.getArg(1)->getNumUses(), 0u);
  EXPECT_GT(PF.getArg(2)->getNumUses(), 0u);
}

/// Real ORC execution of the automatic plan: every (cell, TRP) member and a
/// range of real x values must match the AOT reference, which is only possible
/// if each field's projected index is exactly its own axis and the folded
/// constant is the domain's true value.
TEST_F(SmallTableTest, JitAutomaticSpecializationMatchesAotEveryRow) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("a_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "auto module failed to parse";

  EJitSmallTableRequest Req;
  Req.module = M.get();
  Req.entryName = "a_entry";
  Req.sourceVarName = "g_auto";
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  Req.dims = Dims;
  Req.source = EJitSmallTableSource{
      reinterpret_cast<const uint8_t *>(&g_auto[0][0]), sizeof(g_auto)};
  SmallVector<EJitSmallTableRowKey, 16> Rows = autoRows(kAutoCells, kAutoTrps);
  Req.authorizedRows = Rows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness = testReadiness();
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(Req, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(std::move(*Plan)));

  PeriodArrayRegistry &Registry = makeRegistry();
  std::memset(g_auto_out, 0, sizeof(g_auto_out));
  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab20, "a_entry");
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab20, "a_entry");
  ASSERT_NE(Fn, nullptr);

  // The engine publishes exactly the three projected columns; the folded field
  // has no table to publish into.
  ArrayRef<std::string> Names = Engine->getLastSmallTableColumnNames();
  ASSERT_EQ(Names.size(), 3u);
  EXPECT_EQ(Names[0], "__ejit_stab_a_entry_c1");
  EXPECT_EQ(Names[1], "__ejit_stab_a_entry_c2");
  EXPECT_EQ(Names[2], "__ejit_stab_a_entry_c3");

  for (unsigned C = 0; C < kAutoCells; ++C)
    for (unsigned T = 0; T < kAutoTrps; ++T)
      for (int32_t X = -3; X <= 9; ++X) {
        EXPECT_EQ(Fn(C, T, X), aotAuto(g_auto[C][T], X))
            << "cell=" << C << " trp=" << T << " x=" << X;
      }
  EXPECT_EQ(g_auto_out[0], 9);
  EXPECT_EQ(g_auto_out[kAutoCells - 1], 9)
      << "the store still targets the real per-cell output address";
}

/// Sparse/checkerboard domains: the solver must decide on the COMPLETE proven
/// domain, deterministically, and must never delete two axes because a single
/// axis had no comparable neighbour (spec §4.1).
TEST_F(SmallTableTest, AutomaticSolverUsesTheWholeDomainDeterministically) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("s_entry", "g_sparse",
                                              "g_sparse_out", 2, 2),
                               Err, Ctx);
  ASSERT_TRUE(M) << "sparse module failed to parse";

  auto PlanFor = [&](ArrayRef<EJitSmallTableRowKey> Rows,
                     std::string &Error) {
    SmallVector<EJitSmallTableDim, 2> Dims = autoDims(2, 2);
    EJitSmallTableRequest Req;
    Req.module = M.get();
    Req.entryName = "s_entry";
    Req.sourceVarName = "g_sparse";
    Req.dims = Dims;
    Req.source = EJitSmallTableSource{
        reinterpret_cast<const uint8_t *>(&g_sparse[0][0]), sizeof(g_sparse)};
    Req.authorizedRows = Rows;
    Req.mode = EJitSmallTablePlanMode::Automatic;
    Req.readiness = testReadiness();
    return EJitSmallTablePlanner::plan(Req, Error);
  };

  // Diagonal domain D = {(0,0) = 7, (1,1) = 9}: {cell} and {TRP} both prove it,
  // and the deterministic tie-break is the schema axis order, so {cell} wins.
  // The empty set fails: the values differ, so no vacuous constant is allowed.
  g_sparse[0][0].joint = 7;
  g_sparse[1][1].joint = 9;
  g_sparse[0][1].joint = 11; // outside D, must not affect the decision
  g_sparse[1][0].joint = 13;
  SmallVector<EJitSmallTableRowKey, 4> Diagonal = {{{0, 0}}, {{1, 1}}};
  std::string Error;
  auto Diag = PlanFor(Diagonal, Error);
  ASSERT_TRUE(Diag.has_value()) << Error;
  const EJitSmallTableField &DiagJoint = Diag->fields[3];
  EXPECT_EQ(DiagJoint.strategy, EJitSmallTableStrategy::Table);
  ASSERT_EQ(DiagJoint.retainedAxes.size(), 1u);
  EXPECT_EQ(DiagJoint.retainedAxes[0], 0u)
      << "equal-size candidates are decided by schema axis order";
  EXPECT_EQ(DiagJoint.tableRows, 2u);
  // A field equal on both diagonal rows is a constant inside this controlled
  // contract, with no payload.
  EXPECT_EQ(Diag->fields[2].strategy, EJitSmallTableStrategy::Uniform)
      << "byTrp is 5 on both diagonal rows";
  EXPECT_EQ(Diag->fields[2].tableBytes, 0u);
  // The unproven rows stay unready, so the partial plan can never be lowered.
  EXPECT_FALSE(Diag->allRowsReady());

  // Checkerboard on the complete domain: both single axes fail, so the joint
  // axes are kept. An implementation that proved each axis "irrelevant"
  // independently would wrongly delete both.
  g_sparse[0][0].joint = 7;
  g_sparse[0][1].joint = 9;
  g_sparse[1][0].joint = 9;
  g_sparse[1][1].joint = 7;
  SmallVector<EJitSmallTableRowKey, 4> Full = autoRows(2, 2);
  Error.clear();
  auto Checker = PlanFor(Full, Error);
  ASSERT_TRUE(Checker.has_value()) << Error;
  const EJitSmallTableField &CheckerJoint = Checker->fields[3];
  ASSERT_EQ(CheckerJoint.retainedAxes.size(), 2u);
  EXPECT_EQ(CheckerJoint.retainedAxes[0], 0u);
  EXPECT_EQ(CheckerJoint.retainedAxes[1], 1u);
  EXPECT_EQ(CheckerJoint.tableRows, 4u);
  EXPECT_TRUE(Checker->allRowsReady());
  EXPECT_TRUE(Checker->verifyProjections(&Error)) << Error;

  // Lower the checkerboard plan for real and execute it: the projected index
  // must reproduce the checkerboard exactly.
  Error.clear();
  auto JitPlan = PlanFor(Full, Error);
  ASSERT_TRUE(JitPlan.has_value()) << Error;
  auto JitM = parseAssemblyString(
      autoModuleText("s_entry", "g_sparse", "g_sparse_out", 2, 2), Err, Ctx);
  ASSERT_TRUE(JitM);
  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(std::move(*JitPlan)));
  PeriodArrayRegistry &Registry = makeRegistry();
  std::memset(g_sparse_out, 0, sizeof(g_sparse_out));
  auto Engine = compileWithEngine(*JitM, Set, Registry, 0x57ab21, "s_entry");
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab21, "s_entry");
  ASSERT_NE(Fn, nullptr);
  for (unsigned C = 0; C < 2; ++C)
    for (unsigned T = 0; T < 2; ++T)
      EXPECT_EQ(Fn(C, T, 4), aotAuto(g_sparse[C][T], 4))
          << "cell=" << C << " trp=" << T;

  // A TRP-only difference on the complete domain keeps the TRP axis and drops
  // the cell axis.
  for (unsigned C = 0; C < 2; ++C)
    for (unsigned T = 0; T < 2; ++T)
      g_sparse[C][T].joint = static_cast<int32_t>(3 + T);
  Error.clear();
  auto ByTrp = PlanFor(Full, Error);
  ASSERT_TRUE(ByTrp.has_value()) << Error;
  ASSERT_EQ(ByTrp->fields[3].retainedAxes.size(), 1u);
  EXPECT_EQ(ByTrp->fields[3].retainedAxes[0], 1u);
  EXPECT_EQ(ByTrp->fields[3].tableRows, 2u);
}

/// The default mode is automatic; an explicit comparison mode is distinctly
/// selected, an empty proven domain can never become a vacuous constant, and a
/// partially proven domain is planned but never lowered executably.
TEST_F(SmallTableTest, AutomaticModeRefusesContractsAndUnprovenDomains) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("a_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "auto module failed to parse";
  const auto *Base = reinterpret_cast<const uint8_t *>(&g_auto[0][0]);
  SmallVector<EJitSmallTableRowKey, 16> Full = autoRows(kAutoCells, kAutoTrps);

  // The default mode takes no caller contract: mixing would blur the required
  // default and the comparison primitive.
  std::vector<std::optional<uint64_t>> Contracts = {uint64_t{1}, std::nullopt,
                                                    std::nullopt, std::nullopt};
  std::string Error;
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  EJitSmallTableRequest Auto;
  Auto.module = M.get();
  Auto.entryName = "a_entry";
  Auto.sourceVarName = "g_auto";
  Auto.dims = Dims;
  Auto.source = EJitSmallTableSource{Base, sizeof(g_auto)};
  Auto.authorizedRows = Full;
  Auto.uniformContracts = Contracts;
  Auto.mode = EJitSmallTablePlanMode::Automatic;
  auto Refused = EJitSmallTablePlanner::plan(Auto, Error);
  EXPECT_FALSE(Refused.has_value());
  EXPECT_NE(Error.find("automatic mode"), std::string::npos) << Error;

  // An empty proven domain must refuse rather than produce a vacuous constant.
  Error.clear();
  SmallVector<EJitSmallTableRowKey, 1> NoRows;
  Auto.uniformContracts = {};
  Auto.authorizedRows = NoRows;
  auto Empty = EJitSmallTablePlanner::plan(Auto, Error);
  EXPECT_FALSE(Empty.has_value());
  EXPECT_NE(Error.find("empty"), std::string::npos) << Error;

  // The same schema and rows through the historical entry point select the
  // explicit comparison mode: the contract is honored there and the mode is
  // recorded on the plan, so the two modes are never conflated.
  Error.clear();
  auto Explicit = EJitSmallTablePlanner::plan(
      *M, "a_entry", "g_auto", autoDims(kAutoCells, kAutoTrps),
      EJitSmallTableSource{Base, sizeof(g_auto)}, Full, Contracts, Error);
  ASSERT_TRUE(Explicit.has_value()) << Error;
  EXPECT_EQ(Explicit->mode, EJitSmallTablePlanMode::ExplicitContracts);
  EXPECT_EQ(Explicit->fields[0].strategy, EJitSmallTableStrategy::Uniform);
  EXPECT_TRUE(Explicit->fields[0].uniformFromContract);
  // The comparison mode does not infer: every other field keeps all axes.
  for (unsigned I = 1; I < Explicit->fields.size(); ++I) {
    EXPECT_EQ(Explicit->fields[I].strategy, EJitSmallTableStrategy::Table);
    EXPECT_EQ(Explicit->fields[I].retainedAxes.size(), 2u);
    EXPECT_EQ(Explicit->fields[I].tableRows, kAutoCells * kAutoTrps);
  }

  // A partially proven automatic plan records the strategies it could prove but
  // stays non-executable: no column is created and no load is replaced, so the
  // whole-entry readiness gate survives the new planner.
  Error.clear();
  SmallVector<EJitSmallTableRowKey, 4> PartialRows = {{{0, 0}}, {{1, 0}}};
  auto Partial = EJitSmallTablePlanner::plan(
      *M, "a_entry", "g_auto", autoDims(kAutoCells, kAutoTrps),
      EJitSmallTableSource{Base, sizeof(g_auto)}, PartialRows, {}, Error);
  ASSERT_TRUE(Partial.has_value()) << Error;
  EXPECT_FALSE(Partial->allRowsReady());
  EXPECT_FALSE(Partial->domainComplete());
  EXPECT_EQ(Partial->fields[0].strategy, EJitSmallTableStrategy::Uniform);
  Error.clear();
  EXPECT_FALSE(EJitSmallTablePass::materialize(*M, *Partial, &Error));
  EXPECT_NE(Error.find("rows ready"), std::string::npos) << Error;
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_a_entry_c1"), nullptr);
  EJitSmallTablePass Pass(*Partial);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("a_entry"), FAM);
  EXPECT_EQ(Pass.getStats().uniformFolded, 0u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 0u);
  EXPECT_EQ(Pass.getStats().refusedNotReady, 1u);
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("a_entry"), "g_auto"), 4u)
      << "an unproven domain keeps every original load";
}

/// Automatic specialization on the scalar-width fixture: the two fields that are
/// bit-exactly equal on the whole domain fold with no column (an i1 field and an
/// i8 field at the same address!), the two that differ per phase keep one-axis
/// tables with their declared widths, and the ordinary i32 load stays original.
TEST_F(SmallTableTest, AutomaticWidthsKeepTheirTypedValues) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(widthModuleText(), Err, Ctx);
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kWidthCells, kWidthPhases);
  SmallVector<EJitSmallTableRowKey, 8> WRows = autoRows(kWidthCells, kWidthPhases);

  EJitSmallTableRequest Req;
  Req.module = M.get();
  Req.entryName = "w_entry";
  Req.sourceVarName = "g_width";
  Req.dims = Dims;
  Req.source = EJitSmallTableSource{
      reinterpret_cast<const uint8_t *>(&g_width[0][0]), sizeof(g_width)};
  Req.authorizedRows = WRows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness = testReadiness();
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(Req, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  ASSERT_EQ(Plan->fields.size(), 4u);

  // i1 (0xFE storage byte, low bit clear on every row) folds to the typed i1
  // value 0 — never to the raw byte.
  EXPECT_EQ(Plan->fields[0].strategy, EJitSmallTableStrategy::Uniform);
  EXPECT_EQ(Plan->fields[0].bitWidth, 1u);
  ASSERT_TRUE(Plan->fields[0].uniformValue.has_value());
  EXPECT_EQ(*Plan->fields[0].uniformValue, 0u);
  EXPECT_TRUE(Plan->fields[0].columnName.empty());
  // i8 at the same address is a different typed value and folds separately to
  // the full byte.
  EXPECT_EQ(Plan->fields[1].strategy, EJitSmallTableStrategy::Uniform);
  EXPECT_EQ(Plan->fields[1].bitWidth, 8u);
  ASSERT_TRUE(Plan->fields[1].uniformValue.has_value());
  EXPECT_EQ(*Plan->fields[1].uniformValue, 0xFEu);
  // i16 and i9 differ per phase: one-axis tables, declared widths preserved.
  EXPECT_EQ(Plan->fields[2].strategy, EJitSmallTableStrategy::Table);
  ASSERT_EQ(Plan->fields[2].retainedAxes.size(), 1u);
  EXPECT_EQ(Plan->fields[2].retainedAxes[0], 1u);
  EXPECT_EQ(Plan->fields[2].bitWidth, 16u);
  EXPECT_EQ(Plan->fields[3].strategy, EJitSmallTableStrategy::Table);
  ASSERT_EQ(Plan->fields[3].retainedAxes.size(), 1u);
  EXPECT_EQ(Plan->fields[3].retainedAxes[0], 1u);
  EXPECT_EQ(Plan->fields[3].bitWidth, 9u);
  EXPECT_EQ(Plan->tableBytes(), (kWidthPhases * 2) + (kWidthPhases * 2));

  // The exported contract keeps the typed width of the folded i1 field: a raw
  // storage byte (0xFE) is not an i1 value and can never validate as one.
  EJitSmallTableContract Contract = buildAdmissionContract(*Plan);
  ASSERT_TRUE(Contract.fields[0].requiredValue.has_value());
  EXPECT_EQ(*Contract.fields[0].requiredValue, 0u);
  EJitSmallTableMember RawByte = autoMember(g_auto[0][0], {0, 0});
  RawByte.indices = {0, 0};
  RawByte.bits = {0xFEu, 0xFEu, 0x5AFEu, 0x0014u};
  std::string Why;
  EXPECT_EQ(validateAdmission(Contract, RawByte, &Why),
            EJitSmallTableAdmission::Unusable)
      << Why;
  EXPECT_NE(Why.find("width"), std::string::npos) << Why;

  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("w_entry"), FAM);
  EXPECT_EQ(Pass.getStats().uniformFolded, 2u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 2u);
  EXPECT_EQ(Pass.getStats().keptOriginal, 1u)
      << "the ordinary live i32 load is not an authorized field";
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_w_entry_c0"), nullptr);
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_w_entry_c1"), nullptr);
  GlobalVariable *SCol = M->getNamedGlobal("__ejit_stab_w_entry_c2");
  GlobalVariable *NCol = M->getNamedGlobal("__ejit_stab_w_entry_c3");
  ASSERT_NE(SCol, nullptr);
  ASSERT_NE(NCol, nullptr);
  EXPECT_EQ(SCol->getValueType(),
            ArrayType::get(Type::getInt16Ty(Ctx), kWidthPhases));
  EXPECT_EQ(NCol->getValueType(),
            ArrayType::get(Type::getIntNTy(Ctx, 9), kWidthPhases));
  for (unsigned P = 0; P < kWidthPhases; ++P) {
    EXPECT_EQ(elementBits(SCol->getInitializer(), P),
              truncateToBits(static_cast<uint64_t>(g_width[0][P].bits) |
                                 (static_cast<uint64_t>(g_width[0][P].pad) << 8),
                             16));
    EXPECT_EQ(elementBits(NCol->getInitializer(), P),
              truncateToBits(g_width[0][P].sub, 9));
  }

  // Real execution: the folded i1/i8 constants and the per-phase tables must
  // reproduce every source row.
  auto JitM = parseAssemblyString(widthModuleText(), Err, Ctx);
  ASSERT_TRUE(JitM);
  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(*Plan));
  PeriodArrayRegistry &Registry = makeRegistry();
  auto Engine = compileWithEngine(*JitM, Set, Registry, 0x57ab22, "w_entry");
  ASSERT_NE(Engine, nullptr);
  ASSERT_EQ(Engine->getLastSmallTableColumnNames().size(), 2u);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t)>(*Engine, 0x57ab22,
                                                          "w_entry");
  ASSERT_NE(Fn, nullptr);
  for (unsigned Wrap = 0; Wrap < 2; ++Wrap)
    for (unsigned C = 0; C < kWidthCells; ++C)
      for (unsigned P = 0; P < kWidthPhases; ++P) {
        const WidthElement &E = g_width[C][P];
        const uint64_t I16 = truncateToBits(
            static_cast<uint64_t>(E.bits) | (static_cast<uint64_t>(E.pad) << 8),
            16);
        const uint64_t Expected = (E.bits & 1u) + static_cast<uint64_t>(E.bits) +
                                  I16 + truncateToBits(E.sub, 9) + E.live +
                                  static_cast<uint64_t>(g_width_free[0]);
        EXPECT_EQ(Fn(C, P), static_cast<int32_t>(Expected))
            << "cell=" << C << " phase=" << P;
      }
}

/// The exported admission contract on the compiler contract boundary: a later
/// member is classified as compatible, extendable, conflicting or unusable using
/// only the contract's own recorded identity — no module, no plan, and a clearly
/// labeled test provider standing in for the B0 configuration point.
TEST_F(SmallTableTest, AdmissionContractValidatesLaterMembers) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("a_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "auto module failed to parse";
  EJitSmallTableRequest Req;
  Req.module = M.get();
  Req.entryName = "a_entry";
  Req.sourceVarName = "g_auto";
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  Req.dims = Dims;
  Req.source = EJitSmallTableSource{
      reinterpret_cast<const uint8_t *>(&g_auto[0][0]), sizeof(g_auto)};
  SmallVector<EJitSmallTableRowKey, 16> Rows = autoRows(kAutoCells, kAutoTrps);
  Req.authorizedRows = Rows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness = testReadiness();
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(Req, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;

  EJitSmallTableContract Contract = buildAdmissionContract(*Plan);
  EXPECT_NE(Contract.identityHash, 0ull);
  EXPECT_EQ(Contract.entryName, "a_entry");
  EXPECT_EQ(Contract.sourceVarName, "g_auto");
  EXPECT_EQ(Contract.domainEpoch, 0x5EED20260914ull);
  EXPECT_EQ(Contract.readinessProvider, "test.provider.compiler-boundary");
  EXPECT_TRUE(Contract.coverageAsserted);
  EXPECT_TRUE(Contract.borrowedStable);
  EXPECT_TRUE(Contract.domainComplete);
  EXPECT_EQ(Contract.declaredRows, kAutoCells * kAutoTrps);
  EXPECT_EQ(Contract.provenRows, kAutoCells * kAutoTrps);
  ASSERT_EQ(Contract.fields.size(), 4u);
  // The contract carries the resource identity of the compressed column, not a
  // bare symbol name.
  EXPECT_EQ(Contract.fields[1].retainedAxes.size(), 1u);
  EXPECT_EQ(Contract.fields[1].resource.symbolName, "__ejit_stab_a_entry_c1");
  EXPECT_EQ(Contract.fields[1].resource.rows, kAutoCells);
  EXPECT_EQ(Contract.fields[1].resource.bytes, kAutoCells * 4);
  EXPECT_EQ(Contract.fields[1].publishedValues.size(), kAutoCells);
  EXPECT_EQ(Contract.fields[1].resource.fixedAddress,
            Triple(M->getTargetTriple()).isAArch64())
      << "AArch64 columns bind directly; x86-64 columns stay preemptible";
  ASSERT_TRUE(Contract.fields[0].requiredValue.has_value());
  EXPECT_EQ(*Contract.fields[0].requiredValue, 1u);

  // A member read out of its own storage through the contract's offsets/widths
  // and compared against the existing obligations. The cold path passes the
  // registered region base and the member coordinate, exactly like the planner
  // did when it copied the values.
  std::string Why;
  auto Live = readAdmissionMember(
      Contract,
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_auto[0][0]),
                           sizeof(g_auto)},
      {2, 1}, Why);
  ASSERT_TRUE(Live.has_value()) << Why;
  EXPECT_EQ(Live->bits[1], static_cast<uint32_t>(g_auto[2][1].byCell));
  EXPECT_EQ(validateAdmission(Contract, *Live, &Why),
            EJitSmallTableAdmission::Compatible)
      << Why;

  // A region that cannot hold the member's field access is refused before any
  // value is read, so a cold path can never dereference past its borrow.
  Why.clear();
  EXPECT_FALSE(readAdmissionMember(
                   Contract,
                   EJitSmallTableSource{
                       reinterpret_cast<const uint8_t *>(&g_auto[2][1]),
                       sizeof(AutoElement)},
                   {2, 1}, Why)
                   .has_value());
  EXPECT_NE(Why.find("member region"), std::string::npos) << Why;

  // A conflicting uniform constant must never reuse the specialized code.
  AutoElement Changed = g_auto[2][1];
  Changed.mode = 2;
  Why.clear();
  auto BadMode = autoMember(Changed, {2, 1});
  EXPECT_EQ(validateAdmission(Contract, BadMode, &Why),
            EJitSmallTableAdmission::Conflict)
      << Why;
  EXPECT_NE(Why.find("uniform constant"), std::string::npos) << Why;

  // A conflicting value in an already published projected coordinate is a
  // conflict even though a full-dimensional table would accept a new row: the
  // compressed projection is shared (spec §6.6).
  AutoElement BadCell = g_auto[2][1];
  BadCell.byCell = 4242;
  Why.clear();
  EXPECT_EQ(validateAdmission(Contract, autoMember(BadCell, {2, 1}), &Why),
            EJitSmallTableAdmission::Conflict)
      << Why;
  EXPECT_NE(Why.find("projected value"), std::string::npos) << Why;

  // A member outside the declared schema/capacity cannot be validated at all.
  Why.clear();
  EXPECT_EQ(validateAdmission(Contract, autoMember(g_auto[0][0], {9, 0}), &Why),
            EJitSmallTableAdmission::Unusable)
      << Why;
  // A member whose value count does not match the contract cannot be validated.
  Why.clear();
  EJitSmallTableMember Short = autoMember(g_auto[0][0], {0, 0});
  Short.bits.pop_back();
  EXPECT_EQ(validateAdmission(Contract, Short, &Why),
            EJitSmallTableAdmission::Unusable)
      << Why;

  // An unpublished but supported projected coordinate is extendable: only the
  // runtime may add it (B1), and only with the same contract values.
  SmallVector<EJitSmallTableRowKey, 4> PartialRows = {{{0, 0}}, {{1, 0}}};
  Error.clear();
  auto Partial = EJitSmallTablePlanner::plan(
      *M, "a_entry", "g_auto", autoDims(kAutoCells, kAutoTrps),
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_auto[0][0]),
                           sizeof(g_auto)},
      PartialRows, {}, Error);
  ASSERT_TRUE(Partial.has_value()) << Error;
  EJitSmallTableContract PartialContract = buildAdmissionContract(*Partial);
  EXPECT_FALSE(PartialContract.domainComplete);
  EXPECT_NE(PartialContract.identityHash, Contract.identityHash);
  Why.clear();
  EXPECT_EQ(validateAdmission(PartialContract, autoMember(g_auto[2][0], {2, 0}),
                              &Why),
            EJitSmallTableAdmission::Extendable)
      << Why;
  // ...but its unrelated invariants must still hold during that classification.
  AutoElement BadPartial = g_auto[2][0];
  BadPartial.mode = 7;
  Why.clear();
  EXPECT_EQ(validateAdmission(PartialContract,
                              autoMember(BadPartial, {2, 0}), &Why),
            EJitSmallTableAdmission::Conflict)
      << Why;
  EXPECT_STREQ(admissionName(EJitSmallTableAdmission::Extendable), "extendable");
}

/// §6.6.1 step 3 at the A1 boundary: when a conflicting member appears, only the
/// field that actually differs recovers an axis; the unrelated invariants stay
/// constants. The new domain is a new plan/identity, never an edit of the old
/// contract (production coalesced rebuild and migration are B).
TEST_F(SmallTableTest, WideningForAConflictingMemberOnlyExpandsThatField) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("a_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "auto module failed to parse";
  // On cell 0/1 the cell field is constant; it changes at cell 2.
  g_auto[0][0].byCell = 7;
  g_auto[1][0].byCell = 7;
  const auto *Base = reinterpret_cast<const uint8_t *>(&g_auto[0][0]);

  auto PlanFor = [&](ArrayRef<EJitSmallTableRowKey> Rows,
                     std::string &Error) {
    return EJitSmallTablePlanner::plan(
        *M, "a_entry", "g_auto", autoDims(kAutoCells, kAutoTrps),
        EJitSmallTableSource{Base, sizeof(g_auto)}, Rows, {}, Error);
  };

  SmallVector<EJitSmallTableRowKey, 4> First = {{{0, 0}}, {{1, 0}}};
  std::string Error;
  auto Before = PlanFor(First, Error);
  ASSERT_TRUE(Before.has_value()) << Error;
  EXPECT_EQ(Before->fields[1].strategy, EJitSmallTableStrategy::Uniform);
  ASSERT_TRUE(Before->fields[1].uniformValue.has_value());
  EXPECT_EQ(*Before->fields[1].uniformValue, 7u);
  EXPECT_EQ(Before->fields[0].strategy, EJitSmallTableStrategy::Uniform);
  EXPECT_EQ(Before->fields[2].strategy, EJitSmallTableStrategy::Uniform);
  EJitSmallTableContract BeforeContract = buildAdmissionContract(*Before);

  // The new member conflicts with the old contract's cell field: it must stay
  // AOT rather than run the specialized code.
  std::string Why;
  EXPECT_EQ(validateAdmission(BeforeContract, autoMember(g_auto[2][0], {2, 0}),
                              &Why),
            EJitSmallTableAdmission::Conflict)
      << Why;

  // A new optimization round over the widened domain: only the cell field
  // recovers an axis; the other constants stay folded and the joint table keeps
  // its own axes.
  SmallVector<EJitSmallTableRowKey, 4> Widened = {{{0, 0}}, {{1, 0}}, {{2, 0}}};
  Error.clear();
  auto After = PlanFor(Widened, Error);
  ASSERT_TRUE(After.has_value()) << Error;
  EXPECT_EQ(After->fields[1].strategy, EJitSmallTableStrategy::Table);
  ASSERT_EQ(After->fields[1].retainedAxes.size(), 1u);
  EXPECT_EQ(After->fields[1].retainedAxes[0], 0u);
  EXPECT_EQ(After->fields[0].strategy, EJitSmallTableStrategy::Uniform)
      << "an unrelated invariant must stay a constant";
  EXPECT_EQ(After->fields[2].strategy, EJitSmallTableStrategy::Uniform)
      << "an unrelated invariant must stay a constant";
  EXPECT_FALSE(After->allRowsReady());

  EJitSmallTableContract AfterContract = buildAdmissionContract(*After);
  EXPECT_NE(AfterContract.identityHash, BeforeContract.identityHash)
      << "a new domain is a new contract identity, not an edit of the old one";
  Why.clear();
  EXPECT_EQ(validateAdmission(AfterContract, autoMember(g_auto[2][0], {2, 0}),
                              &Why),
            EJitSmallTableAdmission::Compatible)
      << Why;
  // The old contract is unchanged: its recorded constant is still 7.
  ASSERT_TRUE(BeforeContract.fields[1].requiredValue.has_value());
  EXPECT_EQ(*BeforeContract.fields[1].requiredValue, 7u);
}

/// A column symbol name is not shared storage: two compiles with the same
/// spelling but different obligations must produce different contract
/// identities, and a contract with no recorded identity validates nothing.
TEST_F(SmallTableTest, ContractIdentityIsPerCompileNotPerSymbolName) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("a_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "auto module failed to parse";
  const auto *Base = reinterpret_cast<const uint8_t *>(&g_auto[0][0]);

  auto PlanFor = [&](ArrayRef<EJitSmallTableRowKey> Rows,
                     std::string &Error) {
    return EJitSmallTablePlanner::plan(
        *M, "a_entry", "g_auto", autoDims(kAutoCells, kAutoTrps),
        EJitSmallTableSource{Base, sizeof(g_auto)}, Rows, {}, Error);
  };
  SmallVector<EJitSmallTableRowKey, 16> Full = autoRows(kAutoCells, kAutoTrps);
  SmallVector<EJitSmallTableRowKey, 4> FirstTwo = {{{0, 0}}, {{1, 0}}};
  std::string Error;
  auto FullPlan = PlanFor(Full, Error);
  ASSERT_TRUE(FullPlan.has_value()) << Error;
  Error.clear();
  auto PartialPlan = PlanFor(FirstTwo, Error);
  ASSERT_TRUE(PartialPlan.has_value()) << Error;

  EJitSmallTableContract A = buildAdmissionContract(*FullPlan);
  EJitSmallTableContract B = buildAdmissionContract(*PartialPlan);
  // Same entry, same source global, same symbol spellings for the columns — a
  // different proven domain is a different admission contract.
  EXPECT_EQ(A.fields[1].resource.symbolName, B.fields[1].resource.symbolName);
  EXPECT_NE(A.identityHash, B.identityHash);
  EXPECT_NE(A.provenRows, B.provenRows);
  EXPECT_TRUE(A.domainComplete);
  EXPECT_FALSE(B.domainComplete);

  // Rebuilding the same domain yields the same identity (deterministic), so the
  // runtime can compare contracts instead of trusting a symbol name.
  Error.clear();
  auto Again = PlanFor(Full, Error);
  ASSERT_TRUE(Again.has_value()) << Error;
  EXPECT_EQ(buildAdmissionContract(*Again).identityHash, A.identityHash);

  // A contract without an identity cannot validate a member at all.
  EJitSmallTableContract Anonymous = A;
  Anonymous.identityHash = 0;
  std::string Why;
  EXPECT_EQ(validateAdmission(Anonymous, autoMember(g_auto[0][0], {0, 0}),
                              &Why),
            EJitSmallTableAdmission::Unusable)
      << Why;
}

/// §13 per-field reporting: the specialization emits one diagnostic per field
/// (original axes, retained axes, eliminated axes, constant/table strategy and
/// payload before/after) at the VERBOSE log level, and the same accounting is
/// observable through the plan API. This test raises the runtime log level for
/// the duration of one pipeline run so the report is present in the captured
/// diagnostics log; it asserts the API-visible accounting, since the log text is
/// the harness's evidence, not a test oracle.
TEST_F(SmallTableTest, AutomaticPlanReportsPerFieldAxes) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("a_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "auto module failed to parse";
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "a_entry", "g_auto", autoDims(kAutoCells, kAutoTrps),
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_auto[0][0]),
                           sizeof(g_auto)},
      autoRows(kAutoCells, kAutoTrps), {}, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;

  // The per-field report is derived from exactly this accounting.
  EXPECT_EQ(Plan->uniformFieldCount(), 1u);
  EXPECT_EQ(Plan->tableFieldCount(), 3u);
  EXPECT_EQ(Plan->fields[0].tableBytes, 0u);
  EXPECT_EQ(Plan->fields[1].tableBytes, kAutoCells * 4);
  EXPECT_EQ(Plan->fields[2].tableBytes, kAutoTrps * 4);
  EXPECT_EQ(Plan->fields[3].tableBytes, kAutoCells * kAutoTrps * 4);
  // Optimized-before payload: every field at its full declared-domain size.
  uint64_t PerRowBytes = 0;
  for (const EJitSmallTableField &F : Plan->fields)
    PerRowBytes += F.accessSize;
  EXPECT_EQ(Plan->numRows() * PerRowBytes, kAutoCells * kAutoTrps * 16);

  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(*Plan));
  PeriodArrayRegistry &Registry = makeRegistry();
  EJitOptimizer Opt(Registry);
  Opt.setSmallTablePlans(Set);
  SpecializationContext C = baselineCtx("a_entry");
  const ejit_log_level_t Saved = ejit_get_log_level();
  ejit_set_log_level(EJIT_LOG_VERBOSE);
  Opt.runPipeline(*M, C);
  ejit_set_log_level(Saved);
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("a_entry"), "g_auto"), 0u);
}

/// A controlled one-cell/one-TRP domain: the proven domain covers the declared
/// schema exactly, so every field is a genuine invariant of that closed domain
/// and folds with no column and no load at all — the §4.1 "single TRP, equal
/// across the domain" case, on a complete domain, executed through the real ORC
/// engine.
TEST_F(SmallTableTest, AutomaticSolverFoldsAClosedSingleMemberDomain) {
  SMDiagnostic Err;
  const unsigned One = 1;
  auto M = parseAssemblyString(
      autoModuleText("o_entry", "g_one", "g_one_out", One, One), Err, Ctx);
  ASSERT_TRUE(M) << "single-member module failed to parse";
  AutoElement &E = g_one[0][0];
  E.mode = 3;
  E.byCell = 11;
  E.byTrp = 13;
  E.joint = 17;
  std::memset(g_one_out, 0, sizeof(g_one_out));

  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(One, One);
  SmallVector<EJitSmallTableRowKey, 1> Rows;
  Rows.push_back({{0, 0}});
  EJitSmallTableRequest Req;
  Req.module = M.get();
  Req.entryName = "o_entry";
  Req.sourceVarName = "g_one";
  Req.dims = Dims;
  Req.source = EJitSmallTableSource{
      reinterpret_cast<const uint8_t *>(&g_one[0][0]), sizeof(g_one)};
  Req.authorizedRows = Rows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness = testReadiness();
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(Req, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  ASSERT_EQ(Plan->fields.size(), 4u);
  EXPECT_TRUE(Plan->allRowsReady())
      << "the domain covers the whole declared schema";
  EXPECT_EQ(Plan->uniformFieldCount(), 4u);
  EXPECT_EQ(Plan->tableFieldCount(), 0u);
  EXPECT_EQ(Plan->tableBytes(), 0u);
  for (const EJitSmallTableField &Field : Plan->fields) {
    EXPECT_EQ(Field.strategy, EJitSmallTableStrategy::Uniform);
    EXPECT_TRUE(Field.columnName.empty());
    EXPECT_EQ(Field.tableRows, 0u);
  }
  EXPECT_EQ(*Plan->fields[0].uniformValue, 3u);
  EXPECT_EQ(*Plan->fields[3].uniformValue, 17u);

  ASSERT_TRUE(EJitSmallTablePass::materialize(*M, *Plan, &Error)) << Error;
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_o_entry_c0"), nullptr);
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_o_entry_c1"), nullptr);
  EJitSmallTablePass Pass(*Plan);
  FunctionAnalysisManager FAM;
  Pass.run(*M->getFunction("o_entry"), FAM);
  EXPECT_EQ(Pass.getStats().uniformFolded, 4u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 0u);
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("o_entry"), "g_one"), 0u)
      << "no field keeps a load in a fully folded closed domain";

  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(*Plan));
  PeriodArrayRegistry &Registry = makeRegistry();
  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab23, "o_entry");
  ASSERT_NE(Engine, nullptr);
  EXPECT_TRUE(Engine->getLastSmallTableColumnNames().empty())
      << "a fully folded domain publishes no table";
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab23, "o_entry");
  ASSERT_NE(Fn, nullptr);
  EXPECT_EQ(Fn(0, 0, 5), -1)
      << "mode is 3, so the source's `mode == 1` branch is false; a wrongly "
         "discovered constant (for example 1) would take the arithmetic path";
  EXPECT_NE(aotAuto(E, 5), -1) << "the arithmetic path is distinguishable";
  EXPECT_EQ(g_one_out[0], 5);
}

/// Exact float bit patterns through the automatic path: a float field that is
/// bit-identical on the whole domain folds to exactly that bit pattern, so -0.0
/// stays -0.0 and a NaN payload is preserved (spec §5: floats compare and
/// construct by bits). The varying integer field still gets its own column.
TEST_F(SmallTableTest, AutomaticSolverFoldsExactFloatBits) {
  const std::string Text = moduleTargetHeader() + R"(
    %F = type { float, i32 }
    @g_f = external global [2 x [2 x %F]]
    @g_f_out = external global [2 x i32]

    define i32 @f_entry(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {
    entry:
      %row = getelementptr inbounds [2 x [2 x %F]], ptr @g_f, i64 0, i32 %cell, i32 %trp
      %p0 = getelementptr inbounds %F, ptr %row, i32 0, i32 0
      %fv = load float, ptr %p0, align 4, !ejit.may_const !1
      %p1 = getelementptr inbounds %F, ptr %row, i32 0, i32 1
      %iv = load i32, ptr %p1, align 4, !ejit.may_const !1
      %outp = getelementptr inbounds [2 x i32], ptr @g_f_out, i64 0, i32 %cell
      store i32 %x, ptr %outp, align 4
      %fb = bitcast float %fv to i32
      %s = add i32 %fb, %iv
      %xm = mul i32 %x, 7
      %res = add i32 %s, %xm
      ret i32 %res
    }

    !0 = !{!2}
    !1 = !{}
    !2 = !{!"ejit_entry"}
  )";
  SMDiagnostic Err;
  auto M = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(M) << "float module failed to parse";

  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(2, 2);
  SmallVector<EJitSmallTableRowKey, 4> Rows = autoRows(2, 2);
  auto PlanWith = [&](uint32_t FloatBits, std::string &Error) {
    for (unsigned C = 0; C < 2; ++C)
      for (unsigned T = 0; T < 2; ++T) {
        g_f[C][T].f = floatFromBits(FloatBits);
        g_f[C][T].i = static_cast<int32_t>(20 + C * 5 + T);
      }
    EJitSmallTableRequest Req;
    Req.module = M.get();
    Req.entryName = "f_entry";
    Req.sourceVarName = "g_f";
    Req.dims = Dims;
    Req.source = EJitSmallTableSource{
        reinterpret_cast<const uint8_t *>(&g_f[0][0]), sizeof(g_f)};
    Req.authorizedRows = Rows;
    Req.mode = EJitSmallTablePlanMode::Automatic;
    Req.readiness = testReadiness();
    return EJitSmallTablePlanner::plan(Req, Error);
  };

  // -0.0 is a bit pattern, not "zero": it must fold to 0x80000000 exactly.
  std::string Error;
  auto NegZero = PlanWith(0x80000000u, Error);
  ASSERT_TRUE(NegZero.has_value()) << Error;
  EXPECT_EQ(NegZero->fields[0].strategy, EJitSmallTableStrategy::Uniform);
  ASSERT_TRUE(NegZero->fields[0].uniformValue.has_value());
  EXPECT_EQ(*NegZero->fields[0].uniformValue, 0x80000000ull);
  EXPECT_EQ(NegZero->fields[0].kind, EJitSmallTableKind::Float);
  EXPECT_TRUE(NegZero->fields[0].columnName.empty());
  EXPECT_EQ(NegZero->fields[1].strategy, EJitSmallTableStrategy::Table)
      << "the integer field differs per cell and keeps its own column";

  // A NaN payload survives the fold unchanged; it is never canonicalized.
  Error.clear();
  auto Nan = PlanWith(0x7fc00001u, Error);
  ASSERT_TRUE(Nan.has_value()) << Error;
  ASSERT_TRUE(Nan->fields[0].uniformValue.has_value());
  EXPECT_EQ(*Nan->fields[0].uniformValue, 0x7fc00001ull);

  // IR + execution with the NaN domain: the folded constant carries the exact
  // bit pattern and the compiled entry reproduces the source value.
  auto JitM = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(JitM);
  ASSERT_TRUE(EJitSmallTablePass::materialize(*JitM, *Nan, &Error)) << Error;
  EJitSmallTablePass Pass(*Nan);
  FunctionAnalysisManager FAM;
  Pass.run(*JitM->getFunction("f_entry"), FAM);
  EXPECT_EQ(Pass.getStats().uniformFolded, 1u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 1u);
  EXPECT_EQ(JitM->getNamedGlobal("__ejit_stab_f_entry_c0"), nullptr);
  bool SawNanBits = false;
  for (Instruction &I : instructions(*JitM->getFunction("f_entry")))
    for (Value *Op : I.operands())
      if (auto *CF = dyn_cast<ConstantFP>(Op))
        if (CF->getValueAPF().bitcastToAPInt().getZExtValue() == 0x7fc00001ull)
          SawNanBits = true;
  EXPECT_TRUE(SawNanBits) << "the folded float constant must be the exact bits";

  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(*Nan));
  PeriodArrayRegistry &Registry = makeRegistry();
  Registry.registerArray("cell", "g_f",
                         reinterpret_cast<void *>(&g_f[0][0]), sizeof(g_f));
  Registry.registerStaticVar("g_f_out", &g_f_out[0]);
  std::memset(g_f_out, 0, sizeof(g_f_out));
  auto Engine = compileWithEngine(*JitM, Set, Registry, 0x57ab24, "f_entry");
  ASSERT_NE(Engine, nullptr);
  ASSERT_EQ(Engine->getLastSmallTableColumnNames().size(), 1u);
  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab24, "f_entry");
  ASSERT_NE(Fn, nullptr);
  for (unsigned C = 0; C < 2; ++C)
    for (unsigned T = 0; T < 2; ++T)
      for (int32_t X = 0; X < 4; ++X) {
        const int32_t Expected = static_cast<int32_t>(bitsFromFloat(g_f[C][T].f)) +
                                 g_f[C][T].i + X * 7;
        EXPECT_EQ(Fn(C, T, X), Expected) << "cell=" << C << " trp=" << T;
      }
  EXPECT_EQ(g_f_out[1], 3);
}

/// The bounded solver refuses declaratively instead of degrading: a schema with
/// more declared axes than the bounded search supports is refused with a
/// diagnostic and every load stays original (spec §4.1 "超出支持范围或预算必须
/// 显式报告，不影响安全回退").
TEST_F(SmallTableTest, AutomaticSolverRefusesBeyondItsAxisBudget) {
  const unsigned kAxes = 9; // MaxSolverAxes is 8
  std::string Args;
  std::string GEP;
  std::string Type = "i32";
  for (unsigned I = 0; I < kAxes; ++I)
    Type = "[2 x " + Type + "]";
  for (unsigned I = 0; I < kAxes; ++I) {
    Args += ", i32 %a" + Twine(I).str();
    GEP += ", i32 %a" + Twine(I).str();
  }
  std::string Text = moduleTargetHeader() + "\n    @g_nine = external global " +
                     Type + "\n\n    define i32 @n_entry(" + Args.substr(2) +
                     ", i32 %x) !ejit.metadata !0 {\n    entry:\n"
                     "      %p = getelementptr inbounds " + Type +
                     ", ptr @g_nine, i64 0" + GEP + "\n"
                     "      %v = load i32, ptr %p, align 4, !ejit.may_const !1\n"
                     "      %r = add i32 %v, %x\n      ret i32 %r\n    }\n\n"
                     "    !0 = !{!2}\n    !1 = !{}\n"
                     "    !2 = !{!\"ejit_entry\"}\n";
  SMDiagnostic Err;
  auto M = parseAssemblyString(Text, Err, Ctx);
  ASSERT_TRUE(M) << "nine-axis module failed to parse";

  static int32_t Nine[2][2][2][2][2][2][2][2][2];
  std::memset(Nine, 0, sizeof(Nine));
  SmallVector<EJitSmallTableDim, 9> Dims;
  SmallVector<EJitSmallTableRowKey, 512> Rows;
  for (unsigned I = 0; I < kAxes; ++I)
    Dims.push_back({EJitSmallTableDim::Kind::Argument, I, 0, 2});
  for (unsigned I = 0; I < 512; ++I) {
    EJitSmallTableRowKey Key;
    for (unsigned B = 0; B < kAxes; ++B)
      Key.indices.push_back((I >> (kAxes - 1 - B)) & 1u);
    Rows.push_back(Key);
  }
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "n_entry", "g_nine", Dims,
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&Nine[0][0][0][0][0]
                                                                    [0][0][0][0]),
                           sizeof(Nine)},
      Rows, {}, Error);
  EXPECT_FALSE(Plan.has_value());
  EXPECT_NE(Error.find("bounded solver"), std::string::npos) << Error;

  // Safety fallback: nothing was created and no load changed.
  EXPECT_EQ(M->getNamedGlobal("__ejit_stab_n_entry_c0"), nullptr);
  EXPECT_EQ(countLoadsRootedAt(*M->getFunction("n_entry"), "g_nine"), 1u);
}

//===----------------------------------------------------------------------===//
// Target byte order, independent of the machine running these tests.
// Literal source/column bytes prevent a host-native memcpy or a reversed
// numeric expectation from accidentally making either target order pass.
//===----------------------------------------------------------------------===//

struct EndianScalarCase {
  unsigned bitWidth;
  unsigned bytes;
  uint64_t values[2];
  uint8_t little[2][8];
  uint8_t big[2][8];
};

const EndianScalarCase EndianScalars[] = {
    {8, 1, {0xd6, 0xa5}, {{0xd6}, {0xa5}}, {{0xd6}, {0xa5}}},
    {16, 2, {0xa1b2, 0xc3d4}, {{0xb2, 0xa1}, {0xd4, 0xc3}},
     {{0xa1, 0xb2}, {0xc3, 0xd4}}},
    {32, 4, {0x89abcdef, 0x10203040},
     {{0xef, 0xcd, 0xab, 0x89}, {0x40, 0x30, 0x20, 0x10}},
     {{0x89, 0xab, 0xcd, 0xef}, {0x10, 0x20, 0x30, 0x40}}},
    {64, 8, {0x0123456789abcdefULL, 0xfedcba9876543210ULL},
     {{0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01},
      {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe}},
     {{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef},
      {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}}},
    {9, 2, {0x1a5, 0x102}, {{0xa5, 0x01}, {0x02, 0x01}},
     {{0x01, 0xa5}, {0x01, 0x02}}},
    {17, 3, {0x1a2b3, 0x10002},
     {{0xb3, 0xa2, 0x01}, {0x02, 0x00, 0x01}},
     {{0x01, 0xa2, 0xb3}, {0x01, 0x00, 0x02}}},
    {33, 5, {0x1a2b3c4d5ULL, 0x102030405ULL},
     {{0xd5, 0xc4, 0xb3, 0xa2, 0x01}, {0x05, 0x04, 0x03, 0x02, 0x01}},
     {{0x01, 0xa2, 0xb3, 0xc4, 0xd5}, {0x01, 0x02, 0x03, 0x04, 0x05}}},
    {1, 1, {0, 1}, {{0}, {1}}, {{0}, {1}}},
    {7, 1, {0x65, 0x72}, {{0x65}, {0x72}}, {{0x65}, {0x72}}},
};

EJitSmallTablePlan endianResourcePlan(bool Little) {
  EJitSmallTablePlan Plan;
  Plan.entryName = "endian_resource";
  Plan.sourceVarName = "g_endian_resource";
  Plan.littleEndian = Little;
  Plan.storage = EJitSmallTableStorage::RuntimeOwned;
  Plan.runtimeRowAdmission = true;
  Plan.dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, 2});
  Plan.readiness = {17, "test.literal-byte-order.not-product", true, true};
  Plan.rows.resize(2);
  for (EJitSmallTableRow &Row : Plan.rows)
    Row.ready = true;
  for (unsigned I = 0; I < std::size(EndianScalars); ++I) {
    const EndianScalarCase &Scalar = EndianScalars[I];
    EJitSmallTableField Field;
    Field.sourceOffset = Plan.elementBytes;
    Field.accessSize = Scalar.bytes;
    Field.bitWidth = Scalar.bitWidth;
    Field.retainedAxes = {0};
    Field.tableRows = 2;
    Field.tableBytes = 2 * Scalar.bytes;
    Field.columnName = "__ejit_stab_endian_resource_c" + std::to_string(I);
    Plan.fields.push_back(std::move(Field));
    Plan.elementBytes += Scalar.bytes;
    for (unsigned Row = 0; Row < 2; ++Row)
      Plan.rows[Row].bits.push_back(Scalar.values[Row]);
  }
  Plan.sourceStrides.push_back(Plan.elementBytes);
  return Plan;
}

TEST(SmallTableTableResourceTest, PublicationUsesDeclaredTargetByteOrder) {
  using Result = EJitSmallTableTableResource::PublishResult;
  for (bool Little : {true, false}) {
    SCOPED_TRACE(Little ? "LE target" : "BE target");
    auto Plan = endianResourcePlan(Little);
    std::string Error;
    ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
    auto Resource = EJitSmallTableTableResource::create(Plan, 19, 82, Error);
    ASSERT_NE(Resource, nullptr) << Error;
    EXPECT_EQ(Resource->generation(), 19u);
    EXPECT_EQ(Resource->capacityBytes(), 82u);
    EXPECT_EQ(Resource->accounting().payloadBytes, 54u);
    EXPECT_EQ(Resource->accounting().publishedCells, 0u);
    const std::vector<uint8_t> Zero(Resource->capacityBytes(), 0);
    EXPECT_EQ(std::vector<uint8_t>(Resource->base(),
                                  Resource->base() + Resource->capacityBytes()),
              Zero);
    for (unsigned Field = 0; Field < std::size(EndianScalars); ++Field) {
      const EndianScalarCase &Scalar = EndianScalars[Field];
      auto *Column = static_cast<uint8_t *>(Resource->columnAddress(Field));
      ASSERT_NE(Column, nullptr);
      for (unsigned Row = 0; Row < 2; ++Row) {
        SCOPED_TRACE("width=" + std::to_string(Scalar.bitWidth) +
                     " row=" + std::to_string(Row));
        uint64_t Bits = UINT64_MAX;
        EXPECT_FALSE(Resource->published(Field, Row, &Bits));
        EXPECT_EQ(Bits, UINT64_MAX) << "unknown rows never fabricate a value";
        ASSERT_EQ(Resource->publish(Field, Row, Scalar.values[Row]),
                  Result::Stored);
        const uint8_t *Address = Column + Row * Scalar.bytes;
        const uint8_t *Expected = Little ? Scalar.little[Row] : Scalar.big[Row];
        const std::vector<uint8_t> ExpectedBytes(Expected, Expected + Scalar.bytes);
        EXPECT_EQ(std::vector<uint8_t>(Address, Address + Scalar.bytes),
                  ExpectedBytes);
        ASSERT_TRUE(Resource->published(Field, Row, &Bits));
        EXPECT_EQ(Bits, Scalar.values[Row]);
        EXPECT_EQ(Resource->publish(Field, Row, Scalar.values[Row]),
                  Result::AlreadySame);
        EXPECT_EQ(Resource->publish(Field, Row, Scalar.values[Row] ^ 1),
                  Result::Conflict);
        EXPECT_EQ(std::vector<uint8_t>(Address, Address + Scalar.bytes),
                  ExpectedBytes) << "a conflict cannot overwrite immutable bytes";
      }
      const std::vector<std::pair<uint64_t, uint64_t>> ExpectedValues = {
          {0, Scalar.values[0]}, {1, Scalar.values[1]}};
      EXPECT_EQ(Resource->publishedValues(Field), ExpectedValues);
      EXPECT_EQ(Resource->publish(Field, 2, 0), Result::OutOfRange);
      EXPECT_FALSE(Resource->published(Field, 2, nullptr));
    }
    EXPECT_EQ(Resource->publish(99, 0, 0), Result::NotATable);
    const auto Accounting = Resource->accounting();
    EXPECT_EQ(Accounting.reservedBytes, 82u);
    EXPECT_EQ(Accounting.allocatedBytes, 82u);
    EXPECT_EQ(Accounting.payloadBytes, 54u);
    EXPECT_EQ(Accounting.publishedBytes, 54u);
    EXPECT_EQ(Accounting.publishedCells, 18u);
  }
}

TEST(SmallTableTableResourceTest, EachResourceSnapshotsItsPlanByteOrder) {
  auto Plan = endianResourcePlan(false);
  std::string Error;
  ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
  auto Big = EJitSmallTableTableResource::create(Plan, 31, 82, Error);
  ASSERT_NE(Big, nullptr) << Error;
  Plan.littleEndian = true;
  auto Little = EJitSmallTableTableResource::create(Plan, 32, 82, Error);
  ASSERT_NE(Little, nullptr) << Error;
  // Neither resource may retain the caller's mutable plan or share the last
  // plan's byte-order setting. Mutation happens before either publication.
  Plan.littleEndian = false;
  ASSERT_EQ(Big->publish(1, 1, 0xc3d4),
            EJitSmallTableTableResource::PublishResult::Stored);
  ASSERT_EQ(Little->publish(1, 1, 0xc3d4),
            EJitSmallTableTableResource::PublishResult::Stored);
  const auto *B = static_cast<const uint8_t *>(Big->columnAddress(1));
  const auto *L = static_cast<const uint8_t *>(Little->columnAddress(1));
  ASSERT_NE(B, nullptr);
  ASSERT_NE(L, nullptr);
  EXPECT_EQ(B[2], 0xc3);
  EXPECT_EQ(B[3], 0xd4);
  EXPECT_EQ(L[2], 0xd4);
  EXPECT_EQ(L[3], 0xc3);
  EXPECT_EQ(B[0], 0u);
  EXPECT_EQ(B[1], 0u);
  EXPECT_EQ(L[0], 0u);
  EXPECT_EQ(L[1], 0u);
  EXPECT_EQ(Big->accounting().publishedCells, 1u);
  EXPECT_EQ(Little->accounting().publishedCells, 1u);
}

TEST(SmallTableTableResourceTest, UniformOnlyPlanAllocatesNoEndianPayload) {
  for (bool Little : {true, false}) {
    auto Plan = endianResourcePlan(Little);
    for (unsigned I = 0; I < Plan.fields.size(); ++I) {
      auto &Field = Plan.fields[I];
      Field.strategy = EJitSmallTableStrategy::Uniform;
      Field.uniformValue = Plan.rows[0].bits[I];
      Field.retainedAxes.clear();
      Field.tableRows = Field.tableBytes = 0;
      Field.columnName.clear();
      Plan.rows[1].bits[I] = Plan.rows[0].bits[I];
    }
    std::string Error;
    ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
    auto Resource = EJitSmallTableTableResource::create(Plan, 41, 0, Error);
    ASSERT_NE(Resource, nullptr) << Error;
    EXPECT_EQ(Resource->base(), nullptr);
    EXPECT_TRUE(Resource->columns().empty());
    EXPECT_EQ(Resource->capacityBytes(), 0u);
    EXPECT_EQ(Resource->accounting().payloadBytes, 0u);
    EXPECT_EQ(Resource->accounting().publishedBytes, 0u);
    EXPECT_EQ(Resource->accounting().publishedCells, 0u);
    EXPECT_EQ(Resource->publish(0, 0, Plan.rows[0].bits[0]),
              EJitSmallTableTableResource::PublishResult::NotATable);
  }
}

TEST(SmallTableTableResourceTest, PlanRejectsUnsupportedIntegerByteShapes) {
  for (bool Little : {true, false}) {
    const auto Good = endianResourcePlan(Little);
    for (const auto &BadShape :
         {std::pair<unsigned, unsigned>{0, 1}, {65, 8}, {9, 1}, {16, 3}}) {
      auto Bad = Good;
      Bad.fields[0].bitWidth = BadShape.first;
      Bad.fields[0].accessSize = BadShape.second;
      std::string Why;
      EXPECT_FALSE(Bad.isConsistent(&Why));
      EXPECT_FALSE(Why.empty());
    }
  }
}

// Copy literal target bytes, deliberately setting integer storage padding to
// prove width masking. No host-native integers enter the source fixture.
void fillEndianSource(uint8_t *Source, const EJitSmallTablePlan &Plan) {
  for (unsigned Row = 0; Row < 2; ++Row)
    for (unsigned Field = 0; Field < std::size(EndianScalars); ++Field) {
      const auto &Scalar = EndianScalars[Field];
      auto *Address = Source + Row * Plan.sourceStrides[0] +
                      Plan.fields[Field].sourceOffset;
      const uint8_t *Bytes = Plan.littleEndian ? Scalar.little[Row] : Scalar.big[Row];
      std::memcpy(Address, Bytes, Scalar.bytes);
      if (Scalar.bitWidth % 8 != 0) {
        const unsigned HighByte = Plan.littleEndian ? Scalar.bytes - 1 : 0;
        Address[HighByte] |= static_cast<uint8_t>(0xff << (Scalar.bitWidth % 8));
      }
    }
}

TEST(SmallTableEndianReadTest, AdmissionReadsLiteralBytesInContractOrder) {
  for (bool Little : {true, false}) {
    auto Plan = endianResourcePlan(Little);
    // These bit patterns are compared unchanged, not converted through a host
    // float/double value; the scalar-kind path must retain their exact bits.
    Plan.fields[2].kind = EJitSmallTableKind::Float;
    Plan.fields[3].kind = EJitSmallTableKind::Double;
    std::string Error;
    ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
    const auto Contract = buildAdmissionContract(Plan);
    EXPECT_EQ(Contract.littleEndian, Little);
    std::vector<uint8_t> Source(2 * Plan.elementBytes);
    fillEndianSource(Source.data(), Plan);
    for (unsigned Row = 0; Row < 2; ++Row) {
      SCOPED_TRACE(std::string(Little ? "LE" : "BE") +
                   " admission row=" + std::to_string(Row));
      auto Member = readAdmissionMember(
          Contract, {Source.data(), Source.size()}, {Row}, Error);
      ASSERT_TRUE(Member.has_value()) << Error;
      EXPECT_EQ(ArrayRef<uint64_t>(Member->bits),
                ArrayRef<uint64_t>(Plan.rows[Row].bits));
      EXPECT_EQ(validateAdmission(Contract, *Member, &Error),
                EJitSmallTableAdmission::Compatible) << Error;
    }
  }
}

TEST(SmallTableEndianReadTest, PlannerReadsLiteralBytesInModuleOrder) {
  for (bool Little : {true, false}) {
    LLVMContext Context;
    std::string Text = "target datalayout = \"";
    Text += Little ? "e" : "E";
    Text += "-p:64:64-i64:64-n32:64-S128\"\n";
    Text += "target triple = \"";
    Text += Little ? "aarch64-none-elf" : "aarch64_be-none-elf";
    Text += "\"\n%Row = type { ";
    for (unsigned I = 0; I < std::size(EndianScalars); ++I)
      Text += (I ? ", " : "") + std::string("[8 x i8]");
    Text += " }\n@g_ordered = external global [2 x %Row]\n";
    Text += "define i32 @ordered(i32 %index) !ejit.metadata !0 {\n";
    Text += "%row = getelementptr inbounds [2 x %Row], ptr @g_ordered, "
            "i64 0, i32 %index\n";
    for (unsigned I = 0; I < std::size(EndianScalars); ++I) {
      const std::string Index = std::to_string(I);
      Text += "%p" + Index + " = getelementptr inbounds %Row, ptr %row, "
              "i32 0, i32 " + Index + "\n";
      const std::string Type = I == 2 ? "float" : I == 3 ? "double" :
          "i" + std::to_string(EndianScalars[I].bitWidth);
      Text += "%v" + Index + " = load " + Type + ", ptr %p" + Index +
              ", align 1, !ejit.may_const !1\n";
    }
    Text += "ret i32 0\n}\n!0 = !{!2}\n!1 = !{}\n"
            "!2 = !{!\"ejit_entry\"}\n";
    SMDiagnostic Diagnostic;
    auto Module = parseAssemblyString(Text, Diagnostic, Context);
    ASSERT_NE(Module, nullptr) << Text;
    auto Expected = endianResourcePlan(Little);
    Expected.elementBytes = 8 * std::size(EndianScalars);
    Expected.sourceStrides[0] = Expected.elementBytes;
    for (unsigned I = 0; I < Expected.fields.size(); ++I)
      Expected.fields[I].sourceOffset = 8 * I;
    std::vector<uint8_t> Source(2 * Expected.elementBytes);
    fillEndianSource(Source.data(), Expected);
    const SmallVector<EJitSmallTableDim, 1> Dims = {
        {EJitSmallTableDim::Kind::Argument, 0, 0, 2}};
    const SmallVector<EJitSmallTableRowKey, 2> Rows = {{{0}}, {{1}}};
    EJitSmallTableRequest Request;
    Request.module = Module.get();
    Request.entryName = "ordered";
    Request.sourceVarName = "g_ordered";
    Request.dims = Dims;
    Request.source = {Source.data(), Source.size()};
    Request.authorizedRows = Rows;
    Request.readiness = testReadiness();
    std::string Error;
    auto Actual = EJitSmallTablePlanner::plan(Request, Error);
    ASSERT_TRUE(Actual.has_value()) << Error;
    ASSERT_EQ(Actual->fields.size(), std::size(EndianScalars));
    EXPECT_EQ(Actual->littleEndian, Little);
    EXPECT_EQ(Actual->elementBytes, Expected.elementBytes);
    EXPECT_EQ(Actual->fields[2].kind, EJitSmallTableKind::Float);
    EXPECT_EQ(Actual->fields[3].kind, EJitSmallTableKind::Double);
    for (unsigned Row = 0; Row < 2; ++Row)
      EXPECT_EQ(ArrayRef<uint64_t>(Actual->rows[Row].bits),
                ArrayRef<uint64_t>(Expected.rows[Row].bits))
          << (Little ? "LE" : "BE") << " planner row=" << Row;
    const auto Contract = buildAdmissionContract(*Actual);
    for (unsigned Row = 0; Row < 2; ++Row) {
      auto Member = readAdmissionMember(
          Contract, {Source.data(), Source.size()}, {Row}, Error);
      ASSERT_TRUE(Member.has_value()) << Error;
      EXPECT_EQ(ArrayRef<uint64_t>(Member->bits),
                ArrayRef<uint64_t>(Expected.rows[Row].bits));
      EXPECT_EQ(validateAdmission(Contract, *Member, &Error),
                EJitSmallTableAdmission::Compatible) << Error;
    }
  }
}

std::string printEndianTestModule(const Module &M) {
  std::string Text;
  raw_string_ostream Stream(Text);
  M.print(Stream, nullptr);
  return Text;
}

TEST_F(SmallTableTest, MaterializationRefusesMismatchedPlanByteOrderWithoutEdits) {
  for (bool ModuleLittle : {true, false}) {
    SMDiagnostic Diagnostic;
    auto M = parseAssemblyString(widthModuleText(), Diagnostic, Ctx);
    ASSERT_NE(M, nullptr);
    auto Set = makeWidthPlanSet(*M);
    ASSERT_NE(Set->find("w_entry"), nullptr);
    auto Plan = *Set->find("w_entry");
    std::string Layout = M->getDataLayoutStr();
    ASSERT_FALSE(Layout.empty());
    Layout[0] = ModuleLittle ? 'e' : 'E';
    M->setDataLayout(Layout);
    Plan.littleEndian = !ModuleLittle;
    std::string Error;
    ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
    const std::string Before = printEndianTestModule(*M);
    EXPECT_FALSE(EJitSmallTablePass::materialize(*M, Plan, &Error));
    EXPECT_EQ(Error,
              "small-table plan byte order does not match module data layout");
    EXPECT_EQ(printEndianTestModule(*M), Before)
        << "refusal precedes any global insertion or load replacement";
  }
}

TEST_F(SmallTableTest, DirectPassRefusesMismatchedByteOrderWithExistingColumns) {
  for (bool ModuleLittle : {true, false}) {
    SMDiagnostic Diagnostic;
    auto M = parseAssemblyString(widthModuleText(), Diagnostic, Ctx);
    ASSERT_NE(M, nullptr);
    auto Set = makeWidthPlanSet(*M);
    ASSERT_NE(Set->find("w_entry"), nullptr);
    auto Plan = *Set->find("w_entry");
    // The first may_const i1 field is uniform; the other three remain tables.
    // A caller that bypasses materialize must neither fold this constant nor
    // reuse existing columns against a differently ordered plan.
    auto &Uniform = Plan.fields[0];
    Uniform.strategy = EJitSmallTableStrategy::Uniform;
    Uniform.uniformValue = Plan.rows[0].bits[0];
    Uniform.retainedAxes.clear();
    Uniform.columnName.clear();
    Uniform.tableRows = Uniform.tableBytes = 0;
    std::string Layout = M->getDataLayoutStr();
    ASSERT_FALSE(Layout.empty());
    Layout[0] = ModuleLittle ? 'e' : 'E';
    M->setDataLayout(Layout);
    Plan.littleEndian = ModuleLittle;
    std::string Error;
    ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
    ASSERT_TRUE(EJitSmallTablePass::materialize(*M, Plan, &Error)) << Error;
    for (unsigned I = 1; I < Plan.fields.size(); ++I)
      ASSERT_NE(M->getNamedGlobal(Plan.fields[I].columnName), nullptr);
    Plan.littleEndian = !ModuleLittle;
    const std::string Before = printEndianTestModule(*M);
    EJitSmallTablePass Pass(Plan);
    FunctionAnalysisManager Analyses;
    const auto Preserved = Pass.run(*M->getFunction("w_entry"), Analyses);
    EXPECT_TRUE(Preserved.areAllPreserved());
    EXPECT_EQ(Pass.getStats().uniformFolded, 0u);
    EXPECT_EQ(Pass.getStats().tableReplaced, 0u);
    EXPECT_EQ(printEndianTestModule(*M), Before);
    EXPECT_EQ(countTableLoads(*M->getFunction("w_entry"),
                             EJitSmallTablePlan::TableGlobalPrefix), 0u);
  }
}

//===----------------------------------------------------------------------===//
// B0/B1/B2: the online runtime over the real planner, pass, ORC engine and real
// executions.
//
// The readiness facts come from `EJitSmallTableHostProvider`, the explicitly
// labeled HOST adapter ("host-adapter.pr231-not-product"): the product
// configuration transaction is not available in this local checkout, so every
// artifact records that label and nothing here is claimed as product readiness.
// What IS real: the borrow discipline, the shared resource with generation
// identity, row publication, contract validation, the common instrumented T1,
// the aggregate sampling session, the frozen bundle with real counter data and
// the common T2 execution.
//===----------------------------------------------------------------------===//

class SmallTableRuntimeTest : public SmallTableTest {
protected:
  std::shared_ptr<EJitSmallTableHostProvider>
  hostProvider(uint64_t Epoch, unsigned Cells = kAutoCells,
               unsigned Trps = kAutoTrps) {
    auto P = std::make_shared<EJitSmallTableHostProvider>(
        "g_auto", reinterpret_cast<const void *>(&g_auto[0][0]), sizeof(g_auto),
        Epoch);
    for (unsigned C = 0; C < Cells; ++C)
      for (unsigned T = 0; T < Trps; ++T)
        P->addReadyMember({C, T}, 0xA000 + C * 16 + T);
    return P;
  }

  std::unique_ptr<EJitSmallTableRuntime>
  makeRuntime(std::shared_ptr<EJitSmallTableReadinessProvider> Provider,
              EJitSmallTableRuntime::Options Opts = {}) {
    Config Cfg;
    auto R = EJitSmallTableRuntime::create(Cfg, makeRegistry(), State,
                                           std::move(Provider), Opts);
    if (!R) {
      ADD_FAILURE() << "runtime create failed: " << toString(R.takeError());
      return nullptr;
    }
    return std::move(*R);
  }

  std::unique_ptr<Module> parseAutoEntry(StringRef Entry = "a_entry",
                                         StringRef Global = "g_auto",
                                         StringRef Out = "g_auto_out") {
    SMDiagnostic Err;
    auto M = parseAssemblyString(
        autoModuleText(Entry, Global, Out, kAutoCells, kAutoTrps), Err, Ctx);
    if (!M)
      Err.print("SmallTableRuntimeTest", errs());
    return M;
  }

  /// Host stand-in for the profile-runtime HOOK symbol the freestanding product
  /// image provides (`__llvm_profile_runtime`). InstrProfilingLowering declares
  /// it (and synthesizes a user function for it) when it instruments a module; a
  /// declaration created after `addIRModule` is resolved through the engine's
  /// registered-symbol table, so the runtime registers the stand-in there. It
  /// carries no counter state: the counters are the real transform-generated
  /// `__profc_` globals this suite reads back. Harness support only, recorded as
  /// such in the delivery. The profile version flag needs no stand-in: the
  /// engine claims that transform-created definition.
  void addProfileRuntimeHook(EJitSmallTableRuntime &RT) {
    static uint32_t RuntimeHook = 0;
    RT.engine().addUserSymbol("__llvm_profile_runtime",
                              reinterpret_cast<void *>(&RuntimeHook));
  }
};

// A real nonuniform i64 trailer keeps pre-fix last i33 typed reads inside the
// actual allocation: deterministic incorrect execution, not an out-of-bounds
// read whose undefined behavior could make the regression spuriously pass.
const uint64_t PackedOddValues[3][3] = {
    {0x1a2b3, 0x1a2b3c4d5ULL, 0x8877665544332211ULL},
    {0x10002, 0x102030405ULL, 0x1021324354657687ULL},
    {0x1c4d5, 0x1e6f70819ULL, 0xf0e1d2c3b4a59687ULL}};
const uint32_t PackedOddLive[3] = {8, 17, 29};
const uint8_t PackedOddLittle[3][3][8] = {
    {{0xb3, 0xa2, 0x01}, {0xd5, 0xc4, 0xb3, 0xa2, 0x01},
     {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}},
    {{0x02, 0x00, 0x01}, {0x05, 0x04, 0x03, 0x02, 0x01},
     {0x87, 0x76, 0x65, 0x54, 0x43, 0x32, 0x21, 0x10}},
    {{0xd5, 0xc4, 0x01}, {0x19, 0x08, 0xf7, 0xe6, 0x01},
     {0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0}}};
const uint8_t PackedOddBig[3][3][8] = {
    {{0x01, 0xa2, 0xb3}, {0x01, 0xa2, 0xb3, 0xc4, 0xd5},
     {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}},
    {{0x01, 0x00, 0x02}, {0x01, 0x02, 0x03, 0x04, 0x05},
     {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87}},
    {{0x01, 0xc4, 0xd5}, {0x01, 0xe6, 0xf7, 0x08, 0x19},
     {0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87}}};

std::string packedOddModuleText(StringRef Header, ArrayRef<unsigned> Widths,
                                bool HasLive) {
  std::string Text = Header.str() + "\n%PackedRow = type { ";
  for (unsigned I = 0; I < Widths.size() + unsigned(HasLive); ++I)
    Text += (I ? ", " : "") + std::string("[8 x i8]");
  Text += " }\n@g_packed_odd = external global [3 x %PackedRow]\n";
  Text += "define i64 @packed_odd_entry(i32 %index) !ejit.metadata !0 {\n";
  Text += "%row = getelementptr inbounds [3 x %PackedRow], ptr @g_packed_odd, "
          "i64 0, i32 %index\n";
  for (unsigned I = 0; I < Widths.size(); ++I) {
    const std::string Id = std::to_string(I);
    Text += "%p" + Id + " = getelementptr inbounds %PackedRow, ptr %row, "
            "i32 0, i32 " + Id + "\n";
    Text += "%v" + Id + " = load i" + std::to_string(Widths[I]) +
            ", ptr %p" + Id + ", align 1, !ejit.may_const !1\n";
    if (Widths[I] < 64)
      Text += "%w" + Id + " = zext i" + std::to_string(Widths[I]) +
              " %v" + Id + " to i64\n";
    const std::string Value = (Widths[I] == 64 ? "%v" : "%w") + Id;
    if (I == 0)
      Text += "%mix0 = xor i64 " + Value + ", 0\n";
    else
      Text += "%mix" + Id + " = xor i64 %mix" + std::to_string(I - 1) +
              ", " + Value + "\n";
  }
  const std::string Mix = "%mix" + std::to_string(Widths.size() - 1);
  if (HasLive) {
    Text += "%livep = getelementptr inbounds %PackedRow, ptr %row, i32 0, i32 " +
            std::to_string(Widths.size()) + "\n";
    Text += "%live = load i32, ptr %livep, align 1\n"
            "%live64 = zext i32 %live to i64\n"
            "%low = and i32 %live, 1\n%even = icmp eq i32 %low, 0\n"
            "br i1 %even, label %positive, label %negative\n"
            "positive:\n%plus = add i64 " + Mix + ", %live64\nret i64 %plus\n"
            "negative:\n%minus = sub i64 " + Mix +
            ", %live64\nret i64 %minus\n";
  } else {
    Text += "ret i64 " + Mix + "\n";
  }
  Text += "}\n!0 = !{!2}\n!1 = !{}\n!2 = !{!\"ejit_entry\"}\n";
  return Text;
}

void fillPackedOddSource(uint8_t (&Source)[3][32], bool Little) {
  std::memset(Source, 0x5a, sizeof(Source));
  const unsigned Sizes[] = {3, 5, 8};
  for (unsigned Row = 0; Row < 3; ++Row) {
    for (unsigned Field = 0; Field < 3; ++Field) {
      const uint8_t *Bytes = Little ? PackedOddLittle[Row][Field]
                                    : PackedOddBig[Row][Field];
      std::memcpy(Source[Row] + 8 * Field, Bytes, Sizes[Field]);
      if (Field < 2)
        Source[Row][8 * Field + (Little ? Sizes[Field] - 1 : 0)] |= 0xfe;
    }
    std::memset(Source[Row] + 24, 0, 4);
    Source[Row][Little ? 24 : 27] = static_cast<uint8_t>(PackedOddLive[Row]);
  }
}

uint64_t packedOddAot(unsigned Row) {
  const uint64_t Mix = PackedOddValues[Row][0] ^ PackedOddValues[Row][1] ^
                       PackedOddValues[Row][2];
  return PackedOddLive[Row] & 1 ? Mix - PackedOddLive[Row]
                               : Mix + PackedOddLive[Row];
}

TEST_F(SmallTableTest, CompilerEmittedOddWidthsExecuteSecondAndLastRows) {
  SMDiagnostic Diagnostic;
  auto M = parseAssemblyString(
      packedOddModuleText(moduleTargetHeader(), {17, 33, 64}, true),
      Diagnostic, Ctx);
  ASSERT_NE(M, nullptr);
  alignas(8) uint8_t Source[3][32];
  fillPackedOddSource(Source, M->getDataLayout().isLittleEndian());
  const SmallVector<EJitSmallTableDim, 1> Dims = {
      {EJitSmallTableDim::Kind::Argument, 0, 0, 3}};
  const SmallVector<EJitSmallTableRowKey, 3> Rows = {{{0}}, {{1}}, {{2}}};
  EJitSmallTableRequest Request;
  Request.module = M.get();
  Request.entryName = "packed_odd_entry";
  Request.sourceVarName = "g_packed_odd";
  Request.dims = Dims;
  Request.source = {&Source[0][0], sizeof(Source)};
  Request.authorizedRows = Rows;
  Request.readiness = testReadiness();
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(Request, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  EXPECT_EQ(Plan->storage, EJitSmallTableStorage::CompilerEmitted);
  ASSERT_EQ(Plan->fields.size(), 3u);
  for (unsigned Field = 0; Field < 3; ++Field) {
    EXPECT_EQ(Plan->fields[Field].strategy, EJitSmallTableStrategy::Table);
    for (unsigned Row = 0; Row < 3; ++Row)
      EXPECT_EQ(Plan->rows[Row].bits[Field], PackedOddValues[Row][Field]);
  }
  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(*Plan));
  auto &Registry = makeRegistry();
  Registry.registerArray("row", "g_packed_odd", &Source[0][0], sizeof(Source));
  auto Engine = compileWithEngine(*M, Set, Registry, 0x571733,
                                  "packed_odd_entry");
  ASSERT_NE(Engine, nullptr);
  auto Fn = lookupSealed<uint64_t (*)(uint32_t)>(
      *Engine, 0x571733, "packed_odd_entry");
  ASSERT_NE(Fn, nullptr);
  for (unsigned Row = 0; Row < 3; ++Row)
    EXPECT_EQ(Fn(Row), packedOddAot(Row)) << "compiler-emitted row=" << Row;
}

TEST_F(SmallTableRuntimeTest, RuntimeOwnedOddWidthsRunCommonT1AndCompleteT2) {
  struct RegistryScope {
    RegistryScope() {
      EJitFuncRegistry::instance().reset();
      EJitLifecycleRegistry::instance().reset();
    }
    ~RegistryScope() {
      EJitFuncRegistry::instance().reset();
      EJitLifecycleRegistry::instance().reset();
    }
  } Registries;
  SMDiagnostic Diagnostic;
  auto M = parseAssemblyString(
      packedOddModuleText(moduleTargetHeader(), {17, 33, 64}, true),
      Diagnostic, Ctx);
  ASSERT_NE(M, nullptr);
  alignas(8) uint8_t Source[3][32];
  fillPackedOddSource(Source, M->getDataLayout().isLittleEndian());
  auto &Registry = makeRegistry();
  Registry.registerArray("row", "g_packed_odd", &Source[0][0], sizeof(Source));
  auto Provider = std::make_shared<EJitSmallTableHostFactSource>(
      "g_packed_odd", Source, sizeof(Source), 0xe1733);
  for (unsigned Row = 0; Row < 3; ++Row)
    Provider->addReadyMember({Row}, Row + 1);
  EJitSmallTableRuntime::Options Options;
  Options.sampling.aggregateLimit = 8;
  auto Runtime = makeRuntime(Provider, Options);
  ASSERT_NE(Runtime, nullptr);
  EJitSmallTableHost::Options HostOptions;
  HostOptions.runtime = Options;
  auto HostOrError = EJitSmallTableHost::create(*Runtime, Provider, HostOptions);
  ASSERT_TRUE(static_cast<bool>(HostOrError)) << toString(HostOrError.takeError());
  auto Host = std::move(*HostOrError);
  const uint32_t DimType =
      EJitLifecycleRegistry::instance().resolveAssign("row");
  ASSERT_NE(DimType, kEJitInvalidDimType);
  const SmallVector<EJitSmallTableDim, 1> Dims = {
      {EJitSmallTableDim::Kind::Argument, 0, 0, 3}};
  std::string Error;
  const std::vector<std::string> Periods = {"row"};
  EJitSmallTableHost::EntryRequest Request;
  Request.module = M.get();
  Request.entryName = "packed_odd_entry";
  Request.funcIndex =
      EJitFuncRegistry::instance().resolveAssign("packed_odd_entry");
  Request.sourceVarName = "g_packed_odd";
  Request.dims = Dims;
  Request.dimPeriodNames = Periods;
  Request.codeGeneration = 0x1733;
  auto Prepared = Host->planEntry(Request, Error);
  ASSERT_TRUE(static_cast<bool>(Prepared)) << Error;
  ASSERT_EQ(Runtime->plan()->fields.size(), 3u);
  ASSERT_EQ(Runtime->resource()->columns().size(), 3u);
  EXPECT_EQ(Runtime->plan()->storage, EJitSmallTableStorage::RuntimeOwned);
  EXPECT_EQ(Runtime->plan()->fields[0].accessSize, 3u);
  EXPECT_EQ(Runtime->plan()->fields[1].accessSize, 5u);
  EXPECT_EQ(Runtime->resource()->capacityBytes(), 56u);
  for (unsigned Field = 0; Field < 3; ++Field) {
    EXPECT_EQ(Runtime->plan()->fields[Field].strategy,
              EJitSmallTableStrategy::Table);
    for (unsigned Row = 0; Row < 3; ++Row)
      EXPECT_EQ(Runtime->plan()->rows[Row].bits[Field],
                PackedOddValues[Row][Field]);
  }
  auto T1OrError = Host->compileT1(Error);
  ASSERT_TRUE(static_cast<bool>(T1OrError)) << Error;
  auto T1 = reinterpret_cast<uint64_t (*)(uint32_t)>(*T1OrError);
  ASSERT_NE(T1, nullptr);
  const auto *I33 = Runtime->resource()->columnForField(1);
  const auto *Trailer = Runtime->resource()->columnForField(2);
  ASSERT_NE(I33, nullptr);
  ASSERT_NE(Trailer, nullptr);
  EXPECT_EQ(I33->offset, 16u);
  EXPECT_EQ(Trailer->offset, 32u);
  EXPECT_LE(I33->offset + 2 * 8 + 8, Runtime->resource()->capacityBytes());
  for (unsigned Field = 0; Field < 3; ++Field) {
    auto Bound = Runtime->engine().lookup(
        0x1733, Runtime->plan()->fields[Field].columnName);
    ASSERT_TRUE(static_cast<bool>(Bound)) << toString(Bound.takeError());
    EXPECT_EQ(*Bound, Runtime->resource()->columnAddress(Field));
  }
  for (unsigned Sample = 0; Sample < 8; ++Sample) {
    const unsigned Row = Sample % 3;
    uint64_t Ticket = 0;
    void *Entered = Host->enterInstrumented({DimType}, {Row}, &Ticket, &Error);
    ASSERT_NE(Entered, nullptr) << Error;
    ASSERT_NE(Ticket, 0u);
    EXPECT_EQ(Entered, *T1OrError);
    EXPECT_EQ(Host->activeExecutions(), 1u);
    EXPECT_EQ(Runtime->physicalReaders(Runtime->resourceGeneration()), 1u);
    EXPECT_EQ(reinterpret_cast<uint64_t (*)(uint32_t)>(Entered)(Row),
              packedOddAot(Row))
        << "common T1 sample=" << Sample << " row=" << Row;
    Host->leave(Ticket);
    EXPECT_EQ(Host->activeExecutions(), 0u);
    EXPECT_EQ(Runtime->physicalReaders(Runtime->resourceGeneration()), 0u);
  }
  EXPECT_EQ(Runtime->currentSessionSamples(), 8u);
  EXPECT_EQ(Runtime->inFlight(), 0u);
  EXPECT_TRUE(Runtime->samplingExhausted());
  uint64_t OverQuota = 99;
  EXPECT_EQ(Host->enterInstrumented({DimType}, {0}, &OverQuota, &Error), nullptr);
  EXPECT_EQ(OverQuota, 0u);
  auto Frozen = Runtime->freeze(Error);
  ASSERT_TRUE(static_cast<bool>(Frozen)) << Error;
  const auto *Bundle = *Frozen;
  ASSERT_NE(Bundle, nullptr);
  EXPECT_EQ(Bundle->sampleCount, 8u);
  EXPECT_EQ(Bundle->participatingMembers, 3u);
  EXPECT_EQ(Provider->outstandingBorrows(), 0u);
  ASSERT_EQ(Bundle->counters.size(), 1u);
  const auto &Captured = Bundle->counters.front();
  ASSERT_NE(Captured.profcAddr, 0u);
  ASSERT_NE(Captured.profdAddr, 0u);
  ASSERT_NE(Captured.pgoName, nullptr);
  EXPECT_STREQ(Captured.pgoName, "packed_odd_entry");
  auto Reader = InstrProfReader::create(
      MemoryBuffer::getMemBufferCopy(Bundle->profileData));
  ASSERT_TRUE(static_cast<bool>(Reader)) << toString(Reader.takeError());
  unsigned Records = 0;
  for (const NamedInstrProfRecord &Record : **Reader) {
    ++Records;
    EXPECT_EQ(Record.Name, StringRef(Captured.pgoName));
    const auto *Data =
        reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(
            Captured.profdAddr);
    EXPECT_EQ(Record.Hash, Data->FuncHash);
    EXPECT_EQ(IndexedInstrProf::ComputeHash(Record.Name), Data->NameRef);
    ASSERT_EQ(Record.Counts.size(), Data->NumCounters);
    ASSERT_FALSE(Record.Counts.empty());
    const auto *Raw = reinterpret_cast<const uint64_t *>(Captured.profcAddr);
    uint64_t Total = 0;
    for (unsigned Counter = 0; Counter < Record.Counts.size(); ++Counter) {
      EXPECT_EQ(Record.Counts[Counter], Raw[Counter]);
      EXPECT_LE(Raw[Counter], 8u) << "no call executes outside the real quota";
      Total += Raw[Counter];
    }
    EXPECT_GE(Total, 8u) << "the profile contains real execution, not tickets";
  }
  EXPECT_EQ(Records, 1u);
  EXPECT_FALSE((*Reader)->hasError());
  auto PublishError = Host->publishGeneration(Error);
  ASSERT_FALSE(static_cast<bool>(PublishError))
      << Error << ": " << toString(std::move(PublishError));
  ASSERT_EQ(Host->publishedSlots(), 3u);
  ASSERT_NE(Host->activeEntry(), nullptr);
  for (unsigned Row = 0; Row < 3; ++Row) {
    uint64_t Ticket = 0;
    void *Entered = Host->enter({DimType}, {Row}, &Ticket, &Error);
    ASSERT_NE(Entered, nullptr) << Error;
    ASSERT_NE(Ticket, 0u);
    EXPECT_EQ(Entered, Host->activeEntry());
    EXPECT_EQ(Host->activeExecutions(), 1u);
    EXPECT_EQ(Runtime->physicalReaders(Runtime->resourceGeneration()), 1u);
    EXPECT_EQ(Provider->outstandingBorrows(), 1u);
    EXPECT_EQ(reinterpret_cast<uint64_t (*)(uint32_t)>(Entered)(Row),
              packedOddAot(Row)) << "common T2 row=" << Row;
    Host->leave(Ticket);
    EXPECT_EQ(Host->activeExecutions(), 0u);
    EXPECT_EQ(Runtime->physicalReaders(Runtime->resourceGeneration()), 0u);
    EXPECT_EQ(Provider->outstandingBorrows(), 0u);
  }
  EXPECT_EQ(Runtime->inFlight(), 0u);
  EXPECT_EQ(Bundle->sampleCount, 8u);
}

TEST_F(SmallTableTest, RuntimeOwnedOddWidthsUsePackedGepInBothTargetOrders) {
  const unsigned Widths[] = {17, 33, 41, 49};
  const unsigned AccessBytes[] = {3, 5, 6, 7};
  for (bool Little : {true, false}) {
    const std::string Header = std::string("target datalayout = \"") +
        (Little ? "e" : "E") + "-p:64:64-i64:64-n32:64-S128\"\n";
    for (bool RuntimeOwned : {false, true}) {
      SMDiagnostic Diagnostic;
      auto M = parseAssemblyString(
          packedOddModuleText(Header, Widths, false), Diagnostic, Ctx);
      ASSERT_NE(M, nullptr);
      EJitSmallTablePlan Plan;
      Plan.entryName = "packed_odd_entry";
      Plan.sourceVarName = "g_packed_odd";
      Plan.elementBytes = 32;
      Plan.sourceStrides.push_back(32);
      Plan.dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, 3});
      Plan.littleEndian = Little;
      Plan.storage = RuntimeOwned ? EJitSmallTableStorage::RuntimeOwned
                                  : EJitSmallTableStorage::CompilerEmitted;
      Plan.runtimeRowAdmission = RuntimeOwned;
      Plan.readiness = testReadiness();
      for (unsigned I = 0; I < 4; ++I) {
        EJitSmallTableField Field;
        Field.sourceOffset = I * 8;
        Field.accessSize = AccessBytes[I];
        Field.bitWidth = Widths[I];
        Field.retainedAxes = {0};
        Field.tableRows = 3;
        Field.tableBytes = 3 * AccessBytes[I];
        Field.columnName = "__ejit_stab_packed_g" + std::to_string(I);
        Plan.fields.push_back(std::move(Field));
      }
      for (unsigned Row = 0; Row < 3; ++Row)
        Plan.rows.push_back({true, {0x10000u + Row, 0x100000000ULL + Row,
                                    0x10000000000ULL + Row,
                                    0x1000000000000ULL + Row}});
      std::string Error;
      ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
      ASSERT_TRUE(EJitSmallTablePass::materialize(*M, Plan, &Error)) << Error;
      const auto Contract = buildAdmissionContract(Plan);
      for (unsigned I = 0; I < 4; ++I) {
        const auto *Global = M->getNamedGlobal(Plan.fields[I].columnName);
        ASSERT_NE(Global, nullptr);
        const auto *Array = dyn_cast<ArrayType>(Global->getValueType());
        ASSERT_NE(Array, nullptr);
        EXPECT_EQ(Contract.fields[I].resource.alignment, 1u);
        EXPECT_EQ(Array->getElementType()->getIntegerBitWidth(),
                  RuntimeOwned ? 8u : Widths[I]);
        EXPECT_EQ(Array->getNumElements(),
                  RuntimeOwned ? Plan.fields[I].tableBytes : 3u);
        EXPECT_EQ(Global->hasInitializer(), !RuntimeOwned);
      }
      EJitSmallTablePass Pass(Plan);
      FunctionAnalysisManager Analyses;
      Pass.run(*M->getFunction("packed_odd_entry"), Analyses);
      EXPECT_EQ(Pass.getStats().tableReplaced, 4u);
      unsigned Loads = 0;
      for (const Instruction &Inst :
           instructions(*M->getFunction("packed_odd_entry"))) {
        const auto *Load = dyn_cast<LoadInst>(&Inst);
        if (!Load || !Load->getMetadata("ejit.smalltable.load"))
          continue;
        ASSERT_LT(Loads, 4u);
        const auto *Gep = dyn_cast<GetElementPtrInst>(Load->getPointerOperand());
        ASSERT_NE(Gep, nullptr);
        if (RuntimeOwned) {
          EXPECT_TRUE(Gep->getSourceElementType()->isIntegerTy(8));
          EXPECT_EQ(Gep->getNumIndices(), 1u);
          EXPECT_EQ(Load->getAlign().value(), 1u);
          const auto *Mul = dyn_cast<BinaryOperator>(Gep->getOperand(1));
          ASSERT_NE(Mul, nullptr);
          EXPECT_EQ(Mul->getOpcode(), Instruction::Mul);
          const auto *Scale = dyn_cast<ConstantInt>(Mul->getOperand(1));
          ASSERT_NE(Scale, nullptr);
          EXPECT_EQ(Scale->getZExtValue(), AccessBytes[Loads]);
        } else {
          const auto *Typed = dyn_cast<ArrayType>(Gep->getSourceElementType());
          ASSERT_NE(Typed, nullptr);
          EXPECT_EQ(Typed->getElementType()->getIntegerBitWidth(), Widths[Loads]);
          EXPECT_EQ(Gep->getNumIndices(), 2u);
          EXPECT_EQ(Load->getAlign(),
                    M->getDataLayout().getABITypeAlign(Load->getType()));
        }
        ++Loads;
      }
      EXPECT_EQ(Loads, 4u);
    }
  }
}

EJitSmallTablePlan packedOddGuardPlan(bool Little) {
  EJitSmallTablePlan Plan;
  Plan.entryName = "packed_odd_entry";
  Plan.sourceVarName = "g_packed_odd";
  Plan.elementBytes = 32;
  Plan.dims.push_back({EJitSmallTableDim::Kind::Argument, 0, 0, 3});
  Plan.sourceStrides.push_back(32);
  Plan.littleEndian = Little;
  Plan.storage = EJitSmallTableStorage::RuntimeOwned;
  Plan.runtimeRowAdmission = true;
  Plan.readiness = testReadiness();
  const unsigned Widths[] = {17, 33, 41, 49};
  for (unsigned I = 0; I < 4; ++I) {
    EJitSmallTableField Field;
    Field.sourceOffset = I * 8;
    Field.bitWidth = Widths[I];
    Field.accessSize = (Widths[I] + 7) / 8;
    Field.retainedAxes = {0};
    Field.tableRows = 3;
    Field.tableBytes = 3 * Field.accessSize;
    Field.columnName = "__ejit_stab_guard_" + std::to_string(I);
    Plan.fields.push_back(std::move(Field));
  }
  for (unsigned Row = 0; Row < 3; ++Row)
    Plan.rows.push_back({true, {0x10000, 0x100000000ULL + Row,
                                0x10000000000ULL + Row,
                                0x1000000000000ULL + Row}});
  // Refusal must occur before even this otherwise-safe uniform replacement.
  auto &Uniform = Plan.fields[0];
  Uniform.strategy = EJitSmallTableStrategy::Uniform;
  Uniform.uniformValue = 0x10000;
  Uniform.retainedAxes.clear();
  Uniform.tableRows = Uniform.tableBytes = 0;
  Uniform.columnName.clear();
  return Plan;
}

TEST_F(SmallTableTest, DirectPassRefusesForeignRuntimeColumnsWithoutAnyIrEdits) {
  enum Fault { StaleTyped, ShortBytes, Defined, ConstantDefined, ConstantDeclared };
  const unsigned Widths[] = {17, 33, 41, 49};
  for (bool Little : {true, false}) {
    for (Fault Bad : {StaleTyped, ShortBytes, Defined, ConstantDefined,
                      ConstantDeclared}) {
      SCOPED_TRACE(std::string(Little ? "LE" : "BE") +
                   " column fault=" + std::to_string(Bad));
      const std::string Header = std::string("target datalayout = \"") +
          (Little ? "e" : "E") + "-p:64:64-i64:64-n32:64-S128\"\n";
      SMDiagnostic Diagnostic;
      auto M = parseAssemblyString(packedOddModuleText(Header, Widths, false),
                                    Diagnostic, Ctx);
      ASSERT_NE(M, nullptr);
      auto Plan = packedOddGuardPlan(Little);
      std::string Error;
      ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
      for (unsigned I = 1; I < Plan.fields.size(); ++I) {
        const auto &Field = Plan.fields[I];
        ArrayType *ColumnType =
            ArrayType::get(Type::getInt8Ty(Ctx), Field.tableBytes);
        if (I == 1 && Bad == StaleTyped)
          ColumnType = ArrayType::get(Type::getIntNTy(Ctx, Field.bitWidth),
                                       Field.tableRows);
        if (I == 1 && Bad == ShortBytes)
          ColumnType = ArrayType::get(Type::getInt8Ty(Ctx), Field.tableBytes - 1);
        Constant *Initializer = nullptr;
        if (I == 1 && (Bad == Defined || Bad == ConstantDefined))
          Initializer = ConstantAggregateZero::get(ColumnType);
        new GlobalVariable(*M, ColumnType,
                           I == 1 && (Bad == ConstantDefined ||
                                      Bad == ConstantDeclared),
                           GlobalValue::ExternalLinkage, Initializer,
                           Field.columnName);
      }
      const std::string Before = printEndianTestModule(*M);
      EJitSmallTablePass Pass(Plan);
      FunctionAnalysisManager Analyses;
      const auto Preserved = Pass.run(*M->getFunction("packed_odd_entry"),
                                      Analyses);
      EXPECT_TRUE(Preserved.areAllPreserved());
      EXPECT_EQ(Pass.getStats().uniformFolded, 0u);
      EXPECT_EQ(Pass.getStats().tableReplaced, 0u);
      EXPECT_EQ(printEndianTestModule(*M), Before)
          << "foreign runtime storage cannot trigger any partial replacement";
    }
  }
}

TEST_F(SmallTableTest, MaterializeRefusesStaleTypedRuntimeColumnWithoutIrEdits) {
  const unsigned Widths[] = {17, 33, 41, 49};
  for (bool Little : {true, false}) {
    const std::string Header = std::string("target datalayout = \"") +
        (Little ? "e" : "E") + "-p:64:64-i64:64-n32:64-S128\"\n";
    SMDiagnostic Diagnostic;
    auto M = parseAssemblyString(packedOddModuleText(Header, Widths, false),
                                  Diagnostic, Ctx);
    ASSERT_NE(M, nullptr);
    auto Plan = packedOddGuardPlan(Little);
    std::string Error;
    ASSERT_TRUE(Plan.isConsistent(&Error)) << Error;
    const auto &Field = Plan.fields[1];
    auto *StaleType = ArrayType::get(Type::getIntNTy(Ctx, Field.bitWidth),
                                     Field.tableRows);
    new GlobalVariable(*M, StaleType, false, GlobalValue::ExternalLinkage,
                       nullptr, Field.columnName);
    const std::string Before = printEndianTestModule(*M);
    EXPECT_FALSE(EJitSmallTablePass::materialize(*M, Plan, &Error));
    EXPECT_NE(Error.find("different type"), std::string::npos) << Error;
    EXPECT_EQ(printEndianTestModule(*M), Before);
  }
}

/// B0 fail-closed: with no provider, with a provider that cannot grant the
/// protected read borrow, with a moved configuration generation, or with no
/// confirmed-ready member, nothing may be specialized.
TEST_F(SmallTableRuntimeTest, RuntimeIsFailClosedWithoutProviderOrBorrow) {
  auto M = parseAutoEntry();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  std::string Error;

  // (a) No provider at all: no facts, no borrow, no specialization.
  auto NoProvider = makeRuntime(nullptr);
  ASSERT_NE(NoProvider, nullptr);
  auto R0 = NoProvider->prepare(*M, "a_entry", "g_auto", Dims, Error);
  EXPECT_FALSE(static_cast<bool>(R0));
  EXPECT_NE(Error.find("no readiness provider"), std::string::npos) << Error;
  EXPECT_EQ(NoProvider->plan(), nullptr);
  EXPECT_EQ(NoProvider->stats().plannedRows, 0u);
  consumeError(R0.takeError());

  // (b) The product configuration transaction is missing/locked: the adapter
  //     refuses the borrow and the entry stays AOT for that exact reason.
  auto P1 = std::make_shared<EJitSmallTableHostProvider>(
      "g_auto", reinterpret_cast<const void *>(&g_auto[0][0]), sizeof(g_auto),
      0xE001);
  P1->addReadyMember({0, 0}, 1);
  P1->refuseBorrow("product configuration transaction unavailable");
  auto R1 = makeRuntime(P1);
  ASSERT_NE(R1, nullptr);
  auto R1OrErr = R1->prepare(*M, "a_entry", "g_auto", Dims, Error);
  EXPECT_FALSE(static_cast<bool>(R1OrErr));
  EXPECT_NE(Error.find("product configuration transaction unavailable"),
            std::string::npos)
      << Error;
  EXPECT_EQ(R1->plan(), nullptr);
  consumeError(R1OrErr.takeError());

  // (c) A moved configuration generation invalidates the old facts: a stale
  //     borrow is never taken.
  auto P2 = std::make_shared<EJitSmallTableHostProvider>(
      "g_auto", reinterpret_cast<const void *>(&g_auto[0][0]), sizeof(g_auto),
      0xE002);
  P2->addReadyMember({0, 0}, 1);
  P2->invalidateGeneration();
  auto R2 = makeRuntime(P2);
  ASSERT_NE(R2, nullptr);
  auto R2OrErr = R2->prepare(*M, "a_entry", "g_auto", Dims, Error);
  EXPECT_FALSE(static_cast<bool>(R2OrErr));
  EXPECT_NE(Error.find("configuration generation moved"), std::string::npos)
      << Error;
  EXPECT_EQ(R2->plan(), nullptr);
  consumeError(R2OrErr.takeError());

  // (d) An epoch-less or member-less fact source is not a proof either.
  auto P3 = std::make_shared<EJitSmallTableHostProvider>(
      "g_auto", reinterpret_cast<const void *>(&g_auto[0][0]), sizeof(g_auto),
      /*Epoch=*/0);
  P3->addReadyMember({0, 0}, 1);
  auto R3 = makeRuntime(P3);
  ASSERT_NE(R3, nullptr);
  auto R3OrErr = R3->prepare(*M, "a_entry", "g_auto", Dims, Error);
  EXPECT_FALSE(static_cast<bool>(R3OrErr));
  EXPECT_NE(Error.find("no domain epoch"), std::string::npos) << Error;
  consumeError(R3OrErr.takeError());

  auto P4 = std::make_shared<EJitSmallTableHostProvider>(
      "g_auto", reinterpret_cast<const void *>(&g_auto[0][0]), sizeof(g_auto),
      0xE004);
  auto R4 = makeRuntime(P4);
  ASSERT_NE(R4, nullptr);
  auto R4OrErr = R4->prepare(*M, "a_entry", "g_auto", Dims, Error);
  EXPECT_FALSE(static_cast<bool>(R4OrErr));
  EXPECT_NE(Error.find("confirms no ready member"), std::string::npos) << Error;
  EXPECT_EQ(R4->plan(), nullptr);
  consumeError(R4OrErr.takeError());

  // A member whose fields are not all initialized is not ready.
  auto P5 = std::make_shared<EJitSmallTableHostProvider>(
      "g_auto", reinterpret_cast<const void *>(&g_auto[0][0]), sizeof(g_auto),
      0xE005);
  P5->addReadyMember({0, 0}, 1, /*FieldsInitialized=*/false);
  auto R5 = makeRuntime(P5);
  ASSERT_NE(R5, nullptr);
  auto R5OrErr = R5->prepare(*M, "a_entry", "g_auto", Dims, Error);
  EXPECT_FALSE(static_cast<bool>(R5OrErr));
  EXPECT_NE(Error.find("confirms no ready member"), std::string::npos) << Error;
  consumeError(R5OrErr.takeError());
}

/// B1 + B2 first half: the runtime plans from provider facts, publishes the
/// admitted rows into its own resource, binds every column of THIS generation to
/// that resource, compiles the common instrumented T1 through the real engine
/// and executes it. A member the provider never confirmed is never admitted and
/// never consumes sampling budget.
TEST_F(SmallTableRuntimeTest, RuntimePublishesAdmittedRowsAndCommonT1RunsRealCode) {
  auto M = parseAutoEntry();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  std::shared_ptr<EJitSmallTableHostProvider> Provider = hostProvider(0xE010);
  auto RT = makeRuntime(Provider);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);

  std::string Error;
  auto PlanOrErr = RT->prepare(*M, "a_entry", "g_auto", Dims, Error);
  ASSERT_TRUE(static_cast<bool>(PlanOrErr)) << Error;
  const EJitSmallTablePlan *Plan = *PlanOrErr;
  ASSERT_NE(Plan, nullptr);
  EXPECT_EQ(RT->providerLabel(), EJitSmallTableHostProvider::Label);
  EXPECT_EQ(Plan->readiness.providerLabel, EJitSmallTableHostProvider::Label)
      << "the host adapter must be recorded as such, never as product readiness";
  EXPECT_EQ(Plan->fields[0].strategy, EJitSmallTableStrategy::Uniform);
  EXPECT_EQ(Plan->fields[1].strategy, EJitSmallTableStrategy::Table);
  EXPECT_EQ(RT->resourceGeneration(), 0xE010u);
  EXPECT_EQ(Provider->outstandingBorrows(), 0u)
      << "planning must release its protected read borrow";

  auto FnOrErr = RT->compileCommonT1(1, Error);
  ASSERT_TRUE(static_cast<bool>(FnOrErr)) << Error;
  auto Fn = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*FnOrErr);
  ASSERT_NE(Fn, nullptr);
  EXPECT_EQ(RT->engine().getTransformClaimSkips(), 3u)
      << "a runtime-owned column is already defined by the runtime absolute "
         "symbol, so the engine must not claim a second definition of it";
  EXPECT_EQ(Provider->outstandingBorrows(), 0u)
      << "row publication must release its protected read borrow";
  EXPECT_TRUE(RT->sessionOpen());
  EXPECT_EQ(RT->currentSessionSamples(), 0u)
      << "opening the session is not a sample";

  // Every admitted (cell, TRP) coordinate was published exactly once per table
  // field: byCell has kAutoCells rows, byTrp kAutoTrps and joint the product.
  EXPECT_EQ(RT->stats().publishedRows,
            static_cast<uint64_t>(kAutoCells + kAutoTrps +
                                  kAutoCells * kAutoTrps));
  uint64_t Bits = 0;
  ASSERT_TRUE(RT->resource()->published(1, 2, &Bits));
  EXPECT_EQ(Bits, 7u + 2u) << "byCell column row 2 holds cell 2's value";
  ASSERT_TRUE(RT->resource()->published(2, 1, &Bits));
  EXPECT_EQ(Bits, 2u + 1u) << "byTrp column row 1 holds TRP 1's value";
  ASSERT_TRUE(RT->resource()->published(3, 2 * kAutoTrps + 1, &Bits));
  EXPECT_EQ(Bits, static_cast<uint64_t>(2 * kAutoTrps + 1));

  // Real resource identity: each column of this generation resolves to the
  // runtime's own resource, not to a same-named foreign table.
  for (unsigned F = 0; F < Plan->fields.size(); ++F) {
    if (Plan->fields[F].strategy != EJitSmallTableStrategy::Table)
      continue;
    auto AddrOrErr = RT->engine().lookup(1, Plan->fields[F].columnName);
    ASSERT_TRUE(static_cast<bool>(AddrOrErr))
        << Plan->fields[F].columnName << ": " << toString(AddrOrErr.takeError());
    EXPECT_EQ(*AddrOrErr, RT->resource()->columnAddress(F));
  }

  // Real execution of the common T1 over every admitted row.
  for (unsigned C = 0; C < kAutoCells; ++C)
    for (unsigned T = 0; T < kAutoTrps; ++T)
      for (int32_t X = -2; X <= 5; ++X)
        EXPECT_EQ(Fn(C, T, X), aotAuto(g_auto[C][T], X))
            << "cell=" << C << " trp=" << T << " x=" << X;

  // A real admitted execution is a sample: counted, tracked in flight, and the
  // completion is not stale. The sampling window runs under the protected read
  // borrow, so the instrumented entry is never called outside the configuration
  // generation the plan was proven against.
  EJitSmallTableSampleTicket Ticket;
  EXPECT_TRUE(RT->enterAdmitted({0, 0}, &Ticket, &Error)) << Error;
  EXPECT_TRUE(Ticket.valid);
  EXPECT_TRUE(RT->samplingProtected())
      << "the sampling window holds the protected read borrow";
  EXPECT_EQ(Provider->outstandingBorrows(), 1u)
      << "one borrow for the session, not one per sample";
  EXPECT_EQ(RT->inFlight(), 1u);
  EXPECT_EQ(RT->currentSessionSamples(), 1u);
  EXPECT_EQ(RT->stats().acceptedSamples, 1u);
  RT->leaveAdmitted(Ticket);
  EXPECT_EQ(RT->inFlight(), 0u);
  EXPECT_EQ(RT->stats().staleCallbacks, 0u);
  EXPECT_TRUE(RT->samplingProtected())
      << "completing one sample does not end the sampling window";
  EXPECT_EQ(Provider->outstandingBorrows(), 1u);

  // A coordinate the provider never confirmed ready is not admitted at all, so
  // the caller must take the AOT path and no budget is consumed.
  EXPECT_FALSE(RT->admittedMembers().empty());
  std::string Why;
  EXPECT_FALSE(RT->enterAdmitted({kAutoCells + 3, 0}, &Ticket, &Why));
  EXPECT_FALSE(Ticket.valid);
  EXPECT_NE(Why.find("never admitted"), std::string::npos) << Why;
  EXPECT_EQ(RT->currentSessionSamples(), 1u)
      << "a not-admitted execution never consumes sampling budget";
}

/// B2: ONE common session per code generation with an AGGREGATE budget shared by
/// the admitted ready members; freezing waits for admitted executions still in
/// flight; the frozen bundle carries the real identity and synthesized profile
/// and the common T2 really executes.
TEST_F(SmallTableRuntimeTest, RuntimeCommonBudgetFreezesBundleAndRunsCommonT2) {
  auto M = parseAutoEntry();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  std::shared_ptr<EJitSmallTableHostProvider> Provider = hostProvider(0xE020);
  EJitSmallTableRuntime::Options Opts;
  Opts.sampling.aggregateLimit = 5;
  Opts.sampling.freezeWaitMillis = 150;
  auto RT = makeRuntime(Provider, Opts);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);

  std::string Error;
  ASSERT_TRUE(static_cast<bool>(RT->prepare(*M, "a_entry", "g_auto", Dims, Error)))
      << Error;
  auto FnOrErr = RT->compileCommonT1(1, Error);
  ASSERT_TRUE(static_cast<bool>(FnOrErr)) << Error;
  auto Fn = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*FnOrErr);
  ASSERT_NE(Fn, nullptr);
  const uint64_t Session = RT->sessionId();

  // Four real samples spread over three DIFFERENT members, then the fifth
  // admitted execution is left IN FLIGHT: the budget is aggregate across
  // admitted members, not per member, and a grant is not a completion.
  const uint64_t Members[3][2] = {{0, 0}, {1, 1}, {3, 2}};
  for (unsigned I = 0; I < 4; ++I) {
    EJitSmallTableSampleTicket T;
    ASSERT_TRUE(RT->enterAdmitted({Members[I % 3][0], Members[I % 3][1]}, &T,
                                  &Error))
        << Error;
    ASSERT_TRUE(T.valid);
    RT->leaveAdmitted(T);
    EXPECT_EQ(RT->currentSessionSamples(), I + 1);
  }

  // A grant is not a completion: freeze must wait for the admitted execution
  // that is still in flight and refuse rather than read half a sample.
  EJitSmallTableSampleTicket InFlightTicket;
  ASSERT_TRUE(RT->enterAdmitted({1, 1}, &InFlightTicket, &Error)) << Error;
  ASSERT_TRUE(InFlightTicket.valid);
  EXPECT_EQ(RT->inFlight(), 1u);
  EXPECT_EQ(RT->currentSessionSamples(), 5u);
  EXPECT_TRUE(RT->samplingExhausted());
  EXPECT_EQ(RT->sampleBudget(), 5u);
  std::string FrozenError;
  auto TooEarly = RT->freeze(FrozenError);
  EXPECT_FALSE(static_cast<bool>(TooEarly));
  EXPECT_NE(FrozenError.find("still in flight"), std::string::npos)
      << FrozenError;
  consumeError(TooEarly.takeError());
  RT->leaveAdmitted(InFlightTicket);
  EXPECT_EQ(RT->inFlight(), 0u);

  // Quota exhaustion rejects a new T1 so its real counters cannot exceed the
  // configured window. The caller continues on AOT until T2 is published.
  EJitSmallTableSampleTicket Over;
  EXPECT_FALSE(RT->enterAdmitted({0, 0}, &Over, &Error));
  EXPECT_FALSE(Over.valid);
  EXPECT_FALSE(Over.counted);
  EXPECT_EQ(RT->currentSessionSamples(), 5u);
  EXPECT_EQ(RT->inFlight(), 0u);

  auto BundleOrErr = RT->freeze(Error);
  ASSERT_TRUE(static_cast<bool>(BundleOrErr)) << Error;
  const EJitSmallTableProfileBundle *B = *BundleOrErr;
  ASSERT_NE(B, nullptr);
  EXPECT_EQ(RT->bundle(), B);
  EXPECT_FALSE(RT->samplingProtected())
      << "freeze ends the sampling window and releases its read borrow";
  EXPECT_EQ(Provider->outstandingBorrows(), 0u);
  EXPECT_EQ(B->entryName, "a_entry");
  EXPECT_EQ(B->codeGeneration, 1u);
  EXPECT_EQ(B->domainEpoch, 0xE020u);
  EXPECT_EQ(B->sessionId, Session);
  EXPECT_EQ(B->contractHash, RT->contract().identityHash);
  EXPECT_EQ(B->resourceAddress,
            reinterpret_cast<uintptr_t>(RT->resource()->base()));
  EXPECT_EQ(B->resourceGeneration, RT->resourceGeneration());
  EXPECT_EQ(B->sampleCount, 5u)
      << "only the real admitted samples of this generation are in the bundle";
  EXPECT_EQ(B->participatingMembers, 3u);
  EXPECT_EQ(B->readinessProvider, EJitSmallTableHostProvider::Label);
  EXPECT_FALSE(B->profileData.empty())
      << "the bundle must carry a synthesized profile, not a model";
  EXPECT_FALSE(B->counters.empty())
      << "the bundle must carry the real Tier-1 counter addresses";

  // The common T2 compiles from that one immutable bundle and really runs. Both
  // tiers must read the SAME table resource; compileCommonT2 refuses otherwise.
  auto T2OrErr = RT->compileCommonT2(Error);
  ASSERT_TRUE(static_cast<bool>(T2OrErr)) << Error;
  auto T2 = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*T2OrErr);
  ASSERT_NE(T2, nullptr);
  for (unsigned C = 0; C < kAutoCells; ++C)
    for (unsigned T = 0; T < kAutoTrps; ++T)
      for (int32_t X = -1; X <= 3; ++X)
        EXPECT_EQ(T2(C, T, X), aotAuto(g_auto[C][T], X))
            << "T2 cell=" << C << " trp=" << T << " x=" << X;
  EXPECT_FALSE(RT->sessionOpen()) << "the session is frozen once and for all";

  // A frozen session accepts no further sample, and the bundle stays immutable.
  EJitSmallTableSampleTicket AfterFreeze;
  EXPECT_FALSE(RT->enterAdmitted({0, 0}, &AfterFreeze, &Error));
  auto Again = RT->freeze(Error);
  ASSERT_TRUE(static_cast<bool>(Again)) << Error;
  EXPECT_EQ(*Again, B) << "freeze returns the one immutable bundle";
}

/// B1: a compatible late member joins the SAME session and the SAME generation,
/// so the common quota is not restarted.
TEST_F(SmallTableRuntimeTest, RuntimeLateCompatibleMemberKeepsTheSameSession) {
  auto M = parseAutoEntry();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  std::shared_ptr<EJitSmallTableHostProvider> Provider =
      std::make_shared<EJitSmallTableHostProvider>(
          "g_auto", reinterpret_cast<const void *>(&g_auto[0][0]), sizeof(g_auto),
          0xE030);
  // Every member except (2,1) is confirmed before the first compile, so the
  // plan's projections already cover (2,1)'s coordinates.
  for (unsigned C = 0; C < kAutoCells; ++C)
    for (unsigned T = 0; T < kAutoTrps; ++T) {
      if (C == 2 && T == 1)
        continue;
      Provider->addReadyMember({C, T}, 1);
    }
  EJitSmallTableRuntime::Options Opts;
  Opts.sampling.aggregateLimit = 4;
  auto RT = makeRuntime(Provider, Opts);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);
  std::string Error;
  ASSERT_TRUE(static_cast<bool>(RT->prepare(*M, "a_entry", "g_auto", Dims, Error)))
      << Error;
  auto T1OrErr = RT->compileCommonT1(1, Error);
  ASSERT_TRUE(static_cast<bool>(T1OrErr)) << Error;
  auto Fn = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*T1OrErr);
  ASSERT_NE(Fn, nullptr);

  EJitSmallTableSampleTicket T;
  ASSERT_TRUE(RT->enterAdmitted({0, 0}, &T, &Error)) << Error;
  RT->leaveAdmitted(T);
  const uint64_t SamplesBefore = RT->currentSessionSamples();
  const uint64_t SessionBefore = RT->sessionId();
  const uint64_t ResourceBefore = RT->resourceGeneration();
  const uint64_t ContractBefore = RT->contract().identityHash;

  // The late member is now confirmed ready and validated against the exported
  // contract. Its byCell coordinate 2 (cell 2) is already published by the rows
  // that shared that cell, but its joint coordinate belongs to no other member:
  // the joint table is injective over the proven domain, so exactly one new
  // projected row must be published. The member is therefore EXTENDABLE, which
  // still admits it to the SAME generation: nothing is re-planned, no new
  // resource generation is created, the contract identity is unchanged and the
  // common quota does not restart.
  const uint64_t PublishedBeforeLate = RT->stats().publishedRows;
  Provider->addReadyMember({2, 1}, 2);
  std::string Why;
  const EJitSmallTableAdmission Admission = RT->admitMember({2, 1}, &Why);
  EXPECT_EQ(Admission, EJitSmallTableAdmission::Extendable)
      << admissionName(Admission) << ": " << Why;
  EXPECT_EQ(RT->currentSessionSamples(), SamplesBefore);
  EXPECT_EQ(RT->sessionId(), SessionBefore);
  EXPECT_EQ(RT->resourceGeneration(), ResourceBefore);
  EXPECT_EQ(RT->contract().identityHash, ContractBefore);
  EXPECT_EQ(RT->stats().publishedRows, PublishedBeforeLate + 1)
      << "an extendable member publishes exactly its own new projected row";

  // Re-admitting the now-published member is COMPATIBLE and republishes
  // nothing: the coordinate it projects to already carries its value.
  const uint64_t PublishedAfterLate = RT->stats().publishedRows;
  EXPECT_EQ(RT->admitMember({2, 1}, &Why), EJitSmallTableAdmission::Compatible)
      << Why;
  EXPECT_EQ(RT->stats().publishedRows, PublishedAfterLate)
      << "a compatible member republishes nothing";

  uint64_t Bits = 0;
  ASSERT_TRUE(RT->resource()->published(1, 2, &Bits))
      << "the late member's byCell coordinate is published";
  EXPECT_EQ(Bits, 7u + 2u);

  // The late member can now dispatch and is counted in the same session.
  ASSERT_TRUE(RT->enterAdmitted({2, 1}, &T, &Error)) << Error;
  RT->leaveAdmitted(T);
  EXPECT_EQ(RT->currentSessionSamples(), SamplesBefore + 1);
  for (int32_t X = -1; X <= 2; ++X)
    EXPECT_EQ(Fn(2, 1, X), aotAuto(g_auto[2][1], X));

  // An out-of-schema coordinate is unusable, never published.
  const uint64_t PublishedBefore = RT->stats().publishedRows;
  EXPECT_EQ(RT->admitMember({kAutoCells, 0}, &Why),
            EJitSmallTableAdmission::Unusable);
  EXPECT_EQ(RT->stats().publishedRows, PublishedBefore);
}

/// B1: a conflicting member stays AOT; ONE coalesced new generation is prepared
/// for the union of the known members, only the affected fields widen, the old
/// members are migrated and the previous resource stays alive until retirement.
TEST_F(SmallTableRuntimeTest, RuntimeConflictPreparesOneGenerationAndMigratesMembers) {
  const unsigned Cells = 2;
  const unsigned Trps = 2;
  SMDiagnostic Err;
  auto M = parseAssemblyString(
      autoModuleText("s_entry", "g_sparse", "g_sparse_out", Cells, Trps), Err,
      Ctx);
  ASSERT_TRUE(M) << "sparse module failed to parse";
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(Cells, Trps);
  auto Provider = std::make_shared<EJitSmallTableHostProvider>(
      "g_sparse", reinterpret_cast<const void *>(&g_sparse[0][0]),
      sizeof(g_sparse), 0xE040);
  Provider->addReadyMember({0, 0}, 1);
  Provider->addReadyMember({0, 1}, 1);
  EJitSmallTableRuntime::Options Opts;
  Opts.sampling.aggregateLimit = 8;
  auto RT = makeRuntime(Provider, Opts);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);

  std::string Error;
  auto PlanOrErr = RT->prepare(*M, "s_entry", "g_sparse", Dims, Error);
  ASSERT_TRUE(static_cast<bool>(PlanOrErr)) << Error;
  const uint64_t Gen1Contract = RT->contract().identityHash;
  auto T1OrErr = RT->compileCommonT1(1, Error);
  ASSERT_TRUE(static_cast<bool>(T1OrErr)) << Error;
  auto Fn = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*T1OrErr);
  ASSERT_NE(Fn, nullptr);
  EXPECT_EQ(RT->plan()->fields[1].strategy, EJitSmallTableStrategy::Uniform)
      << "both cell-0 rows share byCell";
  EXPECT_EQ(RT->plan()->fields[2].strategy, EJitSmallTableStrategy::Uniform);

  // A conflicting late member: cell 1 has a different byCell, which the current
  // generation proved constant. It must stay AOT.
  Provider->addReadyMember({1, 0}, 2);
  std::string Why;
  const uint64_t PublishedBefore = RT->stats().publishedRows;
  EXPECT_EQ(RT->admitMember({1, 0}, &Why), EJitSmallTableAdmission::Conflict)
      << Why;
  EXPECT_EQ(RT->stats().publishedRows, PublishedBefore)
      << "a conflicting member publishes nothing";
  EXPECT_FALSE(RT->enterAdmitted({1, 0}, nullptr, &Why))
      << "a conflicting member must take the AOT path";

  // While an admitted execution is in flight the table generation may not move:
  // that code may still read the current resource.
  EJitSmallTableSampleTicket InFlight;
  ASSERT_TRUE(RT->enterAdmitted({0, 0}, &InFlight, &Error)) << Error;
  ASSERT_TRUE(InFlight.valid);
  SmallVector<EJitSmallTableRowKey, 4> Extra;
  Extra.push_back({{1, 0}});
  auto Blocked = RT->beginNextGeneration(Extra, Error);
  EXPECT_FALSE(static_cast<bool>(Blocked));
  EXPECT_NE(Error.find("still in flight"), std::string::npos) << Error;
  consumeError(Blocked.takeError());
  EXPECT_EQ(RT->resourceGeneration(), 0xE040u)
      << "a refused generation change leaves the live resource in place";
  RT->leaveAdmitted(InFlight);

  // ONE coalesced generation over the union of the members this runtime knows.
  auto NextOrErr = RT->beginNextGeneration(Extra, Error);
  ASSERT_TRUE(static_cast<bool>(NextOrErr)) << Error;
  EXPECT_EQ(RT->resourceGeneration(), 0xE041u);
  EXPECT_EQ(RT->stats().generationsPrepared, 1u);
  EXPECT_GT(RT->stats().migratedRows, 0u);
  EXPECT_NE(RT->contract().identityHash, Gen1Contract)
      << "a new generation has its own contract identity";
  EXPECT_EQ(RT->plan()->fields[1].strategy, EJitSmallTableStrategy::Table)
      << "the conflicting field widens (per-field solver)";
  ASSERT_EQ(RT->plan()->fields[1].retainedAxes.size(), 1u);
  EXPECT_EQ(RT->plan()->fields[1].retainedAxes[0], 0u);
  EXPECT_EQ(RT->plan()->fields[2].strategy, EJitSmallTableStrategy::Uniform)
      << "unaffected constants stay folded";
  EXPECT_FALSE(RT->sessionOpen())
      << "the previous generation's session is over, never merged forward";

  // The old resource is retained for code that may still dispatch to it.
  EXPECT_EQ(RT->retainedGenerationCount(), 1u);
  EXPECT_GT(RT->retainedBytes(), 0u);
  EXPECT_FALSE(RT->retireGenerationsUpTo(0xE042u))
      << "retiring above the current generation is refused";
  EXPECT_EQ(RT->retainedGenerationCount(), 1u);

  // The new generation compiles and serves the migrated old members and the new
  // one, all against the new resource.
  auto T1bOrErr = RT->compileCommonT1(2, Error);
  ASSERT_TRUE(static_cast<bool>(T1bOrErr)) << Error;
  auto Fn2 = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*T1bOrErr);
  ASSERT_NE(Fn2, nullptr);
  uint64_t Bits = 0;
  ASSERT_TRUE(RT->resource()->published(1, 1, &Bits))
      << "cell 1's byCell coordinate is published in the new generation";
  EXPECT_EQ(Bits, 9u);
  for (unsigned C = 0; C < Cells; ++C)
    for (unsigned T = 0; T < Trps; ++T) {
      if (C == 1 && T == 1)
        continue; // never confirmed ready, stays AOT
      for (int32_t X = -1; X <= 2; ++X)
        EXPECT_EQ(Fn2(C, T, X), aotAuto(g_sparse[C][T], X))
            << "cell=" << C << " trp=" << T << " x=" << X;
    }

  // Safe migration is complete: the retired generation is released.
  EXPECT_TRUE(RT->retireGenerationsUpTo(0xE040u));
  EXPECT_EQ(RT->retainedGenerationCount(), 0u);
  EXPECT_EQ(RT->retainedBytes(), 0u);
  EXPECT_EQ(RT->stats().retiredGenerations, 1u);
  EXPECT_EQ(RT->resourceGeneration(), 0xE041u)
      << "retirement never frees the live generation";
}

/// B1/B2 PHYSICAL LIFETIME (2026-09-16 P1 repair): the exact counterexample the
/// coordinator derived from this source. A cancelled execution of generation G
/// is no longer the session's, but it is still a REAL call inside G's column
/// storage. Cancel -> beginNextGeneration -> retireGenerationsUpTo(G) must NOT
/// free that storage: the retirement is deferred to the execution's own leave,
/// which then performs the safe reclamation.
TEST_F(SmallTableRuntimeTest, CancelledExecutionKeepsItsGenerationUntilTheRealLeave) {
  auto M = parseAutoEntry();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  std::shared_ptr<EJitSmallTableHostProvider> Provider = hostProvider(0xE060);
  EJitSmallTableRuntime::Options Opts;
  Opts.sampling.aggregateLimit = 8;
  auto RT = makeRuntime(Provider, Opts);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);
  std::string Error;
  ASSERT_TRUE(static_cast<bool>(RT->prepare(*M, "a_entry", "g_auto", Dims, Error)))
      << Error;
  ASSERT_TRUE(static_cast<bool>(RT->compileCommonT1(1, Error))) << Error;
  const uint64_t G1 = RT->resourceGeneration();

  // Step 1: a real admitted execution of G1 is entered and PAUSED.
  EJitSmallTableSampleTicket Ticket;
  ASSERT_TRUE(RT->enterAdmitted({0, 0}, &Ticket, &Error)) << Error;
  ASSERT_TRUE(Ticket.valid);
  ASSERT_NE(RT->resource(), nullptr);
  ASSERT_FALSE(RT->resource()->columns().empty());
  const unsigned TableField = RT->resource()->columns().front().fieldIndex;
  const uintptr_t G1Column0 =
      reinterpret_cast<uintptr_t>(RT->resource()->columnAddress(TableField));
  ASSERT_NE(G1Column0, 0u);
  EXPECT_EQ(RT->physicalReaders(G1), 1u);

  // Step 2: cancel (logical) and replace the generation (which retains G1).
  RT->cancel("timeout while the call is running");
  EXPECT_EQ(RT->inFlight(), 1u) << "the real call is still in flight";
  EXPECT_EQ(RT->physicalReaders(G1), 1u);
  SmallVector<EJitSmallTableRowKey, 2> Extra;
  Extra.push_back({{1, 1}});
  auto NextOrErr = RT->beginNextGeneration(Extra, Error);
  ASSERT_TRUE(static_cast<bool>(NextOrErr)) << Error
      << "a cancelled session does not block the new generation";
  const uint64_t G2 = RT->resourceGeneration();
  ASSERT_GT(G2, G1);
  EXPECT_EQ(RT->retainedGenerationCount(), 1u);

  // Step 3: retire G1. The storage a running call reads is NOT freed - the
  // retirement is deferred and the bytes stay accounted.
  EXPECT_TRUE(RT->retireGenerationsUpTo(G1));
  EXPECT_EQ(RT->stats().retiredGenerations, 0u)
      << "nothing may be freed while a real execution is inside the generation";
  EXPECT_EQ(RT->retainedGenerationCount(), 1u);
  EXPECT_GT(RT->retainedBytes(), 0u);
  EXPECT_EQ(RT->pendingRetireGenerationCount(), 1u);
  EXPECT_GT(RT->pendingRetireBytes(), 0u);
  EXPECT_GE(RT->stats().deferredRetirements, 1u);
  EXPECT_EQ(RT->physicalReaders(G1), 1u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(RT->resource()->columnAddress(TableField)) != 0,
            true)
      << "the CURRENT generation is unaffected";

  // Step 4: the real return. The old completion is stale for sampling and it is
  // the event that releases G1's storage - never a premature free.
  RT->leaveAdmitted(Ticket);
  EXPECT_EQ(RT->inFlight(), 0u);
  EXPECT_EQ(RT->physicalReaders(G1), 0u);
  EXPECT_EQ(RT->pendingRetireGenerationCount(), 0u);
  EXPECT_EQ(RT->stats().retiredGenerations, 1u);
  EXPECT_GE(RT->stats().reclaimedAfterReaders, 1u)
      << "the safe reclamation is recorded, not silent";
  EXPECT_EQ(RT->retainedGenerationCount(), 0u);
  EXPECT_EQ(RT->retainedBytes(), 0u);
  EXPECT_GE(RT->stats().staleCallbacks, 1u)
      << "the late completion is counted against its OWN session";
  EXPECT_EQ(RT->resourceGeneration(), G2)
      << "the retired generation can never become the current one again";

  // The replacement generation is fully usable: it compiles, samples and runs.
  auto T1bOrErr = RT->compileCommonT1(2, Error);
  ASSERT_TRUE(static_cast<bool>(T1bOrErr)) << Error;
  auto Fn2 = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*T1bOrErr);
  ASSERT_NE(Fn2, nullptr);
  EXPECT_EQ(RT->currentSessionSamples(), 0u)
      << "a stale completion never settles into the replacement session";
  for (int32_t X = -1; X <= 2; ++X)
    EXPECT_EQ(Fn2(0, 0, X), aotAuto(g_auto[0][0], X));
}

/// B2 cancel/timeout: the session stops granting tickets, its borrows are gone
/// and a ticket it granted becomes a stale callback.
TEST_F(SmallTableRuntimeTest, RuntimeCancelRejectsStaleCallbacksAndStopsDispatch) {
  auto M = parseAutoEntry();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  std::shared_ptr<EJitSmallTableHostProvider> Provider = hostProvider(0xE050);
  auto RT = makeRuntime(Provider);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);
  std::string Error;
  ASSERT_TRUE(static_cast<bool>(RT->prepare(*M, "a_entry", "g_auto", Dims, Error)))
      << Error;
  ASSERT_TRUE(static_cast<bool>(RT->compileCommonT1(1, Error))) << Error;
  EXPECT_TRUE(RT->sessionOpen());
  EXPECT_EQ(Provider->outstandingBorrows(), 0u);

  EJitSmallTableSampleTicket Ticket;
  ASSERT_TRUE(RT->enterAdmitted({0, 0}, &Ticket, &Error)) << Error;
  ASSERT_TRUE(Ticket.valid);
  EXPECT_EQ(RT->inFlight(), 1u);
  EXPECT_TRUE(RT->samplingProtected())
      << "a real sample runs under the session read borrow";
  EXPECT_EQ(Provider->outstandingBorrows(), 1u);

  RT->cancel("sampling window timed out");
  EXPECT_FALSE(RT->sessionOpen());
  EXPECT_EQ(RT->cancellationReason(), "sampling window timed out");
  // PHYSICAL LIFETIME (2026-09-16 P1 repair): the cancelled session gave up its
  // sample ACCOUNTING (`sessionInFlight_`), but the execution it granted is a
  // real call that has not returned. The runtime keeps its in-flight count and
  // the window's protected read until that call completes, because the
  // instrumented code already entered and still reads the table/source.
  EXPECT_EQ(RT->sessionInFlight(), 0u)
      << "the cancelled session no longer owns its samples";
  EXPECT_EQ(RT->inFlight(), 1u)
      << "the granted execution is still physically in flight";
  EXPECT_TRUE(RT->samplingProtected())
      << "the window's protected read guards the running call";
  EXPECT_EQ(Provider->outstandingBorrows(), 1u)
      << "cancel does not release a borrow a running execution still needs";

  // The ticket belongs to the cancelled session: its completion is stale, never
  // merged into whatever session comes next. It is also what releases the
  // physical lease, so the borrow and the in-flight count end at the real
  // return - not at the cancel.
  RT->leaveAdmitted(Ticket);
  EXPECT_EQ(RT->stats().staleCallbacks, 1u);
  EXPECT_EQ(RT->inFlight(), 0u);
  EXPECT_FALSE(RT->samplingProtected());
  EXPECT_EQ(Provider->outstandingBorrows(), 0u)
      << "the cancelled window's borrow is released at its last real return";

  // No new dispatch and no freeze on a cancelled session.
  EXPECT_FALSE(RT->enterAdmitted({0, 0}, &Ticket, &Error));
  EXPECT_NE(Error.find("timed out"), std::string::npos) << Error;
  std::string FreezeError;
  auto Frozen = RT->freeze(FreezeError, /*Force=*/true);
  EXPECT_FALSE(static_cast<bool>(Frozen));
  EXPECT_NE(FreezeError.find("timed out"), std::string::npos) << FreezeError;
  consumeError(Frozen.takeError());
  EXPECT_EQ(RT->bundle(), nullptr);
}

/// B2 fail-closed sampling: the sampling window runs under the configuration
/// side's protected read borrow. Without one the call is refused and stays on
/// the AOT path - the specialized code is never entered on an unprotected source
/// and no budget is consumed. A delayed borrow (transient refusal) is not
/// permanent: the same session samples normally once it can be granted again.
TEST_F(SmallTableRuntimeTest, RuntimeSamplingRefusesWithoutAProtectedBorrow) {
  auto M = parseAutoEntry();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  std::shared_ptr<EJitSmallTableHostProvider> Provider = hostProvider(0xE070);
  auto RT = makeRuntime(Provider);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);
  std::string Error;
  ASSERT_TRUE(static_cast<bool>(RT->prepare(*M, "a_entry", "g_auto", Dims, Error)))
      << Error;
  ASSERT_TRUE(static_cast<bool>(RT->compileCommonT1(1, Error))) << Error;
  EXPECT_EQ(Provider->outstandingBorrows(), 0u)
      << "planning and publication released their own borrows";

  Provider->refuseBorrow("product configuration transaction unavailable");
  EJitSmallTableSampleTicket Ticket;
  std::string Why;
  EXPECT_FALSE(RT->enterAdmitted({0, 0}, &Ticket, &Why));
  EXPECT_NE(Why.find("product configuration transaction unavailable"),
            std::string::npos)
      << Why;
  EXPECT_NE(Why.find("protected read borrow"), std::string::npos) << Why;
  EXPECT_FALSE(Ticket.valid);
  EXPECT_FALSE(RT->samplingProtected());
  EXPECT_EQ(RT->currentSessionSamples(), 0u)
      << "a refused sample never consumes the aggregate budget";
  EXPECT_EQ(RT->inFlight(), 0u);
  EXPECT_EQ(Provider->outstandingBorrows(), 0u);

  // A delayed borrow is not a permanent refusal: once the configuration side
  // can grant it again the same session samples normally, and the refusal did
  // not poison the runtime.
  Provider->allowBorrow();
  EXPECT_TRUE(RT->enterAdmitted({0, 0}, &Ticket, &Why)) << Why;
  EXPECT_TRUE(Ticket.valid);
  EXPECT_TRUE(RT->samplingProtected());
  EXPECT_EQ(Provider->outstandingBorrows(), 1u);
  EXPECT_EQ(RT->currentSessionSamples(), 1u);
  RT->leaveAdmitted(Ticket);
  EXPECT_EQ(RT->inFlight(), 0u);
}

/// B1/B2: a member whose own values changed after the generation was published
/// is a conflict on re-validation, so it must stay AOT while the members whose
/// values did not change keep dispatching. Samples are never treated as proof
/// that a member stays constant.
TEST_F(SmallTableRuntimeTest, RuntimeChangedMemberStaysAotOnReValidation) {
  auto M = parseAutoEntry();
  ASSERT_TRUE(M);
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  std::shared_ptr<EJitSmallTableHostProvider> Provider = hostProvider(0xE080);
  auto RT = makeRuntime(Provider);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);
  std::string Error;
  ASSERT_TRUE(static_cast<bool>(RT->prepare(*M, "a_entry", "g_auto", Dims, Error)))
      << Error;
  ASSERT_TRUE(static_cast<bool>(RT->compileCommonT1(1, Error))) << Error;

  EJitSmallTableSampleTicket Ticket;
  EXPECT_TRUE(RT->enterAdmitted({0, 0}, &Ticket, &Error)) << Error;
  EXPECT_TRUE(Ticket.valid);
  RT->leaveAdmitted(Ticket);
  EXPECT_EQ(RT->currentSessionSamples(), 1u);

  // The member's own configuration changes underneath the published generation:
  // its byCell and joint projections no longer match the published values (the
  // mode field it still satisfies).
  const uint64_t PublishedBefore = RT->stats().publishedRows;
  AutoElement Changed = g_auto[0][0];
  Changed.byCell = 99;
  Changed.joint = 999;
  g_auto[0][0] = Changed;

  std::string Why;
  EXPECT_EQ(RT->admitMember({0, 0}, &Why), EJitSmallTableAdmission::Conflict)
      << Why;
  EXPECT_EQ(RT->stats().publishedRows, PublishedBefore)
      << "a conflicting member publishes nothing";
  EXPECT_FALSE(RT->enterAdmitted({0, 0}, &Ticket, &Why))
      << "the changed member must take the AOT path";
  EXPECT_FALSE(Ticket.valid);
  EXPECT_EQ(RT->currentSessionSamples(), 1u)
      << "an AOT call does not consume sampling budget";

  // An untouched member still validates as compatible and still dispatches in
  // the same session.
  EXPECT_EQ(RT->admitMember({1, 1}, &Why), EJitSmallTableAdmission::Compatible)
      << Why;
  EXPECT_TRUE(RT->enterAdmitted({1, 1}, &Ticket, &Error)) << Error;
  EXPECT_TRUE(Ticket.valid);
  RT->leaveAdmitted(Ticket);
  EXPECT_EQ(RT->currentSessionSamples(), 2u);

  // The fixture value is restored for the tests that follow this one.
  g_auto[0][0] = AutoElement{1, 7, 2, 0};
}

/// B2 product shape: neither the planner nor the runtime is hardcoded to the
/// tiny fixture. A 16 cell x 32 TRP declared schema (the stated product ceiling
/// for this entry class) with 6 cells x 20 TRPs actually confirmed ready is
/// planned, served by ONE common T1, sampled under the DEFAULT aggregate budget
/// of 64 real admitted executions shared by every ready member, frozen once and
/// executed as a common T2 against the same table resource.
TEST_F(SmallTableRuntimeTest,
       RuntimeServesTheProductShapeDomainUnderTheDefaultAggregateBudget) {
  constexpr unsigned BigCells = 16;
  constexpr unsigned BigTrps = 32;
  SMDiagnostic Err;
  auto M = parseAssemblyString(
      autoModuleText("b_entry", "g_big", "g_big_out", BigCells, BigTrps), Err,
      Ctx);
  ASSERT_TRUE(M) << "product-shape module failed to parse";
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(BigCells, BigTrps);

  // 6 cells x 20 TRPs are confirmed ready; the rest of the schema stays AOT.
  constexpr unsigned ReadyCells = 6;
  constexpr unsigned ReadyTrps = 20;
  auto Provider = std::make_shared<EJitSmallTableHostProvider>(
      "g_big", reinterpret_cast<const void *>(&g_big[0][0]), sizeof(g_big),
      0xE060);
  for (unsigned C = 0; C < ReadyCells; ++C)
    for (unsigned T = 0; T < ReadyTrps; ++T)
      Provider->addReadyMember({C, T}, 1);

  auto RT = makeRuntime(Provider);
  ASSERT_NE(RT, nullptr);
  addProfileRuntimeHook(*RT);
  std::string Error;
  auto PlanOrErr = RT->prepare(*M, "b_entry", "g_big", Dims, Error);
  ASSERT_TRUE(static_cast<bool>(PlanOrErr)) << Error;
  const EJitSmallTablePlan *Plan = *PlanOrErr;
  ASSERT_NE(Plan, nullptr);

  // The declared schema is the product shape, not the admitted subset: each
  // field keeps exactly its own axes and the resource is sized for the schema.
  ASSERT_EQ(Plan->fields.size(), 4u);
  EXPECT_EQ(Plan->fields[1].tableRows, BigCells);
  EXPECT_EQ(Plan->fields[2].tableRows, BigTrps);
  EXPECT_EQ(Plan->fields[3].tableRows, BigCells * BigTrps);
  EXPECT_EQ(Plan->tableBytes(),
            static_cast<uint64_t>(BigCells + BigTrps + BigCells * BigTrps) *
                4u);
  const uint64_t Capacity = RT->resource()->capacityBytes();
  EXPECT_GE(Capacity, Plan->tableBytes());

  // ONE common T1 for the whole entry/code generation, bound to this
  // generation resource.
  auto T1OrErr = RT->compileCommonT1(7, Error);
  ASSERT_TRUE(static_cast<bool>(T1OrErr)) << Error;
  auto Fn = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*T1OrErr);
  ASSERT_NE(Fn, nullptr);

  // Capacity/retention accounting (spec section 8/13): only the coordinates an
  // admitted ready member projects to are published, and the payload is the
  // compact scalar columns, not a whole-structure snapshot.
  EJitSmallTableTableResource::Accounting Acc = RT->resource()->accounting();
  EXPECT_EQ(Acc.reservedBytes, Capacity);
  EXPECT_EQ(Acc.allocatedBytes, Capacity);
  EXPECT_EQ(Acc.payloadBytes, Plan->tableBytes());
  EXPECT_EQ(Acc.publishedCells,
            static_cast<uint64_t>(ReadyCells + ReadyTrps +
                                  ReadyCells * ReadyTrps));
  EXPECT_EQ(Acc.publishedBytes, Acc.publishedCells * 4u);
  EXPECT_EQ(RT->retainedBytes(), 0u)
      << "the first generation retains nothing older";

  // The confirmed policy: ONE aggregate budget of 64 real admitted sample
  // executions for the entry/code generation, shared by every ready member -
  // not 64 per member and not a representative-only quota.
  EXPECT_EQ(RT->sampleBudget(), 64u);
  for (unsigned I = 0; I < 64; ++I) {
    EJitSmallTableSampleTicket Ticket;
    ASSERT_TRUE(RT->enterAdmitted({I % ReadyCells, (I / ReadyCells) % ReadyTrps},
                                  &Ticket, &Error))
        << Error;
    ASSERT_TRUE(Ticket.valid) << "sample " << I << " must be counted";
    RT->leaveAdmitted(Ticket);
  }
  EXPECT_TRUE(RT->samplingExhausted());
  EXPECT_EQ(RT->currentSessionSamples(), 64u);
  EXPECT_EQ(RT->inFlight(), 0u);

  // Above the budget new calls take AOT; they must not execute uncounted T1
  // instrumentation and silently grow the supposedly bounded profile.
  EJitSmallTableSampleTicket Over;
  EXPECT_FALSE(RT->enterAdmitted({0, 0}, &Over, &Error));
  EXPECT_FALSE(Over.valid);
  EXPECT_FALSE(Over.counted);
  EXPECT_EQ(RT->currentSessionSamples(), 64u);

  // Freeze ONE immutable bundle and run the common T2 against the same
  // resource; every ready member must come back exactly as AOT computes it.
  auto BundleOrErr = RT->freeze(Error);
  ASSERT_TRUE(static_cast<bool>(BundleOrErr)) << Error;
  EXPECT_EQ((*BundleOrErr)->sampleCount, 64u);
  EXPECT_EQ((*BundleOrErr)->participatingMembers, 64u)
      << "the one aggregate quota is shared across 64 distinct ready members";
  EXPECT_FALSE((*BundleOrErr)->counters.empty());
  auto T2OrErr = RT->compileCommonT2(Error);
  ASSERT_TRUE(static_cast<bool>(T2OrErr)) << Error;
  auto T2 = reinterpret_cast<int32_t (*)(uint32_t, uint32_t, int32_t)>(*T2OrErr);
  ASSERT_NE(T2, nullptr);
  for (unsigned C = 0; C < ReadyCells; ++C)
    for (unsigned T = 0; T < ReadyTrps; ++T)
      for (int32_t X = -1; X <= 2; ++X)
        EXPECT_EQ(T2(C, T, X), aotAuto(g_big[C][T], X))
            << "cell=" << C << " trp=" << T << " x=" << X;
}

/// A0 regression: a client that materializes the plan into the module BEFORE
/// handing it to the engine (the supported order, and the one the runtime uses)
/// leaves the table column owned by that module's own
/// `MaterializationResponsibility` claim. The engine's transform must not claim
/// it a second time — that duplicate `defineMaterializing` definition is the
/// diagnostic the coordinator found in the A1 logs — and the compile must still
/// resolve and execute the column.
TEST_F(SmallTableTest, PreparedModuleKeepsOwnershipOfItsTableColumn) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(autoModuleText("o_entry", "g_auto", "g_auto_out",
                                              kAutoCells, kAutoTrps),
                               Err, Ctx);
  ASSERT_TRUE(M) << "ownership module failed to parse";

  EJitSmallTableRequest Req;
  Req.module = M.get();
  Req.entryName = "o_entry";
  Req.sourceVarName = "g_auto";
  SmallVector<EJitSmallTableDim, 2> Dims = autoDims(kAutoCells, kAutoTrps);
  Req.dims = Dims;
  Req.source = EJitSmallTableSource{
      reinterpret_cast<const uint8_t *>(&g_auto[0][0]), sizeof(g_auto)};
  SmallVector<EJitSmallTableRowKey, 16> Rows = autoRows(kAutoCells, kAutoTrps);
  Req.authorizedRows = Rows;
  Req.mode = EJitSmallTablePlanMode::Automatic;
  Req.readiness = testReadiness();
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(Req, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  auto Set = std::make_shared<EJitSmallTablePlanSet>();
  Set->add(std::make_shared<const EJitSmallTablePlan>(std::move(*Plan)));
  const EJitSmallTablePlan *Installed = Set->find("o_entry");
  ASSERT_NE(Installed, nullptr);

  // Client order: the module itself defines the column before JIT time.
  std::string MaterializeError;
  ASSERT_TRUE(
      EJitSmallTablePass::materialize(*M, *Installed, &MaterializeError))
      << MaterializeError;
  const GlobalVariable *Column = M->getNamedGlobal("__ejit_stab_o_entry_c1");
  ASSERT_NE(Column, nullptr);
  EXPECT_FALSE(Column->isDeclaration())
      << "a CompilerEmitted column is defined by this module";

  PeriodArrayRegistry &Registry = makeRegistry();
  std::memset(g_auto_out, 0, sizeof(g_auto_out));
  auto Engine = compileWithEngine(*M, Set, Registry, 0x57ab30, "o_entry");
  ASSERT_NE(Engine, nullptr);

  // Ownership stayed with the module's own claim: the transform skipped the
  // re-claim instead of emitting a duplicate definition.
  EXPECT_GE(Engine->getTransformClaimSkips(), 1u)
      << "a prepared module's column must not be claimed twice";

  auto Fn = lookupSealed<int32_t (*)(uint32_t, uint32_t, int32_t)>(
      *Engine, 0x57ab30, "o_entry");
  ASSERT_NE(Fn, nullptr);
  for (unsigned C = 0; C < kAutoCells; ++C)
    for (unsigned T = 0; T < kAutoTrps; ++T)
      for (int32_t X = -1; X <= 3; ++X)
        EXPECT_EQ(Fn(C, T, X), aotAuto(g_auto[C][T], X))
            << "cell=" << C << " trp=" << T << " x=" << X;

  // The column is a real, stable address inside this compile.
  auto ColOrErr = Engine->lookup(0x57ab30, "__ejit_stab_o_entry_c1");
  ASSERT_TRUE(static_cast<bool>(ColOrErr)) << toString(ColOrErr.takeError());
  ASSERT_NE(*ColOrErr, nullptr);
  auto Again = Engine->lookup(0x57ab30, "__ejit_stab_o_entry_c1");
  ASSERT_TRUE(static_cast<bool>(Again)) << toString(Again.takeError());
  EXPECT_EQ(*Again, *ColOrErr);
}

} // namespace

