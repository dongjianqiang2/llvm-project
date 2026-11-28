//===-- EJitAsanState.h - EJIT-ASAN shared state blob ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The EJIT-ASAN entry instrumentation needs state that is shared across ALL
// cores: emitted entry stubs call the rotation gate and the mismatch hook
// from whichever core executes the specialized function. This header defines
// the single shared blob (gEJitAsanState), following the gEJitVpState
// discipline: EJIT_SHARED_SECTION placement, trivially default constructible,
// loader-zero-filled, no dynamic initializer, fixed-width scalars only.
//
// Only feature-guarded translation units include this header; the blob is
// defined in EJitAsan.cpp (compiled only with EJIT_ASAN=ON).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITASANSTATE_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITASANSTATE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"
#include <cstdint>
#include <type_traits>

// Rotation window: calls per bound-argument sub-window before the gate
// advances (a slot's window spans bound x window calls). Target-scoped
// definition from the EJIT_ASAN_ROTATION_WINDOW CMake cache variable; the
// #ifndef default keeps standalone consumers (host unit tests) independent
// of the board config.
#ifndef EJIT_ASAN_ROTATION_WINDOW
#define EJIT_ASAN_ROTATION_WINDOW 128
#endif
// Per-core rotation cell capacity. Cores with an id at or above this bound
// are safely degraded: their gates always report "not active".
#ifndef EJIT_ASAN_MAX_CORES
#define EJIT_ASAN_MAX_CORES 64
#endif
static_assert(EJIT_ASAN_MAX_CORES >= 1 && EJIT_ASAN_MAX_CORES <= 256,
              "EJIT_ASAN_MAX_CORES must be in [1, 256]");
// Function-slot table capacity and emitted-site budget. Slots are claimed by
// the emitting worker, never freed (dead-slot reuse only once the table is
// full); sites are monotonic in v1 (version retirement does not reclaim
// them) and a full budget skips the capture's REMAINING folds with one log
// (partial emission: already-published folds of that compile stay checked).
#ifndef EJIT_ASAN_MAX_SLOTS
#define EJIT_ASAN_MAX_SLOTS 64
#endif
#ifndef EJIT_ASAN_MAX_SITES
#define EJIT_ASAN_MAX_SITES 512
#endif
static_assert(EJIT_ASAN_ROTATION_WINDOW >= 1, "rotation window must be >= 1");
static_assert(EJIT_ASAN_ROTATION_WINDOW <= 0x10000000,
              "bound x rotation window must fit the uint32 countdown");
static_assert(EJIT_ASAN_MAX_SLOTS >= 1, "slot capacity must be >= 1");
static_assert(EJIT_ASAN_MAX_SITES >= 1, "site budget must be >= 1");

namespace llvm {
namespace ejit {

constexpr uint32_t kEJitAsanNoSite = 0xFFFFFFFFu;
constexpr uint32_t kEJitAsanNoSlot = 0xFFFFFFFFu;
// Site protocol, dispatched by the hook on the site's kind.
constexpr uint8_t kEJitAsanKindFolded = 0;
constexpr uint8_t kEJitAsanKindDrift = 1;

// One rotation cell per core. Single writer on the hot path: only the core
// that owns the cell ever writes it (the gate), so every field is a plain
// load/store resolving in that core's private L1 — no atomics, no coherence
// traffic, cache-line padded against false sharing. Other cores read cells
// only from the read-only rotation-status diagnostic.
struct alignas(64) EJitAsanCell {
  uint32_t slot;      // Slot this core is currently checking.
  uint32_t countdown; // Matching calls left in the window; 0 = advance on
                      // the next gate.
  uint32_t cycles;    // Completed full rotations (slot wrap-arounds).
  uint32_t checks;    // Entry checks served by this core; saturates ~4G.
  uint32_t bound;     // Sub-windows of the armed window; snapshotted at
                      // arming, a later registration waits for the next.
};

// A function slot in the rotation. Claimed and written only by the emitting
// worker (single writer); the gate on other cores reads the slot count only.
struct EJitAsanSlot {
  char name[128];
  EJitAtomicU32 activeSites; // Sites currently published into this slot.
  EJitAtomicU32 boundCount;  // Monotonic max of sub-windows the slot's
                            // stubs check; 0 = none registered yet.
};

// One emitted entry-check site: one (specialization version, field check)
// pair. Written by the worker at emit time and published with storeRelease;
// the mismatch hook on any core consumes it with loadAcquire.
struct EJitAsanSite {
  char function[128];
  char period[32];
  EJitAtomicU32 published; // 0 = free, 1 = fully written and live.
  uint32_t gen;            // Version generation, backfilled by the driver.
  uint32_t slot;
  uint8_t kind;      // kEJitAsanKindFolded: compare the baked constant;
                     // kEJitAsanKindDrift: observe against the baseline.
  uint8_t tier;      // CompileTier as a plain byte.
  uint8_t argNo;     // Bound-pointer argument index.
  uint8_t width;     // Field bit width: 1/8/16/32/64.
  uint8_t fieldAlign; // Field alignment in bytes.
  uint64_t byteOffset; // Constant offset within the pointee.
  uint64_t expected;  // Folded: the baked constant. Drift: the observed
                      // compile-time baseline.
};

// Sticky record for one site. For a folded site this is the first-mismatch
// record (kind=entry rows); a drift site reuses the same freeze protocol
// for the first CHANGED value while lastSeen tracks the most recent
// observation. No samples counter on the folded path — counting every
// executed check would put an atomic in the hot stub; the per-core cell's
// `checks` covers the volume.
struct EJitAsanSticky {
  EJitAtomicU32 recorded;     // 0 = never recorded, 1 = first value frozen.
  EJitAtomicU32 count;        // Folded mismatches, saturating at 0xFFFF.
  EJitAtomicU32 driftChanges; // Drift value transfers, saturating ~4G.
  EJitAtomicU32 samples;      // Drift observations, saturating ~4G.
  uint64_t firstValue;
  uint64_t firstArg;
  EJitAtomicU64 lastSeen;     // Drift baseline, seeded at publish.
};

struct EJitAsanState {
  EJitAsanCell cells[EJIT_ASAN_MAX_CORES];
  EJitAsanSlot slots[EJIT_ASAN_MAX_SLOTS];
  EJitAtomicU32 slotsUsed;   // Monotonic claimed-slot count (single writer).
  EJitAtomicU32 siteCounter; // Next monotonic site id (single writer).
  EJitAsanSite sites[EJIT_ASAN_MAX_SITES];
  EJitAsanSticky sticky[EJIT_ASAN_MAX_SITES];
};

// The blob must be a real implicit-lifetime object: loader-zero-filled, no
// .init_array, only atomically published flags guard visibility (mirror the
// gEJitVpState static_asserts).
static_assert(std::is_trivially_default_constructible_v<EJitAsanState>,
              "gEJitAsanState must be loader-zero-filled");
static_assert(std::is_standard_layout_v<EJitAsanState>,
              "gEJitAsanState must have a stable shared-memory layout");
static_assert(std::is_trivially_destructible_v<EJitAsanState>,
              "gEJitAsanState must never run a destructor");
// The blob lands in the inter-core shared window (.mc_shared on the board);
// keep its footprint visible at compile time and shrink it via the
// EJIT_ASAN_* knobs rather than silently growing past the budget.
static_assert(sizeof(EJitAsanState) <= 256 * 1024,
              "gEJitAsanState exceeds the documented 256KB shared budget");

// The single process-global blob. EJIT_SHARED_SECTION places it in
// inter-core shared memory on real multi-core builds; on host it is ordinary
// .bss. Defined in EJitAsan.cpp.
extern EJIT_SHARED_SECTION EJitAsanState gEJitAsanState;

// Copy a name into a fixed-capacity field, truncating with a terminator.
inline void ejitAsanCopyName(char *Dst, size_t Cap, StringRef Src) {
  size_t N = Src.size() < Cap - 1 ? Src.size() : Cap - 1;
  for (size_t I = 0; I < N; ++I)
    Dst[I] = Src[I];
  Dst[N] = '\0';
}

// Find-or-claim the rotation slot for a function (single writer: the
// emitting worker). Returns kEJitAsanNoSlot when the table is full of live
// slots and the caller must skip instrumenting this version.
uint32_t ejitAsanClaimSlot(StringRef FunctionName);

// Monotonic running max of the slot's sub-window count; the gate snapshots
// it when arming a window.
void ejitAsanSetSlotBound(uint32_t Slot, uint32_t Bound);

// Publish one emitted site and return its id. Fields must be fully known;
// the id is what the emitted stub embeds. Kind selects the hook protocol
// and seeds the drift baseline from Expected. Returns kEJitAsanNoSite when
// the site budget is exhausted (the caller logs and skips the fold).
uint32_t ejitAsanPublishSite(uint32_t Slot, StringRef Function,
                             StringRef Period, uint8_t Kind, uint8_t Tier,
                             uint8_t ArgNo, uint8_t Width, uint8_t FieldAlign,
                             uint64_t ByteOffset, uint64_t Expected);

} // namespace ejit
} // namespace llvm

// Entry-stub hooks, implemented in EJitAsan.cpp. EJit::init registers their
// addresses with the compile driver (like the value-profiling hooks) so the
// emitted stubs resolve them through the absolute-symbol path. The gate
// returns the bound-argument sub-window index to check (0..bound-1) or -1
// when this core is not holding the window for the caller's slot.
extern "C" int32_t __ejit_asan_gate(uint32_t Slot);
extern "C" void __ejit_asan_mismatch(uint32_t SiteId, const void *Arg,
                                     uint64_t ValueSeen);

#endif // LLVM_LIB_EXECUTIONENGINE_EJIT_EJITASANSTATE_H
