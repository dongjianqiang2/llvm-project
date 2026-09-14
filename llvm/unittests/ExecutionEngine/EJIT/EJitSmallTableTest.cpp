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
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTable.h"
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
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
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
  EXPECT_FALSE(Contract.fields[1].resource.fixedAddress)
      << "the host x86-64 column stays preemptible (wantDSOLocal)";
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

} // namespace
