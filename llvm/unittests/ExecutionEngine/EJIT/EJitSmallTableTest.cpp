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
    // Outputs are process-wide; a previous test's stores must not leak into
    // this test's address checks.
    std::memset(g_out, 0, sizeof(g_out));
  }

  std::shared_ptr<const EJitSmallTablePlanSet>
  makePlanSet(const Module &M,
              ArrayRef<std::optional<uint64_t>> Contracts = {},
              ArrayRef<EJitSmallTableRowKey> Rows = {}) {
    std::string Error;
    SmallVector<EJitSmallTableRowKey, 128> DefaultRows = allRows();
    ArrayRef<EJitSmallTableRowKey> UseRows =
        Rows.empty() ? ArrayRef<EJitSmallTableRowKey>(DefaultRows) : Rows;
    auto Plan = EJitSmallTablePlanner::plan(
        M, "stab_entry", "g_cfg", planDims(),
        EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]),
                             sizeof(g_cfg)},
        UseRows, Contracts, Error);
    EXPECT_TRUE(Plan.has_value()) << Error;
    auto Set = std::make_shared<EJitSmallTablePlanSet>();
    if (Plan)
      Set->add(std::make_shared<const EJitSmallTablePlan>(std::move(*Plan)));
    return Set;
  }

  /// The scalar-width fixture's plan: two argument axes, one fully ready row per
  /// (cell, phase).
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
    auto Plan = EJitSmallTablePlanner::plan(
        M, "w_entry", "g_width", Dims,
        EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_width[0][0]),
                             sizeof(g_width)},
        WRows, {}, Error);
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

  // Condensation: the emitted payload is three scalar columns over 120 rows
  // instead of the 120 x 1 KiB source elements, and it is bounded well below
  // the source region the plan was built from.
  uint64_t TableBytes = 0;
  for (const EJitSmallTableField &Field : Plan->fields)
    TableBytes += Plan->numRows() * Field.accessSize;
  EXPECT_EQ(TableBytes, kCells * kTrps * kPhases * (4 + 4 + 4));
  EXPECT_LT(TableBytes, sizeof(g_cfg) / 8);
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

/// Equality across every visible row is not a contract. Without an explicit
/// admission contract the planner must keep the column and never fold.
TEST_F(SmallTableTest, UniformContractIsNeverInferredFromEqualVisibleRows) {
  auto M = parseModule();
  ASSERT_TRUE(M);
  // Rows (0,0,1..3): all four visible `mode` values are 1 in fillConfig(), so a
  // naive "they all agree" inference would fold mode to 1.
  SmallVector<EJitSmallTableRowKey, 4> Rows = {{{0, 0, 1}}, {{0, 0, 2}},
                                               {{0, 0, 3}}};
  std::string Error;
  auto Plan = EJitSmallTablePlanner::plan(
      *M, "stab_entry", "g_cfg", planDims(),
      EJitSmallTableSource{reinterpret_cast<const uint8_t *>(&g_cfg[0][0][0]),
                           sizeof(g_cfg)},
      Rows, {}, Error);
  ASSERT_TRUE(Plan.has_value()) << Error;
  EXPECT_EQ(Plan->uniformFieldCount(), 0u);
  for (const EJitSmallTableField &Field : Plan->fields)
    EXPECT_FALSE(Field.uniformValue.has_value());
  for (const EJitSmallTableField &Field : Plan->fields)
    EXPECT_FALSE(Field.columnName.empty());

  // Lowering that partial plan is refused; the point of this test is the
  // planner-level refusal to infer a contract, which is checked above.
  Error.clear();
  EXPECT_FALSE(EJitSmallTablePass::materialize(*M, *Plan, &Error));
  EXPECT_NE(Error.find("rows ready"), std::string::npos) << Error;

  // On a complete plan with no contract, every field still stays a real table
  // column: no equality inference happens anywhere in the pass.
  auto FullM = parseModule();
  ASSERT_TRUE(FullM);
  auto FullSet = makePlanSet(*FullM);
  const EJitSmallTablePlan *Full = FullSet->find("stab_entry");
  ASSERT_NE(Full, nullptr);
  EXPECT_EQ(Full->uniformFieldCount(), 0u);
  ASSERT_TRUE(EJitSmallTablePass::materialize(*FullM, *Full, &Error)) << Error;
  EJitSmallTablePass Pass(*Full);
  FunctionAnalysisManager FAM;
  Pass.run(*FullM->getFunction("stab_entry"), FAM);
  EXPECT_EQ(Pass.getStats().uniformFolded, 0u);
  EXPECT_EQ(Pass.getStats().tableReplaced, 3u);
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
  auto Set = makePlanSet(*PlanM);
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

} // namespace
