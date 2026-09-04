//===-- EJitSharedTaskPool.cpp - Cross-core shared single-worker facade ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Implements owner election and the producer/consumer paths over the POD
//  EJitSharedTaskPoolState. Uses ONLY EJitAtomic (acquire/release) — no
//  std::thread / std::mutex / std::condition_variable, no STL containers in the
//  shared data path. The switch/dedup/queue/commit-gate logic mirrors the
//  single-instance EJitTaskPool, re-expressed against shared POD storage; the
//  result cache is a fixed-capacity POD table (no std::unordered_map can live
//  in shared memory).
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitStats.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

// Compile-time guard: if EJIT_ICACHE_FUNC_SLOTS ever falls below
// EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX (defined in EJitSharedTaskPoolState.h,
// included above; kEJitMaxFuncIndex mirrors it), funcIndex >=
// EJIT_ICACHE_FUNC_SLOTS silently drops icacheFill and the inline cache misses
// for those functions. Caught at LLVMEJIT compile time in every build (debug
// AND release - static_assert is a C++11 compile-time check, not a runtime
// macro). Compare the two macros directly so this TU keeps its intentionally
// light include set (the unit test target compiles it with LLVMSupport only).
static_assert(
    EJIT_ICACHE_FUNC_SLOTS >= EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX,
    "EJIT_ICACHE_FUNC_SLOTS must be >= EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX; "
    "otherwise icacheFill silently drops high-funcIndex entries.");

// icacheLinearize indexes with shifts (idx = idx * D + instanceId), so D must
// be a power of 2 — a non-pow2 D would corrupt the row-major stride silently.
static_assert((EJIT_ICACHE_DIM_SIZE & (EJIT_ICACHE_DIM_SIZE - 1)) == 0,
              "EJIT_ICACHE_DIM_SIZE must be a power of 2 "
              "(the icache hit path uses shift-based indexing).");

// The AOT wrapper emits [D]^numDims slot arrays up to EJIT_ICACHE_MAX_DIMS;
// the shared cache identity stores dims up to kEJitSharedMaxDims. A drift
// between the two would let the AOT emit 5+ dim wrappers whose identity the
// runtime truncates — silently serving the wrong specialization.
static_assert(EJIT_ICACHE_MAX_DIMS == llvm::ejit::kEJitSharedMaxDims,
              "EJIT_ICACHE_MAX_DIMS (AOT wrapper dim cap) must equal "
              "kEJitSharedMaxDims (shared cache identity cap).");

using namespace llvm;
using namespace llvm::ejit;

#ifndef EJIT_SRE_TASKPOOL_WORKER_THROTTLE_MULT
#define EJIT_SRE_TASKPOOL_WORKER_THROTTLE_MULT 1u
#endif

#ifndef EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS
#define EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS 100u
#endif

namespace {

// Compiler reordering barrier used as a portable idle relax (no platform
// symbol, no arch-specific instruction in this layer).
inline void cpuRelax() { __asm__ __volatile__("" ::: "memory"); }

// Bound pointers are transport-only. Once the compile callback has returned,
// no owner-private publication/retry state should retain or expose them.
EJitCompileRequest requestForPublication(const EJitCompileRequest &Req) {
  EJitCompileRequest Published = Req;
  Published.boundCount = 0;
  for (EJitBoundPtrDescriptor &Bound : Published.boundPointers)
    Bound = {};
  return Published;
}

// Fixed worker core (build policy, spec §11.4): CMake
// EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE pins the single shared worker to ONE
// designated core. When set, ONLY that core may win the owner election - it
// alone runs the Uninitialized->Initializing CAS, builds the engine, and
// creates the worker task (SRE tasks run on the core that creates them).
// Every other core, on observing Uninitialized, does NOT attempt the CAS; it
// waits for the designated core with the same bounded yield protocol as the
// Initializing case, then attaches as a peer. kEJitInvalidCoreId means "no
// fixed core": the open election, byte-for-byte unchanged.
#ifdef EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE
static_assert(EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE != kEJitInvalidCoreId,
              "the fixed worker core id collides with the kEJitInvalidCoreId "
              "sentinel (CMake rejects this; this catches a raw -D compile)");
constexpr uint32_t kEJitFixedWorkerCore = EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE;
#else
constexpr uint32_t kEJitFixedWorkerCore = kEJitInvalidCoreId;
#endif

//===----------------------------------------------------------------------===//
// Inline per-bucket reader/writer lock over the two POD words (same protocol as
// EJitRwLock §3.2, but operating on shared-blob fields directly).
//
// EJIT_SRE_TASKPOOL_NO_RECLAIM changes the READER discipline to a load-only
// seqlock (no per-hit RMW on the shared readers line). This is memory-safe ONLY
// because in that build a published fnPtr is never physically freed (the code
// pool never reclaims; the taskpool releaser is never installed), so a hit that
// hands back a pointer without a read token can never dangle. The writer still
// takes writeFlag for writer/writer exclusion and additionally bumps a
// monotonic per-bucket publishSeq (odd while writing, even when done) that the
// reader uses to detect and discard a read that raced a publish. The default
// (token) build is unchanged: publishSeq is never touched.
//===----------------------------------------------------------------------===//
bool bucketTryRead(EJitSharedCacheBucket &b) {
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // Load-only: never touch the shared readers line. The seqlock validity check
  // (bucketSeqBegin/bucketSeqStable) provides consistency; here we only refuse
  // to start reading while a writer is mid-publish.
  return b.writeFlag.loadAcquire() == 0;
#else
  if (b.writeFlag.loadAcquire() != 0)
    return false;
  b.readers.fetchAdd(1);
  if (b.writeFlag.loadAcquire() != 0) {
    b.readers.fetchSub(1);
    return false;
  }
  return true;
#endif
}
void bucketReadRelease(EJitSharedCacheBucket &b) {
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  (void)b; // no token was taken.
#else
  b.readers.fetchSub(1);
#endif
}
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
// Seqlock begin: snapshot the publish sequence. Returns false if a publish is
// in progress (odd), so the caller cleanly falls back rather than reading a
// slot mid-overwrite.
static inline bool bucketSeqBegin(EJitSharedCacheBucket &b, uint32_t &seq) {
  seq = b.publishSeq.loadAcquire();
  return (seq & 1u) == 0;
}
// Seqlock end: the read is consistent iff the sequence is unchanged (no publish
// to this bucket happened during the scan + fnPtr load). The compiler barrier
// keeps the guarded field loads from being reordered across this check.
static inline bool bucketSeqStable(EJitSharedCacheBucket &b, uint32_t seq0) {
  asm volatile("" ::: "memory");
  return b.publishSeq.loadAcquire() == seq0;
}
#endif
void bucketWrite(EJitSharedCacheBucket &b, bool pgoClearExclusive = false) {
  // PGO (§6 shared trigger): after a cacheLookupSeq (seqlock read) on this
  // bucket, the writeFlag CAS loop below can livelock on aarch64: the prior
  // acquire loads in the seqlock read leave the subsequent CAS unable to make
  // forward progress with the target's exclusive monitor sequence. The PGO
  // path therefore uses an atomic exchange for the writer word in NO_RECLAIM.
  // Unlike the former direct store, exchange remains an indivisible lock
  // acquisition and preserves writer/writer exclusion. Its target lowering and
  // forward progress are platform properties that must be checked on the SRE
  // board; readers remain load-only in this build, so no reader drain is needed
  // after the writer is acquired.
  if (pgoClearExclusive) {
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
    while (b.writeFlag.exchange(1) != 0)
      cpuRelax();
    b.publishSeq.fetchAdd(1);
    return;
#endif
    // (non-NO_RECLAIM: fall through to the normal CAS path below)
  }
  uint32_t expected = 0;
  while (!b.writeFlag.compareExchange(expected, 1))
    expected = 0;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  b.publishSeq.fetchAdd(1);
#endif
  while (b.readers.loadAcquire() != 0)
    cpuRelax();
}
void bucketWriteRelease(EJitSharedCacheBucket &b) {
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // Leave the even (stable) phase after all slot fields are published.
  b.publishSeq.fetchAdd(1);
#endif
  b.writeFlag.storeRelease(0);
}

constexpr uint32_t kReady = static_cast<uint32_t>(EJitSharedInitState::Ready);

/// Mixing constant shared by hashIdentity() and every unrolled fixed-dimension
/// lookup (cacheLookupNd / cacheLookupSeqNd). Kept here so the NO_RECLAIM
/// fixed-dimension seqlock specializations defined above the generic
/// cacheLookupNd block can also reference it.
constexpr uint64_t kHashMul = 0x9e3779b97f4a7c15ULL;

inline void markPostPublishSeen(EJitSharedCacheSlot &Slot) {
#ifdef EJIT_STATS_ENABLE
  if (Slot.postPublishSeen.loadRelaxed() != 0)
    return;
  Slot.postPublishSeen.storeRelaxed(1);
#else
  (void)Slot;
#endif
}

// Per-function inline-cache slot registry (multi-version direct-indexed). Each
// entry records the wrapper's per-function @__ejit_icache_fn_<name> global base
// (a uintptr_t cell, or a [D]^numDims array of them for a multi-version entry)
// plus its dimensionality, registered by name at ejit_auto_register /
// .ejit_period time via ejit_register_icache_slot (which calls
// ejitIcacheRegisterSlot). The wrapper reads the cell directly (a GEP by the
// ejit_dim arg values + one load + null-check + indirect call) with NO
// ejit_icache_try call and NO per-call guards. icacheFill writes the
// specialization pointer through the cell at [i0][i1]... (linearized from dims)
// on a successful resolve; icacheDrainAll writes every cell back to its empty
// value on a period toggle (the &MissFn sentinel for sentinel-form tables, 0
// for guarded ones); icacheTry (test/diagnostic only) reads them.
//
// The REGISTRY is core-private (default BSS); the CELLS it points at are not.
// With EJIT_ICACHE_SECTION set @__ejit_icache_fn_<name> is one shared object,
// so every core registers the same base and a drain on any core reaches every
// core's partition. These bookkeeping words are rebuilt identically at each
// core's own registration and never read cross-core.
//
// Cell accesses therefore go through the atomic wrapper -- a fill on one core
// can race a drain on another. Relaxed is the right order (single-location
// coherence on one naturally-aligned word) and lowers to the same ldr/str on
// AArch64. The AOT probe's load is likewise relaxed, so the cache is race-free
// in the C++ model at zero code-size cost.
struct EJitIcacheSlotReg {
  uintptr_t *base;
  uint32_t numDims;
  // Non-null for SENTINEL-form slots: the wrapper's cell table is defined
  // pre-filled with this MissFn pointer and the probe BLRs the cell without a
  // null guard, so every value ever stored into a cell must be callable.
  // icacheDrainAll and icacheFill's retract write this back instead of 0.
  // Null for guarded slots (3D/4D, timing): their empty value stays 0.
  const void *missFn;
};
EJitIcacheSlotReg gIcacheSlots[EJIT_ICACHE_FUNC_SLOTS];

// A cell viewed as the atomic it is: EJitAtomic<uintptr_t> is a standard-layout
// wrapper over exactly one pointer-sized word, so this overlay on the AOT array
// is layout-identical and only changes which builtin performs the access.
//
// Formally this is a strict-aliasing violation -- the AOT global is declared as
// an array of pointer-sized words, not of EJitAtomicUPtr -- so it rests on the
// compiler assumption every other shared-memory overlay here rests on: the
// access is through a standard-layout type of identical size and alignment, and
// __atomic_* builtins lower to plain LDR/STR on the same address the wrapper
// would have used. LLVM and GCC both accept this; a compiler that did not would
// break the shared blob overlays long before it broke this one. Kept as an
// overlay rather than changing the AOT type because the wrapper's global must
// stay a plain [D]^numDims array of pointers for the probe's GEP.
inline EJitAtomicUPtr &icacheCell(uintptr_t *base, uintptr_t idx) {
  return reinterpret_cast<EJitAtomicUPtr *>(base)[idx];
}

// Cells in a [D]^numDims array. numDims is capped at registration.
inline uintptr_t icacheCellCount(uint32_t numDims) {
  uintptr_t n = 1;
  for (uint32_t i = 0; i < numDims; ++i)
    n *= EJIT_ICACHE_DIM_SIZE;
  return n;
}

// True when every instance id fits the table's per-dim bound. The taskpool
// accepts ids up to MAX_INSTANCES (256) and a period array up to
// MAX_PERIOD_ARR_SIZE (100), against a D of 16, so an ordinary application can
// present an id with no cell; indexing anyway walks off the wrapper's global.
// EJitWrapperGen emits no probe unless it can prove the bound.
static bool icacheDimsInRange(const EJitDimPair *dims, uint32_t numDims) {
  for (uint32_t i = 0; i < numDims; ++i)
    if (dims[i].instanceId >= EJIT_ICACHE_DIM_SIZE)
      return false;
  return true;
}

// Linearize the dim identity to a flat cell index, row-major, dim0 = leftmost
// ejit_dim param (MUST match the AOT [D]^numDims array declaration order). D is
// EJIT_ICACHE_DIM_SIZE (power-of-2). numDims=0 -> idx 0 (the scalar cell).
// Callers must have passed icacheDimsInRange first.
static uintptr_t icacheLinearize(const EJitDimPair *dims, uint32_t numDims) {
  uintptr_t idx = 0;
  for (uint32_t i = 0; i < numDims; ++i)
    idx = idx * EJIT_ICACHE_DIM_SIZE + dims[i].instanceId;
  return idx;
}

} // namespace

EJitIcacheRegResult llvm::ejit::ejitIcacheRegisterSlot(
    uint32_t funcIndex, void *base, uint32_t numDims, const void *missFn) {
  if (!base)
    return EJitIcacheRegResult::Invalid;
  // numDims sizes the array the drain walks, so an out-of-cap value writes far
  // past the wrapper's global. The AOT pass never emits one, but numDims also
  // arrives from a uint64 registry field and the public C ABI -- don't trust
  // it.
  if (numDims > EJIT_ICACHE_MAX_DIMS)
    return EJitIcacheRegResult::Invalid;
  // Checked LAST so malformed data is still reported as malformed. Running out
  // of slots while the function registry holds 4096 is expected and degrades.
  if (funcIndex >= EJIT_ICACHE_FUNC_SLOTS)
    return EJitIcacheRegResult::CapacityMiss;
  gIcacheSlots[funcIndex].base = reinterpret_cast<uintptr_t *>(base);
  gIcacheSlots[funcIndex].numDims = numDims;
  gIcacheSlots[funcIndex].missFn = missFn;
  // Sentinel deployment check: a branchless probe BLRs whatever its cell
  // holds, so a table whose definition-time &MissFn initializer never made it
  // into the placed image -- a linker script that maps the shared section
  // NOLOAD/zero instead of carrying its initialized data -- crashes on the
  // function's first call, and nothing downstream can catch that. Compare
  // cell[0] (the scalar, or [0]...[0] of the splat) against the registered
  // sentinel and say it loudly here, where the mis-deployment is still
  // attributable. Bounded output: once per function per core.
  if (missFn && icacheCell(reinterpret_cast<uintptr_t *>(base), 0)
                       .loadRelaxed() !=
                   reinterpret_cast<uintptr_t>(missFn)) {
    EJIT_DIAG("icache slot %u: sentinel-form cell[0]=%p != &MissFn=%p -- the "
              "image did not place the table's initializer (shared section "
              "mapped NOLOAD/zero?); the branchless wrapper will crash on "
              "the first call to this function",
              funcIndex,
              (void *)icacheCell(reinterpret_cast<uintptr_t *>(base), 0)
                  .loadRelaxed(),
              missFn);
  }
  return EJitIcacheRegResult::Ok;
}

void llvm::ejit::ejitIcacheClearAll() {
  // Unregister every slot: nulling base makes all probes miss
  // (icacheTry/icacheFill see no base and bail), which is the "empty" state. We
  // do NOT dereference the base pointers here: in tests the slots are stack
  // locals that may already be destroyed by the time a later test clears (e.g.
  // if an ASSERT returned early and skipped the prior test's end-clear), so
  // dereferencing would be a use-after-free -- which is why this stays distinct
  // from icacheDrainAll(), which DOES write through every base. Production
  // never calls this.
  for (uint32_t f = 0; f < EJIT_ICACHE_FUNC_SLOTS; ++f) {
    gIcacheSlots[f].base = nullptr;
    gIcacheSlots[f].numDims = 0;
    gIcacheSlots[f].missFn = nullptr;
  }
}

void llvm::ejit::ejitDumpIcacheSlots(const EJitModuleLoader *loader) {
  EJIT_DIAG_RAW("=== gIcacheSlots dump (%u slots) ===",
                (unsigned)EJIT_ICACHE_FUNC_SLOTS);
  uint32_t registered = 0;
  uint32_t filled = 0;
  for (uint32_t f = 0; f < EJIT_ICACHE_FUNC_SLOTS; ++f) {
    EJitIcacheSlotReg &reg = gIcacheSlots[f];
    if (!reg.base)
      continue;
    registered++;
    // For multi-dim arrays, cell[0] is the [0]...[0] element; for 0-dim,
    // it is the scalar cell. Either way it tells us whether the first
    // identity has been resolved yet. A sentinel-form table's empty value is
    // &MissFn, so "filled" must exclude it explicitly.
    const uintptr_t empty = reinterpret_cast<uintptr_t>(reg.missFn);
    const uintptr_t c0 = icacheCell(reg.base, 0).loadRelaxed();
    if (c0 != 0 && c0 != empty)
      filled++;
    // The slot index IS the funcIndex (a static_assert above guarantees
    // EJIT_ICACHE_FUNC_SLOTS >= EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX), so a
    // loader resolves the function name: "<unknown>" when the registry
    // holds no name for it, "?" when no loader was handed in (unit tests /
    // runtime not yet initialized).
    const char *name = "?";
    if (loader) {
      const std::string &s = loader->getFuncNameByFuncIdx(f);
      name = s.empty() ? "<unknown>" : s.c_str();
    }
    (void)name; // consumed by EJIT_DIAG_RAW only; silence DIAG-off builds
    const char *state =
        c0 == 0 ? "(empty)" : c0 == empty ? "(sentinel)" : "(filled)";
    EJIT_DIAG_RAW("  [%2u] %.24s base=%p numDims=%u cells=%u cell[0]=%p %s",
                  f, name, (void *)reg.base, reg.numDims,
                  (unsigned)icacheCellCount(reg.numDims), (void *)c0, state);
    ejitDiagPrintThrottle();
  }
  EJIT_DIAG_RAW("=== icache slots: %u registered, %u with cell[0] filled ===",
                registered, filled);
  // Both counters are consumed by the RAW macro above only; silence
  // DIAG-off builds (same for `name` inside the loop).
  (void)registered;
  (void)filled;
}

void EJitSharedTaskPool::icacheDrainAll(const char *reason) {
  if (!state_) {
    EJIT_DIAG("icacheDrain SKIP (%s): no shared state bound",
              reason ? reason : "unspecified");
    return;
  }
  // Stamp the drain with the generation it belongs to. setInstanceEnabled()
  // does not require Ready, so a peer drain can still be walking cells when the
  // owner shuts down and a new owner claims the blob. That (re)initialization
  // discards the in-flight count wholesale, so a straggler must NOT decrement
  // afterwards: its increment is already gone, and subtracting again wraps the
  // counter to UINT32_MAX -- after which icacheBeginResolve() refuses every
  // token forever and no later drain can repair it, since each one is +1 then
  // -1.
  const uint32_t gen = state_->generation.loadAcquire();
  // Announce the drain BEFORE touching a cell and retire the sequence AFTER the
  // last one. Together these bracket the whole window: a fill can only be
  // accepted if it saw drainsInFlight == 0 at both ends AND an unchanged
  // sequence, which is possible only when no drain overlapped its resolve.
  // Announcing first is what stops a fill from slipping into a cell this drain
  // has already passed but has not yet accounted for.
  state_->icacheDrainsInFlight.fetchAdd(1);
  // Nothing has been cached since the last drain emptied the table, so there is
  // nothing to zero. Skip the walk -- but still bump the sequence below,
  // because a resolve in flight right now must still be told a drain happened.
  //
  // This is the bring-up case and it dominates: N cores x one ejit_activate per
  // instance, every one of them walking every cell of every registered slot to
  // write 0 over 0. Correctness does not depend on the flag; see icacheArmed.
  const bool armed = state_->icacheArmed.loadAcquire() != 0;
  // Counted only under EJIT_DIAG_ENABLE: reading every cell back to see whether
  // it held anything doubles the drain's memory traffic, and with diagnostics
  // off EJIT_DIAG discards its arguments unevaluated, so these would be dead.
#ifdef EJIT_DIAG_ENABLE
  uint32_t walkedSlots = 0;
  uint64_t clearedCells = 0;
#endif
  for (uint32_t f = 0; armed && f < EJIT_ICACHE_FUNC_SLOTS; ++f) {
    const EJitIcacheSlotReg &reg = gIcacheSlots[f];
    if (!reg.base)
      continue;
    if (reg.numDims > EJIT_ICACHE_MAX_DIMS)
      continue; // defence in depth: never walk past a mis-sized array
    // Sentinel-form slots drain to &MissFn (their branchless probe BLRs the
    // cell unconditionally, so 0 would be a crash, not a miss); guarded slots
    // keep the historical 0.
    const uintptr_t empty = reinterpret_cast<uintptr_t>(reg.missFn);
    const uintptr_t cells = icacheCellCount(reg.numDims);
#ifdef EJIT_DIAG_ENABLE
    ++walkedSlots;
#endif
    for (uintptr_t c = 0; c < cells; ++c) {
#ifdef EJIT_DIAG_ENABLE
      if (icacheCell(reg.base, c).loadRelaxed() != empty)
        ++clearedCells;
#endif
      icacheCell(reg.base, c).storeRelaxed(empty);
    }
  }
  // Only after a real walk: a skip cleared nothing, and clearing the flag it
  // just read as 0 would be a no-op anyway. A fill that armed DURING the walk
  // retracts itself (a drain was in flight for its whole store), so dropping
  // the flag here cannot strand a cell.
  if (armed)
    state_->icacheArmed.storeRelease(0);
  const uint32_t seq = state_->icacheDrainSeq.fetchAdd(1) + 1u;
  // Zeroing cells of a generation we no longer belong to is harmless -- an
  // empty cell only costs a miss -- but the accounting is not.
  ejitIcacheRetireDrain(state_, gen);
  // A drain that actually emptied a cell is the event worth seeing on a board:
  // it is what explains a hot core suddenly missing. A drain that cleared
  // NOTHING invalidated nothing, and at bring-up those dominate -- N cores each
  // calling ejit_activate per instance, every one walking a table that is still
  // cold. Logging those at INFO buries the run in lines reporting a non-event,
  // so they go to VERBOSE; the sequence still moves, so a fill retraction can
  // still be traced by raising the level.
  //
  // `gen` is in the line because a drain that started under a generation the
  // blob has since left retires nothing, and that is otherwise invisible.
#ifdef EJIT_DIAG_ENABLE
  if (clearedCells != 0) {
    EJIT_DIAG("icacheDrain OK (%s): core=%u gen=%u slots=%u cellsCleared=%llu "
              "seq=%u",
              reason ? reason : "unspecified", EJitCoreId::current(), gen,
              walkedSlots, static_cast<unsigned long long>(clearedCells), seq);
  } else {
    EJIT_DIAG_VERBOSE("icacheDrain OK (%s): core=%u gen=%u slots=%u "
                      "cellsCleared=0 seq=%u (nothing was cached)",
                      reason ? reason : "unspecified", EJitCoreId::current(),
                      gen, walkedSlots, seq);
  }
#else
  (void)reason;
  (void)gen;
  (void)seq;
#endif
}

void llvm::ejit::ejitIcacheRetireDrain(EJitSharedTaskPoolState *st,
                                       uint32_t gen) {
  if (!st)
    return;
  // Our increment was discarded with the previous generation's count.
  if (st->generation.loadAcquire() != gen) {
    EJIT_DIAG("icacheDrain retire SKIP: started under gen=%u, blob is now "
              "gen=%u -- the re-init already discarded this increment",
              gen, st->generation.loadAcquire());
    return;
  }
  // Saturating even so: the generation check closes the interleaving this
  // guards against, and this closes any other.
  uint32_t cur = st->icacheDrainsInFlight.loadAcquire();
  while (cur != 0 && !st->icacheDrainsInFlight.compareExchange(cur, cur - 1))
    ;
}

uint32_t EJitSharedTaskPool::icacheDrainSeq() const {
  return state_ ? state_->icacheDrainSeq.loadAcquire() : 0;
}

uint64_t EJitSharedTaskPool::icacheBeginResolve() {
  if (!state_)
    return kEJitIcacheNoResolve;
  const uint32_t seq = state_->icacheDrainSeq.loadAcquire();
  // A drain already running has an unknown reach: it may have passed this
  // identity's cell or not. Refuse the token rather than guess.
  if (state_->icacheDrainsInFlight.loadAcquire() != 0)
    return kEJitIcacheNoResolve;
  return kEJitIcacheResolveValid | seq;
}

//===----------------------------------------------------------------------===//
// Switch controller helpers (§5.1) over shared arrays.
//===----------------------------------------------------------------------===//
bool EJitSharedTaskPool::isInstanceEnabled(uint32_t dimType,
                                           uint32_t instanceId) const {
  if (dimType >= kEJitSharedDimTypes || instanceId >= kEJitSharedInstances)
    return false;
  // An ejit_const_dim has no lifecycle to activate. initSharedStorage zeroes
  // every enabled bit, so without this exemption a const dim would be gated off
  // permanently in a shared-taskpool build and never compile.
  if (dimType == kEJitSharedConstDimType)
    return true;
  return state_->enabled[dimType][instanceId].loadRelaxed() != 0;
}

bool EJitSharedTaskPool::isInstanceActive(uint32_t dimType,
                                          uint32_t instanceId) const {
  // Public read counterpart of setInstanceEnabled. The compile gate
  // (compileCold) and ejit_is_active consult THIS shared bit, not an
  // owner-private copy, so producer (activate) and worker (compile) see the
  // same cross-core fact.
  return isInstanceEnabled(dimType, instanceId);
}

uint32_t EJitSharedTaskPool::instanceVersion(uint32_t dimType,
                                             uint32_t instanceId) const {
  if (dimType >= kEJitSharedDimTypes || instanceId >= kEJitSharedInstances)
    return 0;
  // Pinned at 0: a const dim contributes nothing to the compile checkpoints.
  if (dimType == kEJitSharedConstDimType)
    return 0;
  return state_->version[dimType][instanceId].loadAcquire();
}

//===----------------------------------------------------------------------===//
// Per-function inline cache (multi-version direct-indexed).
//
// The production hit path does NOT call icacheTry: the ejit_entry wrapper reads
// the shared @__ejit_icache_fn_<name> table directly (GEP into the [D]^numDims
// array by the ejit_dim arg values + one load + null check + indirect call, no
// call, no per-call guards). icacheTry is retained for unit tests and
// diagnostics. icacheFill writes the specialization pointer through the cell at
// [i0][i1]... (linearized from dims) on a successful resolve, and
// icacheDrainAll zeroes every cell on a period toggle - so a cell holds the
// specialization for its identity under the CURRENT period values, and stops
// answering the moment those change. Lifetime is safe because JIT code is never
// freed in production; the safety gate auto-disables the cache if a releaser is
// wired (no HP-scan retire). The code-sharing gate retains the cross-core
// pointer discipline of resolveMatchedSlot (relevant to icacheTry in non-shared
// test builds; the wrapper's inline probe is only enabled under
// EJIT_SRE_SHARED_CODE_POINTERS, where the gate is compile-time true).
//===----------------------------------------------------------------------===//
bool EJitSharedTaskPool::icacheTry(uint32_t funcIndex, const EJitDimPair *dims,
                                   uint32_t numDims, void **outFn) {
  if (!outFn)
    return false;
  *outFn = nullptr;
  // Safety gate: auto-disable while a releaser is wired ANYWHERE (no HP-scan
  // retire, so freeing code + a cached fnPtr = UAF). Shared, because the table
  // is: a peer's releaser has to close this core's probe too. Production wires
  // no releaser, so this never trips and the cache is unconditionally safe.
  if (!icacheReclamationSafeShared())
    return false;
  if (!state_ || funcIndex >= EJIT_ICACHE_FUNC_SLOTS)
    return false;
  EJitIcacheSlotReg &reg = gIcacheSlots[funcIndex];
  // Unregistered function, or shape mismatch (caller's numDims != registered):
  // miss.
  if (!reg.base || numDims != reg.numDims)
    return false;
  // The cache is only meaningful once the pool is Ready.
  if (state_->initState.loadAcquire() != kReady)
    return false;
  // Cross-core fnPtr gate (compile-time, mirrors resolveMatchedSlot): a
  // non-owner core may only read a cached pointer when code sharing is
  // platform-validated.
  uint32_t self = EJitCoreId::current();
  uint32_t owner = state_->ownerCoreId.loadRelaxed();
#if defined(EJIT_SRE_SHARED_CODE_POINTERS)
  constexpr bool mayReadPtr = true;
#else
  bool mayReadPtr = (self == owner);
#endif
  if (!mayReadPtr)
    return false;
  if (!icacheDimsInRange(dims, numDims))
    return false;
  // Read this identity's cell. Relaxed is enough, and is what the AOT probe
  // emits: the cell holds a self-contained pointer, the caller's indirect call
  // depends on the value loaded (data dependency), and the only cross-core
  // write is a drain storing the slot's empty value -- which yields a miss
  // (the sentinel IS MissFn), never a bad pointer.
  uintptr_t idx = icacheLinearize(dims, numDims);
  uintptr_t p = icacheCell(reg.base, idx).loadRelaxed();
  if (p == 0 || p == reinterpret_cast<uintptr_t>(reg.missFn))
    return false;
  *outFn = reinterpret_cast<void *>(p);
  return true;
}

//===----------------------------------------------------------------------===//
// Per-core L0 dispatch cache
//
// A direct-mapped table in core-private memory, in front of the bucket lookup.
// A hit is a key compare plus one read of the shared dispatchEpoch: no atomics,
// no scan, no cache-line ownership transfer.
//
// Invalidation is coarse on purpose -- any epoch bump retires every core's
// whole table. Publishes cluster in warm-up and then stop, so the epoch is
// stable in steady state and entries only miss while warming.
//===----------------------------------------------------------------------===//
EJitL0Entry llvm::ejit::gEJitL0[kEJitL0Slots];
const void *llvm::ejit::gEJitL0State = nullptr;

void EJitSharedTaskPool::retireDispatchCache() {
  if (!state_)
    return;
  state_->dispatchEpoch.fetchAdd(1);
  // The inline cache answers the same (identity -> fnPtr) question with no
  // epoch of its own, so the events that retire the L0 must empty it outright.
  icacheDrainAll("retire-dispatch-cache");
}

void EJitSharedTaskPool::l0Fill(uint32_t funcIndex, void *fnPtr,
                                const EJitDimPair *dims, uint32_t numDims) {
  if (!icacheReclamationSafeShared() || !state_ || !fnPtr)
    return;
  // PGO hotness is counted in resolveMatchedSlot(). An L0 hit bypasses that
  // path, so caching here would prevent the Tier-2 threshold from being
  // reached. PGO-off behavior and its steady-state L0 hot path are unchanged.
  if (state_->pgoEnabled.loadAcquire())
    return;
  if (numDims > kEJitSharedMaxDims)
    return;
  if (state_->initState.loadAcquire() != kReady)
    return;
#if !defined(EJIT_SRE_SHARED_CODE_POINTERS)
  if (EJitCoreId::current() != state_->ownerCoreId.loadRelaxed())
    return;
#endif
  gEJitL0State = state_;
  EJitL0Entry &e = gEJitL0[ejitL0Index(funcIndex, dims, numDims)];

  // Read the epoch BEFORE publishing, so a bump racing the fill leaves a stale
  // epoch (a miss), never a fresh epoch on a stale pointer.
  const uint32_t epoch = state_->dispatchEpoch.loadAcquire();

  // Seqlock writer: odd while the payload is inconsistent.
  e.seq = e.seq + 1u;
  std::atomic_signal_fence(std::memory_order_release);

  e.fn = fnPtr;
  e.core = EJitCoreId::current();
  e.funcIndex = funcIndex;
  e.numDims = numDims;
  for (uint32_t i = 0; i < numDims; ++i)
    e.dims[i] = dims[i];
  e.epoch = epoch;

  std::atomic_signal_fence(std::memory_order_release);
  e.seq = e.seq + 1u;
}

// One-shot diagnostics for icacheFill's decline paths: the fill runs on every
// successful resolve, so an unconditional log would emit millions of lines.
// Core-private .bss, compiled out when EJIT_DIAG_ENABLE is off.
namespace {
enum EJitIcacheFillReject {
  kFillRejectReclaim = 0,
  kFillRejectArgs,
  kFillRejectUnregistered,
  kFillRejectDimRange,
  kFillRejectRacedDrain,
  kFillRejectRetracted,
  kFillRejectScalarShared,
  kFillRejectCount
};
bool gIcacheFillRejectLogged[kFillRejectCount];
bool gIcacheFillOkLogged;

} // namespace

void EJitSharedTaskPool::icacheFill(uint32_t funcIndex, void *fnPtr,
                                    const EJitDimPair *dims, uint32_t numDims,
                                    uint64_t token) {
  if (!icacheReclamationSafeShared()) {
    if (!gIcacheFillRejectLogged[kFillRejectReclaim]) {
      gIcacheFillRejectLogged[kFillRejectReclaim] = true;
      EJIT_DIAG("icacheFill DECLINE func=%u: a releaser is wired, so the cache "
                "is disabled to avoid handing out freed code",
                funcIndex);
    }
    return;
  }
  // NO blanket icacheCrossCoreExecutable() gate here, deliberately -- but see
  // the numDims == 0 case below, which is not covered by the argument.
  //
  // Cells are addressed by dim identity, and the deployment contract is that
  // cores drive DISJOINT ejit_period_lc instance indices. A core therefore only
  // ever reads cells it filled itself, and it filled them after resolving
  // through the taskpool -- which is where it did whatever per-core execute
  // preparation the platform needs (4K seal / prepareCodeFn_). So the pointer
  // it loads is always code it has already prepared, which is the guarantee a
  // per-core .bss table used to give structurally.
  //
  // Gating every shape on icacheCrossCoreExecutable() instead disables the
  // cache in every build it is allowed in: EJitCompileDriver sets fourKSeal_
  // under EJIT_CODE_POOL_4K_SEAL and wires prepareCodeFn_ otherwise, both
  // inside EJIT_SRE_SHARED_CODE_POINTERS, which the AOT probe requires. Both
  // branches close it, so no cell would ever be filled on any real target.
  //
  // What such a gate would protect against is two cores sharing a dim identity,
  // where the second loads a pointer only the first has sealed. For a
  // dimensioned entry that is a violation of the disjointness contract, and it
  // cannot be detected from here: a cell carries no record of which core wrote
  // it, and adding one puts a per-core gate back on a probe whose whole point
  // is not having one.
  if (!state_ || !fnPtr || funcIndex >= EJIT_ICACHE_FUNC_SLOTS) {
    if (!gIcacheFillRejectLogged[kFillRejectArgs]) {
      gIcacheFillRejectLogged[kFillRejectArgs] = true;
      EJIT_DIAG("icacheFill DECLINE func=%u: state=%p fn=%p slots=%u",
                funcIndex, (const void *)state_, fnPtr, EJIT_ICACHE_FUNC_SLOTS);
    }
    return;
  }
  EJitIcacheSlotReg &reg = gIcacheSlots[funcIndex];
  if (!reg.base || numDims != reg.numDims) {
    // Unregistered, or shape mismatch: nowhere to write. A null base means this
    // core never ran ejit_register_icache_slot for this function -- the AOT
    // .ejit_period entry is missing, or the name did not resolve to funcIndex.
    if (!gIcacheFillRejectLogged[kFillRejectUnregistered]) {
      gIcacheFillRejectLogged[kFillRejectUnregistered] = true;
      EJIT_DIAG("icacheFill DECLINE func=%u: base=%p registeredDims=%u "
                "callerDims=%u (null base = never registered on this core)",
                funcIndex, (const void *)reg.base, reg.numDims, numDims);
    }
    return;
  }
  // A 0-dim entry has exactly ONE cell and no identity to partition it by, so
  // the disjointness argument above does not reach this shape: every core reads
  // and writes that same scalar, by construction, no matter how well behaved
  // the deployment is. Core A can fill it having prepared the code only for A,
  // and core B then loads a non-null pointer on its very first call and
  // branches to a page it never sealed.
  //
  // There is no per-core information in the cell to recover the implication
  // from, so the only sound rule is the platform one: allow the shared scalar
  // only where a resolved pointer is callable everywhere the instant it exists.
  // Elsewhere the probe simply keeps missing and the taskpool -- which does the
  // per-core preparation -- serves every call. The AOT side still emits the
  // probe for 0D entries; it is the fill, not the code, that is platform
  // dependent.
  if (numDims == 0 && !icacheCrossCoreExecutable()) {
    if (!gIcacheFillRejectLogged[kFillRejectScalarShared]) {
      gIcacheFillRejectLogged[kFillRejectScalarShared] = true;
      EJIT_DIAG(
          "icacheFill DECLINE func=%u: 0-dim entry shares one cell across "
          "cores and this platform prepares execute permission per core, "
          "so a peer could branch to code it has not sealed",
          funcIndex);
    }
    return;
  }
  // No cell for this identity. The taskpool serves it; never index anyway.
  if (!icacheDimsInRange(dims, numDims)) {
    if (!gIcacheFillRejectLogged[kFillRejectDimRange]) {
      gIcacheFillRejectLogged[kFillRejectDimRange] = true;
      EJIT_DIAG("icacheFill DECLINE func=%u: an instance id is >= the table's "
                "per-dim bound %u, so this identity has no cell",
                funcIndex, EJIT_ICACHE_DIM_SIZE);
    }
    return;
  }
  // Drop the fill if any drain overlapped this resolve: fnPtr may be
  // specialized for the period values the toggle just replaced, and restoring a
  // cell the drain cleared is something nothing later would notice. The token
  // pins the start of the window, these two loads the end.
  if (!(token & kEJitIcacheResolveValid) ||
      state_->icacheDrainsInFlight.loadAcquire() != 0 ||
      static_cast<uint32_t>(token) != state_->icacheDrainSeq.loadAcquire()) {
    // Expected occasionally: a period toggle overlapped this resolve. Reported
    // once because a PERMANENT stream of these means something is draining
    // continuously, which would keep the cache empty forever.
    if (!gIcacheFillRejectLogged[kFillRejectRacedDrain]) {
      gIcacheFillRejectLogged[kFillRejectRacedDrain] = true;
      EJIT_DIAG("icacheFill DECLINE func=%u: resolve raced a drain "
                "(token=0x%llx valid=%u seqAtResolve=%u seqNow=%u inFlight=%u)",
                funcIndex, static_cast<unsigned long long>(token),
                static_cast<unsigned>((token & kEJitIcacheResolveValid) != 0),
                static_cast<unsigned>(token),
                state_->icacheDrainSeq.loadAcquire(),
                state_->icacheDrainsInFlight.loadAcquire());
    }
    return;
  }
  // A wrapper-inline-cache hit bypasses both PGO hit counting and observation
  // of a later Tier-2 publish. Keep Tier-1 out of the cell while PGO is active;
  // the first resolve of the published Tier-2 pointer may fill it normally.
  if (state_->pgoEnabled.loadAcquire() &&
      !isPublishedTier2(funcIndex, fnPtr, dims, numDims))
    return;
  // Relaxed, and no cross-core publication happens here: under the disjointness
  // contract the core that READS this cell is this core, and it already made
  // the code executable for itself (prepareExecForCurrentCore: split,
  // enable_ex, dc cvau / ic ivau / dsb ish / isb) earlier in program order.
  // Cross-core visibility of the CODE is the owner's broadcast maintenance.
  // Arm BEFORE the store, with release, so a drain that could see this cell
  // non-null is guaranteed to see the flag set and therefore walk. Setting it
  // after the store would let a drain read 0, skip, and leave the cell behind.
  state_->icacheArmed.storeRelease(1);
  const uintptr_t idx = icacheLinearize(dims, numDims);
  EJitAtomicUPtr &cell = icacheCell(reg.base, idx);
  cell.storeRelaxed(reinterpret_cast<uintptr_t>(fnPtr));
#ifdef EJIT_SRE_TASKPOOL_TESTING
  // Test seam: the window between the store and the re-validation below is
  // exactly where a preempting peer core's drain lands in production. A hook
  // lets a single-threaded test fire a drain there deterministically and reach
  // the retract, which the pre-store checks otherwise decline.
  if (icacheFillMidpointHook_)
    icacheFillMidpointHook_(icacheFillMidpointCtx_);
#endif

  // Re-validate AFTER the store and retract on conflict. The checks above only
  // PRECEDE the store: a drain can begin after they pass, empty this cell, and
  // finish before the store lands, stranding a pre-toggle specialization that
  // nothing clears again -- and a preempted filler makes that gap unbounded.
  // If a drain overlapped, it has either bumped the sequence or is still in
  // flight, and both are visible here. Writing the slot's empty value (the
  // &MissFn sentinel for sentinel-form tables, 0 for guarded ones) discards
  // only this core's own fill (disjointness), and an empty cell is always
  // safe: the sentinel is callable, and 0 is guarded.
  if (state_->icacheDrainsInFlight.loadAcquire() != 0 ||
      static_cast<uint32_t>(token) != state_->icacheDrainSeq.loadAcquire()) {
    cell.storeRelaxed(reinterpret_cast<uintptr_t>(reg.missFn));
    if (!gIcacheFillRejectLogged[kFillRejectRetracted]) {
      gIcacheFillRejectLogged[kFillRejectRetracted] = true;
      EJIT_DIAG("icacheFill RETRACT func=%u: a drain began after the fill was "
                "cleared to proceed; cell reset to its empty value (%p)",
                funcIndex, reg.missFn);
    }
    return;
  }
  if (!gIcacheFillOkLogged) {
    gIcacheFillOkLogged = true;
    EJIT_DIAG(
        "icacheFill OK func=%u cell=%p[%llu] fn=%p -- the inline cache is "
        "live on this core",
        funcIndex, (const void *)reg.base, static_cast<unsigned long long>(idx),
        fnPtr);
  }
}

EJitSharedTaskPool::ForEachCompiledStats
EJitSharedTaskPool::forEachCompiled(CompiledFuncCallback cb, void *ctx) const {
  ForEachCompiledStats stats;
  if (!state_ || !cb)
    return stats;
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b) {
    EJitSharedCacheBucket &B = state_->buckets[b];
    // Acquire the per-bucket read lock; retry briefly if a writer is
    // mid-publish, then skip the bucket if still contended (diagnostic — a
    // missing entry here just means it was being updated this instant).
    bool locked = false;
    for (int retry = 0; retry < 16; ++retry) {
      if (bucketTryRead(B)) {
        locked = true;
        break;
      }
      cpuRelax();
    }
    if (!locked) {
      // Skipped, not silent: the caller reports it from the returned stats.
      ++stats.skippedBuckets;
      continue;
    }
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      const EJitSharedCacheSlot &slot = B.slots[s];
      if (static_cast<EJitSharedSlotState>(slot.state.loadAcquire()) !=
          EJitSharedSlotState::Ready)
        continue;
      // Pass the whole slot; the callback reads fnPtr with its own acquire
      // load, per the publish protocol (state Ready acquire orders it).
      cb(slot, ctx);
      ++stats.visitedSlots;
    }
    bucketReadRelease(B);
  }
  return stats;
}

bool EJitSharedTaskPool::setInstanceEnabled(uint32_t dimType,
                                            uint32_t instanceId, bool enabled) {
  if (!state_ || dimType >= kEJitSharedDimTypes ||
      instanceId >= kEJitSharedInstances ||
      dimType == kEJitSharedConstDimType) {
    EJIT_DIAG("shared setInstanceEnabled reject: state=%p dim=%u inst=%u "
              "(OOR dim<%u inst<%u, or reserved const-dim slot %u)",
              (void *)state_, dimType, instanceId, kEJitSharedDimTypes,
              kEJitSharedInstances, kEJitSharedConstDimType);
    return false;
  }
  uint8_t expected = enabled ? 0 : 1;
  uint8_t desired = enabled ? 1 : 0;
  const bool flipped =
      state_->enabled[dimType][instanceId].compareExchange(expected, desired);

  // The INVALIDATIONS run unconditionally: neither cache stores a version, and
  // a caller that LOST the CAS may still have rewritten its own core-private
  // period values. Draining the shared cell table here is what reaches a peer
  // core that is permanently hot and would never call in on its own.
  state_->dispatchEpoch.fetchAdd(1);
  icacheDrainAll("period-toggle");

  // version[] moves only on a real transition: its consumer is runCompile's
  // checkpoints, which DISCARD a finished compile when it moves, and nothing
  // re-enqueues a dropped compile. An unconditional bump would let N cores
  // activating the same instance at startup stall the JIT permanently.
  if (flipped)
    state_->version[dimType][instanceId].fetchAdd(1);
  // Latch on the first enable of ANY instance: this brackets the init→activate
  // window during which instanceDisabled hits are tallied separately
  // (instanceDisabledPreActivate) for diagnosing the pre-activate fallback
  // storm. Once latched it stays 1 until the next (re)initialization.
  if (enabled)
    state_->anyInstanceActivated.storeRelease(1);
  return flipped;
}

uint32_t EJitSharedTaskPool::setAllInstancesEnabled(uint32_t dimType,
                                                    bool enabled) {
  if (!state_ || dimType >= kEJitSharedDimTypes ||
      dimType == kEJitSharedConstDimType) {
    EJIT_DIAG("shared setAllInstancesEnabled reject: state=%p dim=%u (OOR "
              "dim<%u, or reserved const-dim slot %u)",
              (void *)state_, dimType, kEJitSharedDimTypes,
              kEJitSharedConstDimType);
    return 0;
  }
  const uint8_t desired = enabled ? 1 : 0;
  uint32_t flippedCount = 0;
  for (uint32_t i = 0; i < kEJitSharedInstances; ++i) {
    // compareExchange takes `expected` by reference and writes back the
    // observed value on failure, so it needs a fresh local each iteration.
    uint8_t expected = enabled ? 0 : 1;
    if (!state_->enabled[dimType][i].compareExchange(expected, desired))
      continue;
    ++flippedCount;
    // As in setInstanceEnabled: version[] moves only on a real transition.
    state_->version[dimType][i].fetchAdd(1);
  }
  // ONE epoch bump and ONE drain: both are global, so doing them per instance
  // was pure amplification.
  state_->dispatchEpoch.fetchAdd(1);
  icacheDrainAll();
  if (enabled)
    state_->anyInstanceActivated.storeRelease(1);
  return flippedCount;
}

bool EJitSharedTaskPool::versionsCurrent(const EJitCompileRequest &req) const {
  for (uint32_t i = 0; i < req.numDims; ++i)
    if (req.versions[i] !=
        instanceVersion(req.dims[i].dimType, req.dims[i].instanceId))
      return false;
  return true;
}

//===----------------------------------------------------------------------===//
// Dedup helpers (§3.5) over the shared flat slots. Each slot stores the OWNER
// GENERATION that claimed it (0 = free), so cross-generation clears are
// impossible (spec §11 generation-aware dedup).
//===----------------------------------------------------------------------===//
EJitDedupResult EJitSharedTaskPool::dedupMark(uint32_t funcIndex,
                                              uint32_t gen) {
  // PGO: Tier-2 requests pack the tier into the top 2 bits of funcIndex
  // (encodeReqTier).  Strip them before using funcIndex as an array index,
  // otherwise Tier-1 and Tier-2 requests for the same function would map
  // to different dedup slots.
  funcIndex = stripReqTier(funcIndex);
  if (funcIndex >= kEJitSharedMaxFuncIndex)
    return EJitDedupResult::InvalidFuncIndex;
  uint32_t expected = 0;
  if (state_->inFlight[funcIndex].compareExchange(expected, gen))
    return EJitDedupResult::Claimed;
  return EJitDedupResult::AlreadyPending;
}

void EJitSharedTaskPool::dedupClear(uint32_t funcIndex, uint32_t gen) {
  // PGO: Tier-2 requests carry the tier in funcIndex's top 2 bits
  // (encodeReqTier). dedupMark strips them before indexing, so dedupClear MUST
  // strip identically \u2014 otherwise an encoded Tier-2 funcIndex indexes out
  // of range (or a different slot) and the in-flight bit is never cleared,
  // which would permanently block the next Tier-2 claim for that function.
  funcIndex = stripReqTier(funcIndex);
  if (funcIndex >= kEJitSharedMaxFuncIndex)
    return;
  // CAS gen->0: only clears the slot if it still holds OUR generation. A stale
  // worker whose generation was superseded (or whose slot was re-claimed by a
  // new generation after an owner re-init) fails this CAS and clears nothing.
  uint32_t expected = gen;
  state_->inFlight[funcIndex].compareExchange(expected, 0u);
}

//===----------------------------------------------------------------------===//
// MPSC queue helpers (Vyukov, §3.3) over the shared ring.
//===----------------------------------------------------------------------===//
bool EJitSharedTaskPool::queuePush(const EJitCompileRequest &req) {
  constexpr uint32_t mask = kEJitSharedQueueSlots - 1;
  uint32_t pos = state_->enqueuePos.loadRelaxed();
  EJitSharedQueueCell *cell;
  for (;;) {
    cell = &state_->ring[pos & mask];
    uint32_t seq = cell->sequence.loadAcquire();
    int32_t dif = static_cast<int32_t>(seq) - static_cast<int32_t>(pos);
    if (dif == 0) {
      if (state_->enqueuePos.compareExchange(pos, pos + 1))
        break;
    } else if (dif < 0) {
      return false; // full
    } else {
      pos = state_->enqueuePos.loadRelaxed();
    }
  }
  cell->data = req;
  cell->sequence.storeRelease(pos + 1);
  return true;
}

bool EJitSharedTaskPool::queuePop(EJitCompileRequest &out) {
  constexpr uint32_t mask = kEJitSharedQueueSlots - 1;
  uint32_t pos = state_->dequeuePos.loadRelaxed();
  EJitSharedQueueCell *cell;
  for (;;) {
    cell = &state_->ring[pos & mask];
    uint32_t seq = cell->sequence.loadAcquire();
    int32_t dif = static_cast<int32_t>(seq) - static_cast<int32_t>(pos + 1);
    if (dif == 0) {
      if (state_->dequeuePos.compareExchange(pos, pos + 1))
        break;
    } else if (dif < 0) {
      return false; // empty
    } else {
      pos = state_->dequeuePos.loadRelaxed();
    }
  }
  out = cell->data;
  cell->sequence.storeRelease(pos + mask + 1);
  return true;
}

//===----------------------------------------------------------------------===//
// Shared POD result cache (§4.1, re-expressed without std::unordered_map).
//===----------------------------------------------------------------------===//
uint64_t EJitSharedTaskPool::hashIdentity(uint32_t funcIndex,
                                          const EJitDimPair *dims,
                                          uint32_t numDims) const {
  uint64_t key = static_cast<uint64_t>(funcIndex);
  for (uint32_t i = 0; i < numDims; ++i) {
    key ^= (static_cast<uint64_t>(dims[i].dimType) << 32) |
           static_cast<uint64_t>(dims[i].instanceId);
    key *= 0x9e3779b97f4a7c15ULL;
  }
  return key;
}

static bool slotIdentityMatches(const EJitSharedCacheSlot &s,
                                uint32_t funcIndex, const EJitDimPair *dims,
                                uint32_t numDims) {
  if (s.funcIndex != funcIndex || s.numDims != numDims)
    return false;
  for (uint32_t i = 0; i < numDims; ++i)
    if (s.dims[i].dimType != dims[i].dimType ||
        s.dims[i].instanceId != dims[i].instanceId)
      return false;
  return true;
}

bool EJitSharedTaskPool::isPublishedTier2(uint32_t funcIndex, void *fnPtr,
                                          const EJitDimPair *dims,
                                          uint32_t numDims) {
  if (!state_ || !fnPtr || numDims > kEJitSharedMaxDims)
    return false;

  const uint64_t key = hashIdentity(funcIndex, dims, numDims);
  const uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];

  auto matchesTier2 = [&]() {
    const uint32_t curGen = state_->generation.loadAcquire();
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue;
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen ||
          !slotIdentityMatches(Slot, funcIndex, dims, numDims))
        continue;
      for (uint32_t i = 0; i < numDims; ++i)
        if (Slot.versions[i] !=
            instanceVersion(dims[i].dimType, dims[i].instanceId))
          return false;
      return Slot.tier.loadRelaxed() >= kEJitTierPgoUse &&
             reinterpret_cast<void *>(Slot.fnPtr.loadAcquire()) == fnPtr;
    }
    return false;
  };

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) {
      cpuRelax();
      continue;
    }
    const bool match = matchesTier2();
    if (bucketSeqStable(B, seq0))
      return match;
    cpuRelax();
  }
  return false;
#else
  if (!bucketTryRead(B))
    return false;
  const bool match = matchesTier2();
  bucketReadRelease(B);
  return match;
#endif
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup(uint32_t funcIndex, const EJitDimPair *dims,
                                uint32_t numDims) {
  SharedLookup R;
  if (numDims > 4)
    return R;
  uint64_t key = hashIdentity(funcIndex, dims, numDims);
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;

  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen) // stale across an owner re-init: ignore.
      continue;
    if (!slotIdentityMatches(Slot, funcIndex, dims, numDims))
      continue;
    bool versionsOk = true;
    for (uint32_t i = 0; i < numDims; ++i)
      if (Slot.versions[i] !=
          instanceVersion(dims[i].dimType, dims[i].instanceId)) {
        versionsOk = false;
        break;
      }
    if (!versionsOk)
      break; // identity matched but stale → miss, token off.
    // Identity + versions match: resolve the fnPtr gate (owner/memoized fast
    // return, clean reject, or cold peer preparation). Terminal — a bucket
    // holds at most one slot per identity.
    return resolveMatchedSlot(B, bucket, s);
  }

  bucketReadRelease(B);
  return R;
}

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
//===----------------------------------------------------------------------===//
// Load-only seqlock cache lookup (NO_RECLAIM build only). Same result as
// cacheLookup() but with ZERO per-hit RMW on the shared bucket line: the reader
// never touches bucket.readers. Consistency is a seqlock over bucket.publishSeq
// (snapshot before the scan, re-check after resolving). If a publish raced the
// read, retry a bounded number of times, then clean-miss and let the caller
// fall back. Memory-safe ONLY because a published fnPtr is never freed in this
// build, so a returned pointer that holds no read token can never dangle.
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookupSeq(uint32_t funcIndex, const EJitDimPair *dims,
                                   uint32_t numDims) {
  SharedLookup R;
  if (numDims > 4)
    return R;
  uint64_t key = hashIdentity(funcIndex, dims, numDims);
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) { // publish in progress -> retry
      cpuRelax();
      continue;
    }
    uint32_t curGen = state_->generation.loadAcquire();
    SharedLookup hit;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue; // plain-load fast reject before the acquiring state load.
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen)
        continue;
      if (!slotIdentityMatches(Slot, funcIndex, dims, numDims))
        continue;
      bool versionsOk = true;
      for (uint32_t i = 0; i < numDims; ++i)
        if (Slot.versions[i] !=
            instanceVersion(dims[i].dimType, dims[i].instanceId)) {
          versionsOk = false;
          break;
        }
      if (!versionsOk)
        break; // identity matched but stale -> clean miss
      hit = resolveMatchedSlot(B, bucket, s);
      break;
    }
    // Validate that the scan + resolve observed a single stable publish epoch.
    // A cold peer preparation (coldPrepared) re-validates itself and may
    // legally span a publish, so it is exempt from the outer seq re-check.
    if (hit.fnPtr && !hit.coldPrepared && !bucketSeqStable(B, seq0)) {
      cpuRelax();
      continue; // publish raced the read -> retry
    }
    return hit; // validated hit (noTokenHit) or clean miss
  }
  return R; // repeated contention -> clean fallback to the slow path
}

//===----------------------------------------------------------------------===//
// Fixed-dimension load-only seqlock specializations (0-2 dims, NO_RECLAIM
// build only). Each mirrors the matching cacheLookup0D/1D/2D scan (unrolled
// identity hash, unrolled identity + version comparison, plain-identityHash
// fast reject before the acquiring state load) wrapped in the same bounded
// seqlock retry as cacheLookupSeq(). A NO_RECLAIM fixed-dimension caller thus
// avoids the generic numDims loop / slotIdentityMatches() call entirely.
// Behavior is identical to cacheLookupSeq() with the matching numDims; the
// cross-core fnPtr gate and cold peer preparation stay shared via
// resolveMatchedSlot().
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookupSeq0D(uint32_t funcIndex) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex); // hashIdentity(fi, _, 0)
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) {
      cpuRelax();
      continue;
    }
    uint32_t curGen = state_->generation.loadAcquire();
    SharedLookup hit;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue; // plain-load fast reject before the acquiring state load.
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen)
        continue;
      if (Slot.funcIndex != funcIndex || Slot.numDims != 0)
        continue;
      hit = resolveMatchedSlot(B, bucket, s);
      break;
    }
    if (hit.fnPtr && !hit.coldPrepared && !bucketSeqStable(B, seq0)) {
      cpuRelax();
      continue;
    }
    return hit;
  }
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookupSeq1D(uint32_t funcIndex, uint32_t dim0,
                                     uint32_t inst0) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) {
      cpuRelax();
      continue;
    }
    uint32_t curGen = state_->generation.loadAcquire();
    SharedLookup hit;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue;
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen)
        continue;
      if (Slot.funcIndex != funcIndex || Slot.numDims != 1)
        continue;
      if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
        continue;
      if (Slot.versions[0] != instanceVersion(dim0, inst0))
        break; // identity matched but stale -> clean miss.
      hit = resolveMatchedSlot(B, bucket, s);
      break;
    }
    if (hit.fnPtr && !hit.coldPrepared && !bucketSeqStable(B, seq0)) {
      cpuRelax();
      continue;
    }
    return hit;
  }
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookupSeq2D(uint32_t funcIndex, uint32_t dim0,
                                     uint32_t inst0, uint32_t dim1,
                                     uint32_t inst1) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim1) << 32) | static_cast<uint64_t>(inst1);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) {
      cpuRelax();
      continue;
    }
    uint32_t curGen = state_->generation.loadAcquire();
    SharedLookup hit;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue;
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen)
        continue;
      if (Slot.funcIndex != funcIndex || Slot.numDims != 2)
        continue;
      if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
        continue;
      if (Slot.dims[1].dimType != dim1 || Slot.dims[1].instanceId != inst1)
        continue;
      if (Slot.versions[0] != instanceVersion(dim0, inst0))
        break;
      if (Slot.versions[1] != instanceVersion(dim1, inst1))
        break;
      hit = resolveMatchedSlot(B, bucket, s);
      break;
    }
    if (hit.fnPtr && !hit.coldPrepared && !bucketSeqStable(B, seq0)) {
      cpuRelax();
      continue;
    }
    return hit;
  }
  return R;
}
#endif // EJIT_SRE_TASKPOOL_NO_RECLAIM

//===----------------------------------------------------------------------===//
// Shared slot resolution: applied once a slot's identity + versions have
// matched (bucket read lock held). Dimension-independent, so cacheLookup() and
// every fixed-dimension cacheLookupNd() share it. The owner core and any core
// that already memoized execute permission return the hit here with the read
// token held; a core that may not legally read the cross-core pointer gets a
// clean readyButNotShareable fallback; the rare non-owner first touch is
// delegated to the out-of-line peerPrepareSlot().
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::resolveMatchedSlot(EJitSharedCacheBucket &B,
                                       uint32_t bucket, uint32_t slotIndex) {
  SharedLookup R;
  EJitSharedCacheSlot &Slot = B.slots[slotIndex];

  // PGO (§6): every identity-matched hit increments the slot's hitCount.
  // The hit that crosses tier2Threshold_ arms a one-shot Tier-2 recompile.
  // We count all identity hits — even those rejected for shareability —
  // because the function IS being called; the PGO counter is a hotness
  // proxy, not an execution-completed count.
  // A slot already at Tier-2 (PGOUse) is never re-armed — Tier-2 code does
  // not need another Tier-2 compile (§4 repeat-trigger).
  if (state_->pgoEnabled.loadAcquire()) {
    uint8_t slotTier = Slot.tier.loadRelaxed();
    if (slotTier < kEJitTierPgoUse) {
      uint32_t threshold = state_->tier2Threshold.loadAcquire();
      uint64_t sampleIndex = 0;
      if (slotTier == kEJitTierInstrumented && threshold) {
        // Cap Tier-1 execution at the requested number of root samples. A
        // plain fetchAdd lets peer cores overshoot, and continuing to dispatch
        // Tier-1 while a large Tier-2 waits/compiles keeps every instrumented
        // function in that module doing atomic counter updates for no benefit.
        uint64_t observed = Slot.hitCount.loadRelaxed();
        while (observed < threshold &&
               !Slot.hitCount.compareExchange(observed, observed + 1)) {
        }
        if (observed >= threshold) {
          // Retry the enqueue on every saturated lookup if a previous attempt
          // lost to queue pressure. dedupMark makes the normal pending case a
          // cheap no-op. The slot and Tier-1 code remain intact because Tier-2
          // profile synthesis still reads their counter storage.
          R.slot = &Slot;
          R.tier2Arm = true;
#ifndef EJIT_SRE_TASKPOOL_NO_RECLAIM
          bucketReadRelease(B);
#endif
          R.pgoSamplingComplete = true;
          return R;
        }
        sampleIndex = observed + 1;
      } else {
        sampleIndex = Slot.hitCount.fetchAdd(1) + 1;
      }
      uint32_t admissionSlot = kEJitSharedMaxConcurrentProfiles;
      if (slotTier == kEJitTierInstrumented && threshold) {
        const uint32_t encoded = Slot.funcIndex + 1;
        for (uint32_t i = 0; i < kEJitSharedMaxConcurrentProfiles; ++i) {
          if (state_->pgoActiveFunctions[i].loadAcquire() == encoded) {
            admissionSlot = i;
            break;
          }
        }
      }
      if (admissionSlot != kEJitSharedMaxConcurrentProfiles) {
        uint32_t quarter = static_cast<uint32_t>(
            std::min<uint64_t>(4, (sampleIndex * 4) / threshold));
        uint32_t oldQuarter =
            state_->pgoProgressQuarters[admissionSlot].loadRelaxed();
        while (quarter > oldQuarter &&
               !state_->pgoProgressQuarters[admissionSlot].compareExchange(
                   oldQuarter, quarter)) {
        }
        if (quarter > oldQuarter)
          EJIT_DIAG("PGO profile progress func=%u: %llu/%u", Slot.funcIndex,
                    static_cast<unsigned long long>(sampleIndex), threshold);
      }
      // >= (not ==): if the enqueue fails (queue full / already pending),
      // the next hit can still arm — a transient failure is not fatal (§7).
      if (threshold && sampleIndex >= threshold) {
        // Preserve the fixed slot address even when this producer cannot read
        // the cross-core code pointer. tryCacheHit() performs the deferred
        // enqueue after lookup so a bound call can attach its descriptors.
        R.slot = &Slot;
        R.tier2Arm = true;
        if (slotTier == kEJitTierInstrumented && sampleIndex == threshold)
          EJIT_DIAG("PGO sampling complete func=%u: %llu/%u; routing AOT "
                    "until Tier-2 publish",
                    Slot.funcIndex,
                    static_cast<unsigned long long>(sampleIndex), threshold);
      }
    }
  }

  // fnPtr cross-core gate (§11 prerequisites): a non-owner core may read the
  // pointer only when code sharing is platform-validated (same VA, sealed,
  // I/D-cache coherent). Otherwise CLEAN-REJECT — never hand back a pointer
  // this core may not legally execute.
  uint32_t self = EJitCoreId::current();
  // ownerCoreId is immutable for the lifetime of a Ready pool: written exactly
  // once before initState is published Ready (release, "publish last"), and
  // every hit already observed initState==Ready via the acquire Ready-check at
  // entry - which makes that write visible here. It never changes during Ready,
  // so there is no concurrent write to order against; a relaxed re-load
  // suffices. The slot's state/fnPtr acquire loads still gate code publication
  // independently below.
  uint32_t owner = state_->ownerCoreId.loadRelaxed();
  // codeSharingEnabled's value is fixed at compile time by
  // EJIT_SRE_SHARED_CODE_POINTERS, so avoid a shared acquire load on every hit.
#if defined(EJIT_SRE_SHARED_CODE_POINTERS)
  constexpr bool mayReadPtr = true;
#else
  bool mayReadPtr = (self == owner);
#endif
  if (!mayReadPtr) {
    bucketReadRelease(B);
    R.readyButNotShareable = true;
    return R;
  }
  void *fn = reinterpret_cast<void *>(Slot.fnPtr.loadAcquire());
  if (!fn) {
    bucketReadRelease(B);
    return R; // identity matched but no pointer yet → miss.
  }

  // The owner already has execute permission (it sealed the code before
  // publishing). Return directly while holding the read token.
  if (self == owner) {
    R.fnPtr = fn;
    R.slot = &Slot;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
    R.noTokenHit = true;
    R.bucketIndex = kEJitSharedCacheBuckets; // sentinel -> releaseRead no-op
#else
    R.bucketIndex = bucket;
    R.hasReadToken = true;
#endif
    return R;
  }

  // Non-owner: execute permission is a per-core property on the target. If this
  // core has already prepared THIS slot's code (memoized bit), return
  // immediately under the read lock — no platform call.
  const bool CanMemoize = self < kEJitSharedMaxMemoCores;
  const uint64_t CoreBit = CanMemoize ? (uint64_t{1} << self) : uint64_t{0};
  if (CanMemoize && (Slot.executableCoreMask.loadAcquire() & CoreBit) != 0) {
    R.fnPtr = fn;
    R.slot = &Slot;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
    R.noTokenHit = true;
    R.bucketIndex = kEJitSharedCacheBuckets; // sentinel -> releaseRead no-op
#else
    R.bucketIndex = bucket;
    R.hasReadToken = true;
#endif
    return R;
  }

  // Cold: this core has never prepared this code. Preserve the Tier-2 signal
  // across the out-of-line execute-permission preparation; the producer may
  // need to attach bound descriptors before enqueueing the recompile.
  SharedLookup Prepared = peerPrepareSlot(B, bucket, slotIndex);
  Prepared.tier2Arm = R.tier2Arm;
  return Prepared;
}

void EJitSharedTaskPool::enqueueTier2ForIdentity(
    uint32_t funcIndex, const EJitDimPair *dims, uint32_t numDims,
    const EJitBoundPtrDescriptor *boundPointers, uint32_t boundCount) {
  if (state_->mode.loadAcquire() !=
      static_cast<uint32_t>(EJitCompileMode::Async))
    return;

  EJitCompileRequest T2{};
  T2.funcIndex = encodeReqTier(funcIndex, kEJitTierPgoUse);
  T2.numDims = numDims;
  T2.generation = state_->generation.loadAcquire();
  for (uint32_t i = 0; i < numDims && i < 4; ++i) {
    T2.dims[i] = dims[i];
    T2.versions[i] = instanceVersion(dims[i].dimType, dims[i].instanceId);
  }
  T2.boundCount = boundCount;
  for (uint32_t i = 0; i < boundCount; ++i)
    T2.boundPointers[i] = boundPointers[i];

  if (dedupMark(T2.funcIndex, T2.generation) != EJitDedupResult::Claimed)
    return;
  if (queuePush(T2)) {
    EJIT_STAT_INC(state_->counters.asyncEnqueues);
    EJIT_DIAG_VERBOSE("shared taskpool PGO Tier-2 enqueued func=%u gen=%u",
                      funcIndex, T2.generation);
    return;
  }

  dedupClear(T2.funcIndex, T2.generation);
  EJIT_STAT_INC(state_->counters.queueFull);
  EJIT_DIAG("shared taskpool PGO Tier-2 drop func=%u: queue full", funcIndex);
}

void EJitSharedTaskPool::enqueueTier2FromSlot(
    const EJitSharedCacheSlot &Slot,
    const EJitBoundPtrDescriptor *boundPointers, uint32_t boundCount) {
  enqueueTier2ForIdentity(Slot.funcIndex, Slot.dims, Slot.numDims,
                          boundPointers, boundCount);
}

bool EJitSharedTaskPool::admitPgoFunction(uint32_t funcIndex,
                                          bool &newlyAdmitted) {
  newlyAdmitted = false;
  const uint32_t encoded = funcIndex + 1;
  uint32_t expected = 0;
  while (!state_->pgoAdmissionLock.compareExchange(expected, 1))
    expected = 0;

  uint32_t freeSlot = kEJitSharedMaxConcurrentProfiles;
  for (uint32_t i = 0; i < kEJitSharedMaxConcurrentProfiles; ++i) {
    uint32_t active = state_->pgoActiveFunctions[i].loadRelaxed();
    if (active == encoded) {
      state_->pgoAdmissionLock.storeRelease(0);
      // Admission is per function, while cache entries are per specialization.
      // Letting another identity of the same funcIndex proceed would make both
      // versions share one progress/ownership slot: either one's failure or
      // Tier-2 completion could then release the other version prematurely.
      state_->pgoDeferredMisses.fetchAdd(1);
      return false;
    }
    if (active == 0 && freeSlot == kEJitSharedMaxConcurrentProfiles)
      freeSlot = i;
  }

  uint32_t activeCount = state_->pgoActiveFunctionCount.loadRelaxed();
  uint32_t maxActive = state_->pgoMaxActiveFunctions.loadRelaxed();
  if (activeCount >= maxActive ||
      freeSlot == kEJitSharedMaxConcurrentProfiles) {
    state_->pgoAdmissionLock.storeRelease(0);
    state_->pgoDeferredMisses.fetchAdd(1);
    return false;
  }

  state_->pgoProgressQuarters[freeSlot].storeRelaxed(0);
  state_->pgoActiveFunctions[freeSlot].storeRelease(encoded);
  state_->pgoActiveFunctionCount.storeRelease(activeCount + 1);
  state_->pgoAdmissionLock.storeRelease(0);
  newlyAdmitted = true;
  EJIT_DIAG("PGO profile start func=%u: 0/%u (active=%u/%u)", funcIndex,
            state_->tier2Threshold.loadRelaxed(), activeCount + 1, maxActive);
  return true;
}

void EJitSharedTaskPool::finishPgoFunction(uint32_t funcIndex, bool completed,
                                           const char *reason) {
  const uint32_t encoded = funcIndex + 1;
  uint32_t expected = 0;
  while (!state_->pgoAdmissionLock.compareExchange(expected, 1))
    expected = 0;

  uint32_t slot = kEJitSharedMaxConcurrentProfiles;
  for (uint32_t i = 0; i < kEJitSharedMaxConcurrentProfiles; ++i)
    if (state_->pgoActiveFunctions[i].loadRelaxed() == encoded) {
      slot = i;
      break;
    }
  if (slot == kEJitSharedMaxConcurrentProfiles) {
    state_->pgoAdmissionLock.storeRelease(0);
    return;
  }
  state_->pgoProgressQuarters[slot].storeRelaxed(0);
  state_->pgoActiveFunctions[slot].storeRelease(0);
  uint32_t activeCount = state_->pgoActiveFunctionCount.loadRelaxed();
  state_->pgoActiveFunctionCount.storeRelease(activeCount - 1);
  state_->pgoAdmissionLock.storeRelease(0);
  if (completed) {
    uint64_t done = state_->pgoCompletedFunctions.fetchAdd(1) + 1;
    EJIT_DIAG("PGO profile complete func=%u: completed=%llu deferred=%llu",
              funcIndex, static_cast<unsigned long long>(done),
              static_cast<unsigned long long>(
                  state_->pgoDeferredMisses.loadRelaxed()));
  } else {
    EJIT_DIAG("PGO profile aborted func=%u reason=%s; next function may start",
              funcIndex, reason ? reason : "unspecified");
  }
}

//===----------------------------------------------------------------------===//
// Cold non-owner first-touch execute-permission preparation. Out-of-line
// (noinline) so it never inflates the cache-hit path. Bucket read lock held on
// entry; released here.
//===----------------------------------------------------------------------===//
__attribute__((noinline)) EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::peerPrepareSlot(EJitSharedCacheBucket &B, uint32_t bucket,
                                    uint32_t slotIndex) {
  SharedLookup R;
  EJitSharedCacheSlot &Slot = B.slots[slotIndex];
  uint32_t self = EJitCoreId::current();
  void *fn = reinterpret_cast<void *>(Slot.fnPtr.loadAcquire());

  // Snapshot the identity + the REAL code range, then DROP the bucket read
  // lock: the per-core platform split/enable_ex calls (potentially slow) must
  // never run while holding a cross-core bucket lock, or a concurrent owner
  // publish (which spins for readers==0) would stall.
  PeerCodeRange Snap;
  Snap.fn = fn;
  Snap.slotIndex = slotIndex;
  Snap.bucket = bucket;
  Snap.funcIndex = Slot.funcIndex;
  Snap.numDims = Slot.numDims;
  Snap.generation = Slot.generation;
  for (uint32_t i = 0; i < Snap.numDims && i < 4; ++i) {
    Snap.dims[i] = Slot.dims[i];
    Snap.versions[i] = Slot.versions[i];
  }
  Snap.codeStart = Slot.codeStart;
  Snap.codeSize = Slot.codeSize;
  Snap.poolBase = Slot.poolBase;
  Snap.poolSize = Slot.poolSize;
  // Snapshot the runtime-writable extents too: the per-core enable_rw must run
  // with NO bucket lock held (like the split/seal below). Keep the raw count so
  // prepareExecForCurrentCore stays the single authority that rejects an
  // over-bound count; only the array copy is bounded.
  Snap.writableCount = Slot.writableCount;
  Snap.requiresPeerEnableRw = Slot.requiresPeerEnableRw;
  uint32_t copyN = Slot.writableCount > kEJitSharedMaxWritableRanges
                       ? kEJitSharedMaxWritableRanges
                       : Slot.writableCount;
  for (uint32_t i = 0; i < copyN; ++i) {
    Snap.writables[i].addr = Slot.writableRanges[i].addr;
    Snap.writables[i].size = Slot.writableRanges[i].size;
  }
  bucketReadRelease(B);

  if (!prepareExecForCurrentCore(Snap, self)) {
    EJIT_STAT_INC(state_->counters.executePrepareFailed);
    R.readyButNotShareable = true;
    return R; // clean fallback: no token, no shared pointer handed back.
  }

  // Re-acquire the read lock and RE-VALIDATE the slot: a publish may have
  // overwritten it (new code/generation) while we prepared. Only a slot that is
  // still Ready, same generation + identity + fnPtr, with current versions, may
  // hand back the prepared pointer; otherwise clean fallback so a
  // replaced/stale pointer is never returned (spec §11 / §8).
  if (!bucketTryRead(B)) {
    R.readyButNotShareable = true;
    return R;
  }
  EJitSharedCacheSlot &S2 = B.slots[Snap.slotIndex];
  uint32_t curGen2 = state_->generation.loadAcquire();
  bool stillValid =
      S2.state.loadAcquire() ==
          static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
      S2.generation == curGen2 && S2.generation == Snap.generation &&
      slotIdentityMatches(S2, Snap.funcIndex, Snap.dims, Snap.numDims) &&
      reinterpret_cast<void *>(S2.fnPtr.loadAcquire()) == Snap.fn;
  if (stillValid)
    for (uint32_t i = 0; i < Snap.numDims; ++i)
      if (S2.versions[i] !=
          instanceVersion(Snap.dims[i].dimType, Snap.dims[i].instanceId)) {
        stillValid = false;
        break;
      }
  if (!stillValid) {
    bucketReadRelease(B);
    R.readyButNotShareable = true;
    return R;
  }
  const bool CanMemoize = self < kEJitSharedMaxMemoCores;
  const uint64_t CoreBit = CanMemoize ? (uint64_t{1} << self) : uint64_t{0};
  if (CanMemoize)
    S2.executableCoreMask.fetchOr(CoreBit);
  R.fnPtr = Snap.fn;
  R.slot = &S2;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // NO_RECLAIM: this cold path already fully re-validated
  // identity/versions/fnPtr above under a load-only re-read, so it hands back a
  // validated pointer with no read token. coldPrepared tells the seqlock caller
  // not to re-check publishSeq (this out-of-line path legally spanned platform
  // calls and possible publishes, but the re-validation guarantees the returned
  // pointer is current).
  R.noTokenHit = true;
  R.coldPrepared = true;
  R.bucketIndex = kEJitSharedCacheBuckets; // sentinel -> releaseRead no-op
#else
  R.bucketIndex = Snap.bucket;
  R.hasReadToken = true;
#endif
  return R; // token held (re-acquired); caller releases after using fnPtr.
}

//===----------------------------------------------------------------------===//
// Fixed-dimension cacheLookup specializations (0-4 dims). Identity hashing,
// slot identity comparison, and version comparison are unrolled — no numDims
// loop, no dims[] indirection, no generic numDims>4 guard. The matched-slot
// fnPtr gate and cold peer preparation are shared via resolveMatchedSlot(), so
// each specialization stays small. Results are identical to cacheLookup() with
// the matching numDims. kHashMul mirrors the mixing constant in hashIdentity().
//===----------------------------------------------------------------------===//

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup0D(uint32_t funcIndex) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex); // hashIdentity(fi, _, 0)
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 0)
      continue;
    // 0D: no dims, no versions to compare.
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup1D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 1)
      continue;
    if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
      continue;
    if (Slot.versions[0] != instanceVersion(dim0, inst0))
      break; // identity matched but stale → miss.
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup2D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1,
                                  uint32_t inst1) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim1) << 32) | static_cast<uint64_t>(inst1);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 2)
      continue;
    if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
      continue;
    if (Slot.dims[1].dimType != dim1 || Slot.dims[1].instanceId != inst1)
      continue;
    if (Slot.versions[0] != instanceVersion(dim0, inst0))
      break;
    if (Slot.versions[1] != instanceVersion(dim1, inst1))
      break;
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup3D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1, uint32_t inst1,
                                  uint32_t dim2, uint32_t inst2) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim1) << 32) | static_cast<uint64_t>(inst1);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim2) << 32) | static_cast<uint64_t>(inst2);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 3)
      continue;
    if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
      continue;
    if (Slot.dims[1].dimType != dim1 || Slot.dims[1].instanceId != inst1)
      continue;
    if (Slot.dims[2].dimType != dim2 || Slot.dims[2].instanceId != inst2)
      continue;
    if (Slot.versions[0] != instanceVersion(dim0, inst0))
      break;
    if (Slot.versions[1] != instanceVersion(dim1, inst1))
      break;
    if (Slot.versions[2] != instanceVersion(dim2, inst2))
      break;
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup4D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1, uint32_t inst1,
                                  uint32_t dim2, uint32_t inst2, uint32_t dim3,
                                  uint32_t inst3) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim1) << 32) | static_cast<uint64_t>(inst1);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim2) << 32) | static_cast<uint64_t>(inst2);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim3) << 32) | static_cast<uint64_t>(inst3);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 4)
      continue;
    if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
      continue;
    if (Slot.dims[1].dimType != dim1 || Slot.dims[1].instanceId != inst1)
      continue;
    if (Slot.dims[2].dimType != dim2 || Slot.dims[2].instanceId != inst2)
      continue;
    if (Slot.dims[3].dimType != dim3 || Slot.dims[3].instanceId != inst3)
      continue;
    if (Slot.versions[0] != instanceVersion(dim0, inst0))
      break;
    if (Slot.versions[1] != instanceVersion(dim1, inst1))
      break;
    if (Slot.versions[2] != instanceVersion(dim2, inst2))
      break;
    if (Slot.versions[3] != instanceVersion(dim3, inst3))
      break;
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

//===----------------------------------------------------------------------===//
// Per-core execute-permission preparation (no bucket lock held). 2M mode seals
// the whole 2MiB pool via prepareCodeFn_ (fnPtr-aligned); 4K mode splits the
// pool once per core, then seals exactly the 4KiB pages the code covers. All
// platform work goes through injected callbacks so this core never names a
// platform symbol (spec §7).
//===----------------------------------------------------------------------===//
EJitSharedPoolSplit *EJitSharedTaskPool::findOrClaimPoolSlot(uintptr_t base) {
  // Open addressing keyed by pool base. Every core probes the SAME slot
  // sequence for a given base, so concurrent first-touch converges on one entry
  // (never two entries for the same pool, which would let a core split twice).
  uint32_t start = static_cast<uint32_t>((base / kEJitSharedSplitGranule) %
                                         kEJitSharedPoolSlots);
  for (uint32_t probe = 0; probe < kEJitSharedPoolSlots; ++probe) {
    uint32_t i = (start + probe) % kEJitSharedPoolSlots;
    EJitSharedPoolSplit &P = state_->poolSplits[i];
    uintptr_t cur = P.poolBase.loadAcquire();
    if (cur == base)
      return &P;
    if (cur == 0) {
      uintptr_t expected = 0;
      if (P.poolBase.compareExchange(expected, base))
        return &P; // we claimed this empty entry for `base`.
      // Lost the race: `expected` now holds the winner's base.
      if (expected == base)
        return &P; // another core claimed the same base here.
      // Claimed by a different base — keep probing.
    }
    // Occupied by a different base — keep probing.
  }
  return nullptr; // table full: caller cleanly falls back.
}

bool EJitSharedTaskPool::ensurePoolSplitForCurrentCore(uint32_t self,
                                                       uintptr_t poolBase,
                                                       uint64_t poolSize) {
  EJIT_DIAG_VERBOSE("ensurePoolSplit: core=%u poolBase=0x%llx size=%llu", self,
                    static_cast<unsigned long long>(poolBase),
                    static_cast<unsigned long long>(poolSize));
  // Cores beyond the 64-bit memo width cannot record split state, so they must
  // re-run the (idempotent) split on every hit rather than risk skipping it.
  if (self >= kEJitSharedMaxMemoCores) {
    bool ok = splitPoolFn_ && splitPoolFn_(splitPoolCtx_, poolBase, poolSize);
    if (!ok)
      EJIT_DIAG("ensurePoolSplit FAIL: core=%u >= memoWidth, split callback "
                "missing/failed",
                self);
    return ok;
  }

  EJitSharedPoolSplit *P = findOrClaimPoolSlot(poolBase);
  if (!P) {
    EJIT_DIAG_VERBOSE(
        "ensurePoolSplit fallback: core=%u poolBase=0x%llx split table full",
        self, static_cast<unsigned long long>(poolBase));
    return false; // table full -> clean fallback.
  }

  const uint64_t Bit = uint64_t{1} << self;
  if ((P->splitDoneMask.loadAcquire() & Bit) != 0)
    return true; // already split on this core.

  // Claim the per-core "preparing" bit. fetchOr returns the previous mask.
  const uint64_t Prev = P->splitPreparingMask.fetchOr(Bit);
  if ((Prev & Bit) != 0) {
    // Another context on THIS core is mid-split: wait (bounded) for it to
    // publish done; if it clears preparing without done (it failed) or the
    // bound elapses, cleanly fall back so we never proceed un-split.
    constexpr uint32_t kSplitSpinBudget = 1u << 16;
    for (uint32_t i = 0; i < kSplitSpinBudget; ++i) {
      if ((P->splitDoneMask.loadAcquire() & Bit) != 0)
        return true;
      if ((P->splitPreparingMask.loadAcquire() & Bit) == 0)
        break; // the other context finished without marking done -> it failed.
      cpuRelax();
    }
    bool done = (P->splitDoneMask.loadAcquire() & Bit) != 0;
    if (!done)
      EJIT_DIAG_VERBOSE(
          "ensurePoolSplit fallback: core=%u poolBase=0x%llx peer split "
          "did not publish done",
          self, static_cast<unsigned long long>(poolBase));
    return done;
  }

  // We own the split for this (pool, core). Run it, publishing done only on
  // success; on failure roll back preparing so a later hit can retry.
  bool ok = splitPoolFn_ && splitPoolFn_(splitPoolCtx_, poolBase, poolSize);
  if (ok) {
    P->splitDoneMask.fetchOr(Bit);
    P->splitPreparingMask.fetchAnd(~Bit);
    return true;
  }
  EJIT_DIAG(
      "ensurePoolSplit FAIL: core=%u poolBase=0x%llx split callback failed",
      self, static_cast<unsigned long long>(poolBase));
  P->splitPreparingMask.fetchAnd(~Bit);
  return false;
}

bool EJitSharedTaskPool::prepareExecForCurrentCore(const PeerCodeRange &R,
                                                   uint32_t self) {
  EJIT_DIAG_VERBOSE("prepareExec: core=%u fn=%p codeStart=0x%llx codeSize=%llu "
                    "poolBase=0x%llx fourK=%u",
                    self, R.fn, static_cast<unsigned long long>(R.codeStart),
                    static_cast<unsigned long long>(R.codeSize),
                    static_cast<unsigned long long>(R.poolBase),
                    static_cast<unsigned>(fourKSeal_));
  if (!fourKSeal_) {
    // Legacy whole-2MiB-pool seal: align fnPtr to its pool base and enable_ex
    // that page. Range metadata is not required (spec §6 — 2M unchanged). When
    // no prepare callback is wired the platform/host needs no per-core
    // preparation (e.g. the single shared address space of the host
    // simulation), so the pointer is already executable on this core.
    if (!prepareCodeFn_)
      return true;
    bool ok = prepareCodeFn_(prepareCodeCtx_, R.fn);
    if (!ok)
      EJIT_DIAG("prepareExec FAIL: core=%u legacy prepareCode fn=%p", self,
                R.fn);
    return ok;
  }

  // 4K page seal needs the real executable extent. A slot with no recorded
  // range (or a malformed one) is a clean fallback, never a guessed seal.
  if (R.codeStart == 0 || R.codeSize == 0 || R.poolBase == 0 ||
      R.poolSize == 0) {
    EJIT_DIAG_VERBOSE(
        "prepareExec fallback: core=%u fn=%p malformed range "
        "(codeStart=0x%llx codeSize=%llu poolBase=0x%llx poolSize=%llu)",
        self, R.fn, static_cast<unsigned long long>(R.codeStart),
        static_cast<unsigned long long>(R.codeSize),
        static_cast<unsigned long long>(R.poolBase),
        static_cast<unsigned long long>(R.poolSize));
    return false;
  }
  if (R.codeStart + R.codeSize < R.codeStart) { // code range overflow
    EJIT_DIAG_VERBOSE("prepareExec fallback: core=%u code range overflow",
                      self);
    return false;
  }
  if (R.poolBase + R.poolSize < R.poolBase) { // pool range overflow
    EJIT_DIAG_VERBOSE("prepareExec fallback: core=%u pool range overflow",
                      self);
    return false;
  }
  if (R.codeStart < R.poolBase ||
      R.codeStart + R.codeSize > R.poolBase + R.poolSize) {
    EJIT_DIAG_VERBOSE("prepareExec fallback: core=%u code not inside pool",
                      self);
    return false; // code must lie wholly inside its pool.
  }

  // This core must have split the 2MiB pool exactly once before sealing pages.
  if (!ensurePoolSplitForCurrentCore(self, R.poolBase, R.poolSize)) {
    EJIT_DIAG("prepareExec FAIL: core=%u pool split not done", self);
    return false;
  }

  const uintptr_t Page = static_cast<uintptr_t>(kEJitSharedSealPage);
  const uintptr_t CodePageStart = R.codeStart & ~(Page - 1);
  const uintptr_t CodePageEnd =
      (R.codeStart + static_cast<uintptr_t>(R.codeSize) + Page - 1) &
      ~(Page - 1);

  // STEP 1: prepare the runtime-writable data pages (e.g. Tier-1 __profc_)
  // FIRST, making them writable (enable_rw, RX -> RW) in THIS core's
  // translation context, before any executable page is sealed and before the
  // core-prepared bit is published. This is required ONLY for a fixed RX
  // code-segment pool (requiresPeerEnableRw): its pages are RX on every core at
  // load, so the JIT body's first counter atomicrmw would otherwise fault with
  // a write-permission abort. A dynamic SRE_MemDbgAlloc pool
  // (requiresPeerEnableRw=0) is already RW, so the writable ranges are
  // diagnostic only and are NOT flipped here (and no enable_rw callback is
  // required). Any anomaly on the fixed path is a clean fallback (no fnPtr, no
  // core bit) so a peer that cannot be made safe simply runs AOT.
  if (R.requiresPeerEnableRw && R.writableCount > 0) {
    if (R.writableCount > kEJitSharedMaxWritableRanges) {
      EJIT_DIAG("prepareExec fallback: core=%u writableCount=%u > max=%u", self,
                R.writableCount, kEJitSharedMaxWritableRanges);
      return false;
    }
    if (!enableRwPageFn_) {
      EJIT_DIAG("prepareExec FAIL: core=%u fixed RX pool needs enable_rw but "
                "no callback",
                self);
      return false;
    }
    for (uint32_t i = 0; i < R.writableCount; ++i) {
      const uintptr_t WStart = R.writables[i].addr;
      const uint64_t WSize = R.writables[i].size;
      if (WSize == 0)
        continue;
      if (WStart + static_cast<uintptr_t>(WSize) < WStart) {
        EJIT_DIAG_VERBOSE(
            "prepareExec fallback: core=%u writable range overflow", self);
        return false;
      }
      // Must lie wholly inside the code's pool (same split granule) so the page
      // is already split to 4K in this core's translation.
      if (WStart < R.poolBase ||
          WStart + static_cast<uintptr_t>(WSize) > R.poolBase + R.poolSize) {
        EJIT_DIAG_VERBOSE("prepareExec fallback: core=%u writable not in pool",
                          self);
        return false;
      }
      const uintptr_t WPageStart = WStart & ~(Page - 1);
      const uintptr_t WPageEnd =
          (WStart + static_cast<uintptr_t>(WSize) + Page - 1) & ~(Page - 1);
      // W^X guard: a writable range must NEVER share a 4KiB page with the
      // executable extent, or enable_rw would make a code page writable (RWX).
      // The finalize layout guarantees this; re-verify so a malformed/hostile
      // slot cannot bypass it.
      if (WPageStart < CodePageEnd && CodePageStart < WPageEnd) {
        EJIT_DIAG("prepareExec FAIL: core=%u writable page overlaps code "
                  "(wStart=0x%llx codeStart=0x%llx) - refusing RWX",
                  self, static_cast<unsigned long long>(WStart),
                  static_cast<unsigned long long>(R.codeStart));
        return false;
      }
      for (uintptr_t VA = WPageStart; VA < WPageEnd; VA += Page)
        if (!enableRwPageFn_(enableRwPageCtx_, VA)) {
          EJIT_DIAG("prepareExec FAIL: core=%u enable_rw pageVA=0x%llx", self,
                    static_cast<unsigned long long>(VA));
          return false; // failed enable_rw -> no callable pointer, no core bit.
        }
    }
    EJIT_DIAG_VERBOSE("prepareExec: core=%u writable pages prepared count=%u",
                      self, R.writableCount);
  }

  // STEP 2: seal every 4KiB page the code overlaps (enable_ex, RX): page-align
  // start down, end up. Only after BOTH the writable and executable pages are
  // prepared may the caller publish this core's prepared bit.
  for (uintptr_t VA = CodePageStart; VA < CodePageEnd; VA += Page)
    if (!sealPageFn_ || !sealPageFn_(sealPageCtx_, VA)) {
      EJIT_DIAG("prepareExec FAIL: core=%u sealPage pageVA=0x%llx", self,
                static_cast<unsigned long long>(VA));
      return false; // any page failure -> no callable pointer is returned.
    }
  EJIT_DIAG_VERBOSE("prepareExec OK: core=%u fn=%p pages=[0x%llx,0x%llx)", self,
                    R.fn, static_cast<unsigned long long>(CodePageStart),
                    static_cast<unsigned long long>(CodePageEnd));
  return true;
}

bool EJitSharedTaskPool::cacheHasPending(uint32_t funcIndex,
                                         const EJitDimPair *dims,
                                         uint32_t numDims) {
  if (!state_ || numDims > 4)
    return false;
  const uint64_t Key = hashIdentity(funcIndex, dims, numDims);
  EJitSharedCacheBucket &B =
      state_->buckets[static_cast<uint32_t>(Key % kEJitSharedCacheBuckets)];
  for (unsigned Attempt = 0; Attempt != 4; ++Attempt) {
    if (!bucketTryRead(B)) {
      cpuRelax();
      continue;
    }
    const uint32_t Gen = state_->generation.loadAcquire();
    bool Found = false;
    for (uint32_t S = 0; S < kEJitSharedCacheSlots; ++S) {
      EJitSharedCacheSlot &Slot = B.slots[S];
      if (Slot.identityHash != Key ||
          Slot.state.loadAcquire() !=
              static_cast<uint32_t>(EJitSharedSlotState::Pending) ||
          Slot.generation != Gen ||
          !slotIdentityMatches(Slot, funcIndex, dims, numDims))
        continue;
      Found = true;
      for (uint32_t I = 0; I < numDims; ++I)
        if (Slot.versions[I] !=
            instanceVersion(dims[I].dimType, dims[I].instanceId)) {
          Found = false;
          break;
        }
      break;
    }
    bucketReadRelease(B);
    return Found;
  }
  return false;
}

EJitPublishStatus
EJitSharedTaskPool::cacheStageBatchRequest(const EJitCompileRequest &req) {
  if (!state_ || req.numDims > 4)
    return EJitPublishStatus::InvalidParam;
  const uint32_t FuncIndex = stripReqTier(req.funcIndex);
  const uint64_t Key = hashIdentity(FuncIndex, req.dims, req.numDims);
  EJitSharedCacheBucket &B =
      state_->buckets[static_cast<uint32_t>(Key % kEJitSharedCacheBuckets)];
  bucketWrite(B);

  if (req.generation != state_->generation.loadAcquire() ||
      !versionsCurrent(req)) {
    bucketWriteRelease(B);
    return EJitPublishStatus::VersionMismatch;
  }

  EJitSharedCacheSlot *Target = nullptr;
  for (uint32_t S = 0; S < kEJitSharedCacheSlots; ++S) {
    EJitSharedCacheSlot &Slot = B.slots[S];
    const uint32_t State = Slot.state.loadAcquire();
    if (State != static_cast<uint32_t>(EJitSharedSlotState::Empty) &&
        Slot.generation == req.generation &&
        slotIdentityMatches(Slot, FuncIndex, req.dims, req.numDims)) {
      if (State == static_cast<uint32_t>(EJitSharedSlotState::Pending)) {
        Target = &Slot;
        break;
      }
      // Do not create a second slot for an identity whose old executable
      // version is still Ready. Defer it under the coarse claim instead.
      bucketWriteRelease(B);
      return EJitPublishStatus::Failed;
    }
    if (!Target && State == static_cast<uint32_t>(EJitSharedSlotState::Empty))
      Target = &Slot;
  }
  // A request-only marker must not evict executable code merely to improve
  // layout. Keep the coarse in-flight claim in that rare collision case.
  if (!Target) {
    bucketWriteRelease(B);
    return EJitPublishStatus::Failed;
  }

  Target->state.storeRelease(
      static_cast<uint32_t>(EJitSharedSlotState::Publishing));
  Target->funcIndex = FuncIndex;
  Target->numDims = req.numDims;
  Target->generation = req.generation;
  Target->identityHash = Key;
  for (uint32_t I = 0; I < 4; ++I) {
    Target->dims[I] = I < req.numDims ? req.dims[I] : EJitDimPair{0, 0};
    Target->versions[I] = I < req.numDims ? req.versions[I] : 0;
  }
  Target->codeStart = 0;
  Target->codeSize = 0;
  Target->fnSize = 0;
  Target->poolBase = 0;
  Target->poolSize = 0;
  Target->poolId = 0;
  Target->executableCoreMask.storeRelease(0);
  Target->fnPtr.storeRelease(0);
  Target->state.storeRelease(
      static_cast<uint32_t>(EJitSharedSlotState::Pending));
  bucketWriteRelease(B);
  return EJitPublishStatus::Published;
}

EJitPublishStatus
EJitSharedTaskPool::cacheStagePending(const EJitCompileRequest &req,
                                      void *fnPtr) {
  if (!fnPtr || req.numDims > 4)
    return EJitPublishStatus::InvalidParam;
  const uint32_t FuncIndex = stripReqTier(req.funcIndex);
  const uint64_t Key = hashIdentity(FuncIndex, req.dims, req.numDims);
  EJitSharedCacheBucket &B =
      state_->buckets[static_cast<uint32_t>(Key % kEJitSharedCacheBuckets)];
  bucketWrite(B);

  if (req.generation != state_->generation.loadAcquire() ||
      !versionsCurrent(req)) {
    bucketWriteRelease(B);
    return EJitPublishStatus::VersionMismatch;
  }

  EJitSharedCacheSlot *Target = nullptr;
  EJitSharedCacheSlot *FirstEmpty = nullptr;
  EJitSharedCacheSlot *ReadyVictim = nullptr;
  for (uint32_t S = 0; S < kEJitSharedCacheSlots; ++S) {
    EJitSharedCacheSlot &Slot = B.slots[S];
    const uint32_t State = Slot.state.loadAcquire();
    if (State != static_cast<uint32_t>(EJitSharedSlotState::Empty) &&
        Slot.generation == req.generation &&
        slotIdentityMatches(Slot, FuncIndex, req.dims, req.numDims)) {
      Target = &Slot;
      break;
    }
    if (!FirstEmpty &&
        State == static_cast<uint32_t>(EJitSharedSlotState::Empty))
      FirstEmpty = &Slot;
    if (!ReadyVictim &&
        State == static_cast<uint32_t>(EJitSharedSlotState::Ready))
      ReadyVictim = &Slot;
  }
  if (!Target)
    Target = FirstEmpty ? FirstEmpty : ReadyVictim;
  // Never evict another Pending identity: peers would lose the complete-key
  // dedup marker and could enqueue duplicate code before the manual flush.
  if (!Target) {
    bucketWriteRelease(B);
    return EJitPublishStatus::Failed;
  }

  void *OldFn = reinterpret_cast<void *>(Target->fnPtr.loadAcquire());
  Target->state.storeRelease(
      static_cast<uint32_t>(EJitSharedSlotState::Publishing));
  Target->funcIndex = FuncIndex;
  Target->numDims = req.numDims;
  Target->generation = req.generation;
  Target->identityHash = Key;
  for (uint32_t I = 0; I < req.numDims; ++I) {
    Target->dims[I] = req.dims[I];
    Target->versions[I] = req.versions[I];
  }
  Target->codeStart = 0;
  Target->codeSize = 0;
  Target->fnSize = 0;
  Target->poolBase = 0;
  Target->poolSize = 0;
  Target->poolId = 0;
  Target->executableCoreMask.storeRelease(0);
  Target->fnPtr.storeRelease(reinterpret_cast<uintptr_t>(fnPtr));
  Target->state.storeRelease(
      static_cast<uint32_t>(EJitSharedSlotState::Pending));
  state_->dispatchEpoch.fetchAdd(1);
  bucketWriteRelease(B);

  if (releaseFn_ && OldFn && OldFn != fnPtr)
    releaseFn_(releaseCtx_, OldFn);
  return EJitPublishStatus::Published;
}

void EJitSharedTaskPool::cacheDropPending(const EJitCompileRequest &req,
                                          void *fnPtr) {
  const uint32_t FuncIndex = stripReqTier(req.funcIndex);
  const uint64_t Key = hashIdentity(FuncIndex, req.dims, req.numDims);
  EJitSharedCacheBucket &B =
      state_->buckets[static_cast<uint32_t>(Key % kEJitSharedCacheBuckets)];
  bucketWrite(B);
  for (uint32_t S = 0; S < kEJitSharedCacheSlots; ++S) {
    EJitSharedCacheSlot &Slot = B.slots[S];
    if (Slot.state.loadAcquire() !=
            static_cast<uint32_t>(EJitSharedSlotState::Pending) ||
        Slot.generation != req.generation ||
        Slot.fnPtr.loadAcquire() != reinterpret_cast<uintptr_t>(fnPtr) ||
        !slotIdentityMatches(Slot, FuncIndex, req.dims, req.numDims))
      continue;
    Slot.state.storeRelease(
        static_cast<uint32_t>(EJitSharedSlotState::Publishing));
    Slot.fnPtr.storeRelease(0);
    Slot.identityHash = 0;
    Slot.executableCoreMask.storeRelease(0);
    Slot.codeStart = 0;
    Slot.codeSize = 0;
    Slot.fnSize = 0;
    Slot.poolBase = 0;
    Slot.poolSize = 0;
    Slot.poolId = 0;
    Slot.state.storeRelease(static_cast<uint32_t>(EJitSharedSlotState::Empty));
    state_->dispatchEpoch.fetchAdd(1);
    break;
  }
  bucketWriteRelease(B);
}

EJitPublishStatus
EJitSharedTaskPool::cachePublish(const EJitCompileRequest &req, void *fnPtr,
                                 const EJitCompiledCodeInfo *info,
                                 bool pgoClearExclusive) {
  if (!fnPtr || req.numDims > 4)
    return EJitPublishStatus::InvalidParam;
  // Over-bound writable set: REJECT the publish outright (before touching any
  // slot). Clamping would drop counter pages a peer must enable_rw, so the peer
  // would under-prepare and fault. Rejecting here means no slot goes Ready, no
  // fnPtr is published, and no executableCoreMask bit is set for this identity.
  if (info && info->writableCount > kEJitSharedMaxWritableRanges) {
    EJIT_DIAG("cachePublish REJECT: writableCount=%u > max=%u",
              info->writableCount, kEJitSharedMaxWritableRanges);
    return EJitPublishStatus::InvalidParam;
  }
  uint32_t tier = decodeReqTier(req.funcIndex);
  uint32_t fidx = stripReqTier(req.funcIndex);
  uint64_t key = hashIdentity(fidx, req.dims, req.numDims);
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];

  bucketWrite(B, pgoClearExclusive);

  // Commit gate (§5.3/§5.4): re-verify the version snapshot under the lock.
  for (uint32_t i = 0; i < req.numDims; ++i)
    if (req.versions[i] !=
        instanceVersion(req.dims[i].dimType, req.dims[i].instanceId)) {
      bucketWriteRelease(B);
      return EJitPublishStatus::VersionMismatch;
    }

  uint32_t curGen = state_->generation.loadAcquire();
  // Generation gate (spec §11): use the REQUEST's generation, never silently
  // substitute the current one. A request whose generation has been superseded
  // (owner re-init between enqueue and publish) is rejected here.
  if (req.generation != curGen) {
    bucketWriteRelease(B);
    return EJitPublishStatus::VersionMismatch;
  }
  EJitSharedCacheSlot *target = nullptr;
  EJitSharedCacheSlot *firstEmpty = nullptr;
  EJitSharedCacheSlot *evict = nullptr;
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    uint32_t st = Slot.state.loadAcquire();
    if (st != static_cast<uint32_t>(EJitSharedSlotState::Empty) &&
        Slot.generation == req.generation &&
        slotIdentityMatches(Slot, fidx, req.dims, req.numDims)) {
      target = &Slot; // overwrite same identity in place
      break;
    }
    if (!firstEmpty && st == static_cast<uint32_t>(EJitSharedSlotState::Empty))
      firstEmpty = &Slot;
    if (!evict && st == static_cast<uint32_t>(EJitSharedSlotState::Ready))
      evict = &Slot; // never evict a different pending batch identity
  }
  if (!target)
    target = firstEmpty ? firstEmpty : evict; // bucket full → evict slot 0-ish.
  if (!target) {
    bucketWriteRelease(B);
    return EJitPublishStatus::Failed;
  }

  void *oldFn = reinterpret_cast<void *>(target->fnPtr.loadAcquire());

  target->state.storeRelease(
      static_cast<uint32_t>(EJitSharedSlotState::Publishing));
  target->funcIndex = fidx;
  target->numDims = req.numDims;
  target->generation = req.generation; // the request's generation, not curGen
  target->identityHash = key;
  for (uint32_t i = 0; i < req.numDims; ++i) {
    target->dims[i] = req.dims[i];
    target->versions[i] = req.versions[i];
  }
  // Copy the real executable range (v5) so a peer core can later seal exactly
  // the 4KiB pages this code covers. Written BEFORE the fnPtr/state release
  // below, so an acquiring reader that observes state==Ready also sees a
  // consistent range. A null/empty info clears the range (0 codeSize), which a
  // peer treats as "no range -> clean fallback".
  if (info && info->codeSize != 0) {
    target->codeStart = info->codeStart;
    target->codeSize = info->codeSize;
    target->fnSize = info->fnSize;
    target->poolBase = info->poolBase;
    target->poolSize = info->poolSize;
    target->poolId = info->poolId;
    target->poolKind = static_cast<uint32_t>(info->poolKind);
    // Runtime-writable extents (v9): copy the bounded set the peer may need to
    // enable_rw. The over-bound case was already rejected at entry, so this
    // never truncates. requiresPeerEnableRw records whether the peer must
    // actually flip these writable (fixed RX pool) or they are diagnostic only
    // (dynamic RW pool).
    uint32_t wc = info->writableCount;
    target->writableCount = wc;
    target->requiresPeerEnableRw = info->requiresPeerEnableRw ? 1u : 0u;
    for (uint32_t i = 0; i < kEJitSharedMaxWritableRanges; ++i) {
      if (i < wc) {
        target->writableRanges[i].addr = info->writableRanges[i].addr;
        target->writableRanges[i].size = info->writableRanges[i].size;
      } else {
        target->writableRanges[i].addr = 0;
        target->writableRanges[i].size = 0;
      }
    }
  } else {
    target->codeStart = 0;
    target->codeSize = 0;
    target->fnSize = 0;
    target->poolBase = 0;
    target->poolSize = 0;
    target->poolId = 0;
    target->poolKind = static_cast<uint32_t>(EJitCodePoolKind::Unknown);
    target->writableCount = 0;
    target->requiresPeerEnableRw = 0;
    for (uint32_t i = 0; i < kEJitSharedMaxWritableRanges; ++i) {
      target->writableRanges[i].addr = 0;
      target->writableRanges[i].size = 0;
    }
  }
  target->fnPtr.storeRelease(reinterpret_cast<uintptr_t>(fnPtr));
  // PGO: reset hitCount on every publish.  Record the compile tier so
  // resolveMatchedSlot can suppress the Tier-2 trigger on already-Tier-2
  // slots (§4 repeat-trigger).  Under the write lock + before state=Ready.
  target->hitCount.storeRelaxed(0);
  target->tier.storeRelaxed(static_cast<uint8_t>(tier));
  target->postPublishSeen.storeRelaxed(0);
  const uint32_t OwnerCore = state_->ownerCoreId.loadAcquire();
  target->executableCoreMask.storeRelease(
      OwnerCore < 64 ? (uint64_t{1} << OwnerCore) : 0);
  target->state.storeRelease(static_cast<uint32_t>(EJitSharedSlotState::Ready));
  // Published a new fnPtr, or evicted the slot that held one.
  state_->dispatchEpoch.fetchAdd(1);
  bucketWriteRelease(B);

  // Release the slot's PREVIOUS code OUTSIDE the bucket lock (the callback may
  // re-enter the code pool / ORC / allocator / platform). This covers both a
  // same-identity recompile (new address replaces old) and the eviction of a
  // different identity when the bucket was full. Readers already drained to 0
  // under the write lock and the slot now points at the new code, so the old
  // pointer is unreachable and safe to free.
  if (releaseFn_ && oldFn && oldFn != fnPtr)
    releaseFn_(releaseCtx_, oldFn);
  return EJitPublishStatus::Published;
}

void EJitSharedTaskPool::releaseRead(uint32_t bucketIndex) {
  // Served from the L0: no token was taken.
  if (bucketIndex == kEJitNoBucket)
    return;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // A load-only seqlock hit takes no read token and deliberately returns the
  // one-past-the-end bucket as a sentinel. Wrappers still call releaseRead()
  // uniformly, so this is an expected no-op rather than a rejected release.
  if (bucketIndex == kEJitSharedCacheBuckets)
    return;
#endif
  if (!state_ || bucketIndex >= kEJitSharedCacheBuckets) {
    EJIT_DIAG("shared releaseRead reject: state=%p bucket=%u (max=%u)",
              (void *)state_, bucketIndex, kEJitSharedCacheBuckets);
    return;
  }
  bucketReadRelease(state_->buckets[bucketIndex]);
}

//===----------------------------------------------------------------------===//
// Owner election + init (§11).
//===----------------------------------------------------------------------===//
namespace {
// Field-by-field init so it is correct on raw, uninitialized shared memory
// (never relies on a C++ constructor having run on the blob).
//
// enabled defaults to 0 (inactive): a period instance must be explicitly
// activated via ejit_activate before the JIT will compile it. This matches
// the non-shared EJitRuntimeState::isActive default (no entry => inactive).
// setInstanceEnabled(true) flips 0->1 and bumps version on first activate.
void initSharedStorage(EJitSharedTaskPoolState *st, uint32_t mode,
                       uint32_t pgoEnabled, uint32_t tier2Threshold,
                       uint32_t pgoMaxConcurrentProfiles) {
  for (uint32_t d = 0; d < kEJitSharedDimTypes; ++d)
    for (uint32_t i = 0; i < kEJitSharedInstances; ++i) {
      st->enabled[d][i].storeRelaxed(0);
      st->version[d][i].storeRelaxed(0);
    }
  st->mode.storeRelaxed(mode);
  st->tier2Threshold.storeRelaxed(pgoEnabled ? tier2Threshold : 0);
  st->pgoEnabled.storeRelaxed(pgoEnabled ? 1 : 0);
  st->pgoAdmissionLock.storeRelaxed(0);
  st->pgoMaxActiveFunctions.storeRelaxed(pgoMaxConcurrentProfiles);
  st->pgoActiveFunctionCount.storeRelaxed(0);
  for (uint32_t i = 0; i < kEJitSharedMaxConcurrentProfiles; ++i) {
    st->pgoActiveFunctions[i].storeRelaxed(0);
    st->pgoProgressQuarters[i].storeRelaxed(0);
  }
  st->pgoCompletedFunctions.storeRelaxed(0);
  st->pgoDeferredMisses.storeRelaxed(0);
  st->anyInstanceActivated.storeRelaxed(0);
  // dispatchEpoch is BUMPED, never reset. A per-core L0 entry filled under a
  // previous pool instance must not validate under this one, and the entries
  // outlive the shared blob (they live in core-private memory). Monotonic
  // bumping guarantees the mismatch; resetting to a fixed value would let a
  // stale entry match again after a re-init. On a genuinely fresh blob the
  // starting value is arbitrary, which is harmless: the L0 tables are zeroed
  // BSS and l0Key() never returns 0, so no entry can match anyway.
  st->dispatchEpoch.fetchAdd(1);
  st->mayConstRankingRequest.storeRelaxed(0);
  st->mayConstRankingComplete.storeRelaxed(0);
  st->mayConstRankingResult.storeRelaxed(0);
  st->codeBatchRequestLock.storeRelaxed(0);
  st->codeBatchRequestState.storeRelaxed(
      static_cast<uint32_t>(EJitCodeBatchRequestState::Idle));
  // icacheDrainSeq is likewise BUMPED, never reset: a core that snapshotted it
  // before this (re)initialization must not find its snapshot still current
  // afterwards and fill a cell with a pointer from the previous generation.
  st->icacheDrainSeq.fetchAdd(1);
  // NOTE: icacheDrainsInFlight is deliberately NOT reset here. It is cleared by
  // the owner AFTER it publishes the new generation, so a drain still running
  // under the old one can observe the change and skip its decrement. Clearing
  // it here -- before the generation moves -- leaves a window in which the
  // straggler still believes the generation is its own and decrements a counter
  // that was just zeroed, wrapping it to UINT32_MAX.
  //
  // Zeroing these lets a fresh blob start from "no per-core preparation, no
  // releaser" instead of inheriting a dead generation's answer.
  //
  // Re-publication is NOT symmetric, and the asymmetry is a known limit rather
  // than an oversight. Seal mode is set-only and monotone, so any facade that
  // still needs per-core preparation re-asserts it on its next
  // setSealMode()/setPrepareCodeCallback(), and init() re-asserts the owner's.
  // The releaser count is only re-published by init(), and only for the owner's
  // own releaseFn_: bind() runs once per facade, so a PEER facade that wired a
  // releaser before the re-init is not restored into the new generation's
  // count. Production keeps one facade per blob
  // (EJitCompileDriver::sharedPool_), which makes that unreachable; see
  // syncIcacheReleaserCount() for what closing it properly would take.
  // A fresh generation starts with an empty table, so nothing is armed.
  st->icacheArmed.storeRelaxed(0);
  st->icachePerCorePrepare.storeRelaxed(0);
  st->icacheReleasersWired.storeRelaxed(0);
  for (uint32_t i = 0; i < kEJitSharedMaxFuncIndex; ++i)
    st->inFlight[i].storeRelaxed(0);
  for (uint32_t i = 0; i < kEJitSharedQueueSlots; ++i) {
    st->ring[i].sequence.storeRelaxed(i); // Vyukov initial sequence = index
  }
  st->enqueuePos.storeRelaxed(0);
  st->dequeuePos.storeRelaxed(0);
  st->counters.cacheHits.storeRelaxed(0);
  st->counters.asyncCompiles.storeRelaxed(0);
  st->counters.asyncEnqueues.storeRelaxed(0);
  st->counters.alreadyPending.storeRelaxed(0);
  st->counters.queueFull.storeRelaxed(0);
  st->counters.compileFailed.storeRelaxed(0);
  st->counters.publishFailed.storeRelaxed(0);
  st->counters.instanceDisabled.storeRelaxed(0);
  st->counters.instanceDisabledPreActivate.storeRelaxed(0);
  st->counters.executePrepareFailed.storeRelaxed(0);
  st->counters.tier1Compiles.storeRelaxed(0);
  st->counters.tier2Compiles.storeRelaxed(0);
  st->counters.profileMergeFails.storeRelaxed(0);
  // Code-pool stats mirror: zero until the owner publishes the first snapshot
  // after a successful compile (the pools are owner-private and empty at init).
  st->codePoolStats.poolCount.storeRelaxed(0);
  st->codePoolStats.sealedCount.storeRelaxed(0);
  st->codePoolStats.activeCount.storeRelaxed(0);
  st->codePoolStats.usedBytes.storeRelaxed(0);
  st->codePoolStats.reservedBytes.storeRelaxed(0);
  st->codePoolStats.wastedBytes.storeRelaxed(0);
  st->codePoolStats.sealInvocations.storeRelaxed(0);
  st->codePoolStats.splitInvocations.storeRelaxed(0);
  st->codePoolStats.finalizedRangeCount.storeRelaxed(0);
  auto ClearPoolDetail = [](EJitSharedCodePoolStats::Detail &D) {
    D.poolCount.storeRelaxed(0);
    D.sealedCount.storeRelaxed(0);
    D.activeCount.storeRelaxed(0);
    D.usedBytes.storeRelaxed(0);
    D.reservedBytes.storeRelaxed(0);
    D.wastedBytes.storeRelaxed(0);
    D.sealInvocations.storeRelaxed(0);
    D.splitInvocations.storeRelaxed(0);
    D.finalizedRangeCount.storeRelaxed(0);
    D.baseAddress.storeRelaxed(0);
    D.endAddress.storeRelaxed(0);
    D.pendingBytes.storeRelaxed(0);
    D.pendingRangeCount.storeRelaxed(0);
    D.fallbackCount.storeRelaxed(0);
    D.full.storeRelaxed(0);
  };
  ClearPoolDetail(st->codePoolStats.near);
  ClearPoolDetail(st->codePoolStats.far);
  for (uint32_t I = 0; I < kEJitNearHotPoolCount; ++I)
    ClearPoolDetail(st->codePoolStats.nearHot[I]);
  // Per-core, per-pool 4K split readiness (ABI v5). MUST be cleared on every
  // (re)initialization: a stale splitDone bit from an earlier generation would
  // otherwise make a peer skip split_2m_to_4k for a pool the new generation
  // rebuilt, so it would seal pages that were never split. Cleared, the first
  // post-reinit hit re-runs the split on each core.
  for (uint32_t i = 0; i < kEJitSharedPoolSlots; ++i) {
    st->poolSplits[i].poolBase.storeRelaxed(0);
    st->poolSplits[i].splitDoneMask.storeRelaxed(0);
    st->poolSplits[i].splitPreparingMask.storeRelaxed(0);
  }
  st->dump.lock.storeRelaxed(0);
  st->dump.filterEnabled.storeRelaxed(0);
  st->dump.hasDump.storeRelaxed(0);
  st->dump.status.storeRelaxed(0);
  st->dump.filterLen = 0;
  st->dump.resultNameLen = 0;
  st->dump.irSize = 0;
  st->dump.asmSize = 0;
  st->dump.keyHi = 0;
  st->dump.keyLo = 0;
  st->dump.workerCore = kEJitInvalidCoreId;
  st->dump.reserved0 = 0;
  for (uint32_t i = 0; i < kEJitSharedDumpNameBytes; ++i) {
    st->dump.filterName[i] = 0;
    st->dump.resultName[i] = 0;
  }
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b) {
    st->buckets[b].writeFlag.storeRelaxed(0);
    st->buckets[b].readers.storeRelaxed(0);
    st->buckets[b].publishSeq.storeRelaxed(0); // even => no publish in flight
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = st->buckets[b].slots[s];
      Slot.state.storeRelaxed(
          static_cast<uint32_t>(EJitSharedSlotState::Empty));
      Slot.funcIndex = 0;
      Slot.numDims = 0;
      Slot.generation = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        Slot.dims[i] = EJitDimPair{0, 0};
        Slot.versions[i] = 0;
      }
      Slot.identityHash = 0;
      Slot.fnPtr.storeRelaxed(0);
      Slot.executableCoreMask.storeRelaxed(0);
      // Executable-range metadata (ABI v5): cleared so a stale range from an
      // earlier generation can never be read back after a re-init.
      Slot.codeStart = 0;
      Slot.codeSize = 0;
      Slot.fnSize = 0;
      Slot.poolBase = 0;
      Slot.poolSize = 0;
      Slot.poolId = 0;
      Slot.poolKind = static_cast<uint32_t>(EJitCodePoolKind::Unknown);
      // Runtime-writable ranges (ABI v9): cleared so a re-init never leaves a
      // stale writable extent a peer could enable_rw for retired code.
      Slot.writableCount = 0;
      Slot.requiresPeerEnableRw = 0;
      for (uint32_t i = 0; i < kEJitSharedMaxWritableRanges; ++i) {
        Slot.writableRanges[i].addr = 0;
        Slot.writableRanges[i].size = 0;
      }
      // PGO (§6): clear hitCount + tier so a re-init never leaks stale
      // hotness state or tier-tracking from a prior generation.
      Slot.hitCount.storeRelaxed(0);
      Slot.tier.storeRelaxed(0);
      Slot.postPublishSeen.storeRelaxed(0);
    }
  }
}
} // namespace

EJitSharedTaskPool::InitResult EJitSharedTaskPool::init() {
  if (!state_) {
    EJIT_DIAG("shared taskpool init FAILED: no shared state bound");
    return InitResult::NoState;
  }
  EJIT_DIAG("shared taskpool init: state=%p fingerprint=0x%llx",
            static_cast<void *>(state_),
            static_cast<unsigned long long>(regFingerprint_));
#ifdef EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE
  EJIT_DIAG("shared taskpool init: fixed worker core=%u (open election "
            "disabled; this core=%u)",
            kEJitFixedWorkerCore, EJitCoreId::current());
#endif

  // Bounded retry so an in-progress peer never deadlocks us.
  constexpr uint32_t kMaxSpins = 1u << 20;
  for (uint32_t spin = 0; spin < kMaxSpins; ++spin) {
    uint32_t st = state_->initState.loadAcquire();
    switch (static_cast<EJitSharedInitState>(st)) {
    case EJitSharedInitState::Uninitialized: {
      // Fixed worker core: only the designated core may claim the blob. A peer
      // that activates FIRST (bring-up order is unspecified) must not win the
      // election - it waits for the designated core to run its own init, with
      // the same bounded yield protocol the Initializing case uses, so a
      // high-priority peer never starves the designated core. The budget
      // expiring means the deployment never activated the designated core: fail
      // init cleanly (never hang, never silently elect a core the build pinned
      // the worker away from). A peer never counts as an election attempt.
      if (kEJitFixedWorkerCore != kEJitInvalidCoreId &&
          EJitCoreId::current() != kEJitFixedWorkerCore) {
        if (workerIdle_)
          workerIdle_(workerIdleCtx_, 1); // single yield while waiting
        else
          cpuRelax();
        break; // re-observe
      }
      state_->initAttempts.fetchAdd(1);
      uint32_t expected =
          static_cast<uint32_t>(EJitSharedInitState::Uninitialized);
      if (!state_->initState.compareExchange(
              expected,
              static_cast<uint32_t>(EJitSharedInitState::Initializing)))
        break; // lost the race; re-observe.

      // We are the owner. Build the whole blob, then publish Ready LAST.
      uint32_t self = EJitCoreId::current();
      uint32_t nextGen = state_->generation.loadRelaxed() + 1;
      // Owner-private batch state must never cross a generation boundary.
#ifndef EJIT_CODE_POOL_FIXED_NEAR_HOT
      pendingBatchCompiles_.clear();
#endif
      pendingPublishes_.clear();
      autoTier2PublishPending_ = false;
      autoTier2PublishBlocked_ = false;
#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
      nearHotFirstLinkedDiagnosed_ = false;
      nearHotFirstFlushDiagnosed_ = false;
#endif
      initSharedStorage(state_, static_cast<uint32_t>(configuredMode_),
                        pgoEnabled_.loadRelaxed(),
                        tier2Threshold_.loadRelaxed(),
                        pgoMaxConcurrentProfiles_.loadRelaxed());
      // Empty the shared cell table for the new generation: after a re-init
      // that skipped ownerShutdown the cells hold pointers into the previous
      // generation's code, and a cell carries no epoch to invalidate against.
      icacheDrainAll("owner-init");
      state_->generation.storeRelease(nextGen);
      // Now that the new generation is visible, discard any in-flight count
      // left by drains of the previous one: they will observe the generation
      // change and skip their own decrement, so this cannot be undone by a
      // straggler. Ordering matters -- see initSharedStorage().
      state_->icacheDrainsInFlight.storeRelaxed(0);
      state_->ownerCoreId.storeRelease(self);
      state_->codeSharingEnabled.storeRelease(codeSharingEnabled_ ? 1u : 0u);
      // initSharedStorage() just zeroed these for the new generation, so the
      // owner's own answers have to go back in before any peer can bind and
      // read them. Seal mode / prepareCode decide whether the 0-dim shared
      // scalar may be armed at all, and a releaser wired before election still
      // has to disable the table.
      publishIcachePrepareMode();
      if (releaseFn_)
        state_->icacheReleasersWired.storeRelease(1);
      state_->lastInitError.storeRelease(0);
      state_->workerTaskId.storeRelease(0);
      // Publish the owner's funcIndex/dimType registration digest so peers can
      // reject a divergent mapping before submitting any request (spec §11).
      state_->registrationFingerprint.storeRelease(regFingerprint_);
      state_->magic = kEJitSharedAbiMagic;
      state_->abiVersion = kEJitSharedAbiVersion;
      state_->structSize =
          static_cast<uint32_t>(sizeof(EJitSharedTaskPoolState));

      // Owner-only setup (building the JIT engine) runs HERE: after the blob is
      // built, before the worker exists, and before Ready is published. Both
      // orderings matter -- the worker can compile the instant it starts, and a
      // peer can enqueue the instant it observes Ready. A failure is a clean
      // init failure, exactly like a failed worker start.
      if (ownerElected_ && !ownerElected_(ownerElectedCtx_)) {
        state_->lastInitError.storeRelease(
            static_cast<uint32_t>(EJitSharedInitError::OwnerSetupFailed));
        state_->initState.storeRelease(
            static_cast<uint32_t>(EJitSharedInitState::Failed));
        EJIT_DIAG("shared taskpool owner=%u setup FAILED (engine)", self);
        return InitResult::OwnerFailed;
      }

      // Start the ONE worker (if a starter was injected). A failure here is a
      // clean init failure: record it, publish Failed, and DO NOT pretend JIT
      // is up.
#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
      diagnoseCodeBatchCallbacks("pre-worker", workerCtx_);
#endif
      bool workerOk = true;
      if (workerStart_) {
        uint64_t taskId = 0;
        workerOk = workerStart_(
            workerCtx_, &EJitSharedTaskPool::workerEntryThunk, this, &taskId);
        if (workerOk)
          state_->workerTaskId.storeRelease(taskId);
      }
      if (!workerOk) {
        state_->lastInitError.storeRelease(
            static_cast<uint32_t>(EJitSharedInitError::WorkerStartFailed));
        state_->initState.storeRelease(
            static_cast<uint32_t>(EJitSharedInitState::Failed));
        EJIT_DIAG("shared taskpool owner=%u worker start FAILED", self);
        return InitResult::OwnerFailed;
      }
      isOwner_ = true;
      state_->initState.storeRelease(
          static_cast<uint32_t>(EJitSharedInitState::Ready)); // publish last
      EJIT_DIAG("shared taskpool owner=%u gen=%u ready", self, nextGen);
      return InitResult::BecameOwner;
    }
    case EJitSharedInitState::Initializing:
      // A peer racing the owner yields (not busy-spins) so a high-priority peer
      // never starves the owner core trying to finish init + publish Ready.
      if (workerIdle_)
        workerIdle_(workerIdleCtx_, 1); // single yield while owner publishes
      else
        cpuRelax();
      break;
    case EJitSharedInitState::Ready:
      if (state_->magic != kEJitSharedAbiMagic ||
          state_->abiVersion != kEJitSharedAbiVersion ||
          state_->structSize != sizeof(EJitSharedTaskPoolState)) {
        EJIT_DIAG("shared taskpool attach REJECTED: ABI mismatch "
                  "(magic=0x%x ver=%u size=%u exp_size=%zu)",
                  state_->magic, state_->abiVersion, state_->structSize,
                  sizeof(EJitSharedTaskPoolState));
        return InitResult::AbiMismatch;
      }
      // Registration consistency: a peer whose funcIndex/dimType mapping digest
      // differs from the owner's must NOT submit requests against mismatched
      // indices. Clean-fail instead (the owner itself re-observes its own
      // fingerprint, so this never rejects the owner).
      if (state_->registrationFingerprint.loadAcquire() != regFingerprint_) {
        EJIT_DIAG("shared taskpool attach REJECTED: registration fingerprint "
                  "mismatch (owner=%llu self=%llu)",
                  static_cast<unsigned long long>(
                      state_->registrationFingerprint.loadAcquire()),
                  static_cast<unsigned long long>(regFingerprint_));
        return InitResult::FingerprintMismatch;
      }
      EJIT_DIAG("shared taskpool attached ready (owner=%u)",
                state_->ownerCoreId.loadAcquire());
      return InitResult::AttachedReady;
    case EJitSharedInitState::Failed:
      EJIT_DIAG("shared taskpool init FAILED: owner reported Failed (err=%u)",
                state_->lastInitError.loadAcquire());
      return InitResult::OwnerFailed;
    case EJitSharedInitState::Stopping:
      EJIT_DIAG("shared taskpool init FAILED: owner is Stopping");
      return InitResult::OwnerFailed;
    }
  }
#ifdef EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE
  // Two causes funnel here under a fixed worker core: the designated core never
  // activated (pool still Uninitialized), or it won the CAS but never reached
  // Ready within the budget (engine build too slow). Both are "the designated
  // owner did not come up" from this peer's point of view.
  EJIT_DIAG("shared taskpool init FAILED: designated worker core=%u did not "
            "reach Ready after spins (state=%u)",
            kEJitFixedWorkerCore, state_->initState.loadAcquire());
#else
  EJIT_DIAG("shared taskpool init FAILED: peer still initializing after spins");
#endif
  return InitResult::InitInProgress; // peer still initializing; pending, no
                                     // hang.
}

void EJitSharedTaskPool::ownerShutdown() {
  // Disarm this core's L0: its entries hold code pointers about to become
  // invalid. Peers stay armed but cannot match after the epoch bump below.
  gEJitL0State = nullptr;
  if (state_) {
    state_->dispatchEpoch.fetchAdd(1);
    // Empty every core's inline cache: its cells point at the generation being
    // torn down and carry no epoch to invalidate against. The cells are shared,
    // so zeroing them here retires them everywhere.
    icacheDrainAll("owner-shutdown");
  }
  if (!state_ || !isOwner_)
    return;
  EJIT_DIAG("shared taskpool owner shutdown begin");
  // Signal the worker loop to exit, then join it BEFORE returning state to
  // Uninitialized so no worker can touch owner-private state afterwards.
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Stopping));
  if (workerStop_)
    workerStop_(workerCtx_); // soft-stop + JOIN (no use-after-free).
  // A failed enable_ex may leave linked, non-executable objects queued after
  // the worker's final flush attempt. Drop their owner-private metadata before
  // destroying the engine; shared Pending slots were already drained above.
  if (!pendingPublishes_.empty())
    EJIT_DIAG("near-hot flush reason=shutdown dropped=%zu",
              pendingPublishes_.size());
  for (PendingPublish &P : pendingPublishes_) {
#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
    // A shutdown drops linked-but-unpublished near code.  The pending cache
    // slot carries no function pointer in this mode, but clear it defensively
    // and release the in-flight claims/profile admission before the generation
    // changes below.
    cacheDropPending(P.req, nullptr);
    dedupClear(P.req.funcIndex, P.req.generation);
    const uint32_t Tier = decodeReqTier(P.req.funcIndex);
    if (P.req.generation == state_->generation.loadAcquire() &&
        (Tier == kEJitTierInstrumented || Tier == kEJitTierPgoUse))
      finishPgoFunction(stripReqTier(P.req.funcIndex), /*completed=*/false,
                        "owner-shutdown");
    if (publishFn_)
      publishFn_(publishCtx_, P.req, false);
#endif
    if (releaseFn_)
      releaseFn_(releaseCtx_, P.fn);
  }
#ifndef EJIT_CODE_POOL_FIXED_NEAR_HOT
  pendingBatchCompiles_.clear();
#endif
  pendingPublishes_.clear();
  autoTier2PublishPending_ = false;
  autoTier2PublishBlocked_ = false;
  // Release what the election built, between the join and Uninitialized: no
  // compile can be in flight, and no peer can be elected yet. Without this the
  // former owner keeps its engine while a new owner builds another, so the
  // system accumulates one per handoff.
  if (ownerReleased_)
    ownerReleased_(ownerReleasedCtx_);
  state_->ownerCoreId.storeRelease(kEJitInvalidCoreId);
  state_->workerTaskId.storeRelease(0);
  state_->generation.storeRelease(state_->generation.loadRelaxed() + 1);
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Uninitialized));
  isOwner_ = false;
  EJIT_DIAG("shared taskpool owner shutdown complete");
}

//===----------------------------------------------------------------------===//
// Producer path (§5.2).
//===----------------------------------------------------------------------===//
__attribute__((always_inline)) EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::classifyHit(const SharedLookup &Hit, bool enqueueTier2) {
  CompileOrGetResult R;
  if (enqueueTier2 && Hit.tier2Arm && Hit.slot)
    enqueueTier2FromSlot(*Hit.slot);
  if (Hit.hasReadToken && Hit.fnPtr) {
    if (Hit.slot)
      markPostPublishSeen(*Hit.slot);
    EJIT_STAT_INC(state_->counters.cacheHits);
    R.status = EJitCompileOrGetStatus::CacheHit;
    R.fnPtr = Hit.fnPtr;
    R.bucketIndex = Hit.bucketIndex;
    R.hasReadToken = true;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // Seqlock hit: same terminal CacheHit, but no read token was taken and the
  // bucketIndex is the out-of-range sentinel, so the wrapper's releaseRead()
  // cleanly no-ops. Safe because published code is never freed in this build.
  if (Hit.noTokenHit && Hit.fnPtr) {
    if (Hit.slot)
      markPostPublishSeen(*Hit.slot);
    EJIT_STAT_INC(state_->counters.cacheHits);
    R.status = EJitCompileOrGetStatus::CacheHit;
    R.fnPtr = Hit.fnPtr;
    R.bucketIndex = Hit.bucketIndex; // == kEJitSharedCacheBuckets (sentinel)
    R.hasReadToken = false;
    R.fastPathTerminal = true;
    return R;
  }
#endif
  if (Hit.readyButNotShareable) {
    // The work is already done but this core may not read the cross-core
    // pointer; fall back cleanly WITHOUT re-enqueuing (avoids recompile churn).
    R.status = EJitCompileOrGetStatus::OffMode;
    R.readyButNotShareable = true;
    R.fastPathTerminal = true;
    return R;
  }
  if (Hit.pgoSamplingComplete) {
    // Tier-1 has enough data. Do not enter compileOrGet() again: Tier-2 already
    // owns the per-function dedup claim, and the wrapper should execute its AOT
    // fallback until the final code is published.
    R.status = EJitCompileOrGetStatus::AlreadyPending;
    R.fastPathTerminal = true;
    return R;
  }
  // True miss (Ready, enabled, no shareable cached code): the caller must fall
  // through to the compileOrGet() slow path (Off / Sync / Async).
  R.fastPathTerminal = false;
  return R;
}

EJitSharedTaskPool::CompileOrGetResult EJitSharedTaskPool::tryCacheHit(
    uint32_t funcIndex, const EJitDimPair *dims, uint32_t numDims,
    const EJitBoundPtrDescriptor *boundPointers, uint32_t boundCount) {
  CompileOrGetResult R;
  // Parameter check already done by the C API layer.

  // Ready check (§5.2 step 0): a not-yet-Ready pool is a clean fallback and
  // never reads the queue/cache.
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    EJIT_DIAG_VERBOSE("shared taskpool fallback func=%u: not Ready", funcIndex);
    R.status = EJitCompileOrGetStatus::OffMode; // not Ready → clean fallback.
    R.fastPathTerminal = true;
    return R;
  }

  // Instance-enabled check (§5.2 step 0): a disabled dim falls back and never
  // reaches the cache, so a deactivated instance is never served stale code.
  for (uint32_t i = 0; i < numDims; ++i)
    if (!isInstanceEnabled(dims[i].dimType, dims[i].instanceId)) {
      EJIT_STAT_INC_INSTANCE_DISABLED(state_);
      EJIT_DIAG_VERBOSE("shared taskpool disabled func=%u dim[%u]=(%u,%u)",
                        funcIndex, i, dims[i].dimType, dims[i].instanceId);
      R.status = EJitCompileOrGetStatus::InstanceDisabled;
      R.fastPathTerminal = true;
      return R;
    }

  // Cache lookup (§5.2 step 1) — runs BEFORE the Off check, so an already
  // compiled entry is still served even when the pool is globally Off.
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  SharedLookup Hit = cacheLookupSeq(funcIndex, dims, numDims);
#else
  SharedLookup Hit = cacheLookup(funcIndex, dims, numDims);
#endif
  if (Hit.tier2Arm && Hit.slot) {
    if (boundCount)
      enqueueTier2ForIdentity(funcIndex, dims, numDims, boundPointers,
                              boundCount);
    else
      enqueueTier2FromSlot(*Hit.slot);
    Hit.tier2Arm = false;
  }
  return classifyHit(Hit, /*enqueueTier2=*/false);
}

//===----------------------------------------------------------------------===//
// Fixed-dimension fast cache-hit entries (§5.2 steps 0-1, unrolled). Each
// shares the Ready gate + classifyHit() with tryCacheHit() but the
// instance-enabled check is unrolled and the dim identity is built directly on
// the stack, so the C ABI fixed-dimension entries reach the shared
// cacheLookup() without a numDims loop or variable-length array setup.
// Semantics are identical to tryCacheHit() with the matching numDims.
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit0D(uint32_t funcIndex) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  return classifyHit(cacheLookupSeq0D(funcIndex));
#else
  return classifyHit(cacheLookup0D(funcIndex));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit1D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
  if (!isInstanceEnabled(dim0, inst0)) {
    EJIT_STAT_INC_INSTANCE_DISABLED(state_);
    R.status = EJitCompileOrGetStatus::InstanceDisabled;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  return classifyHit(cacheLookupSeq1D(funcIndex, dim0, inst0));
#else
  return classifyHit(cacheLookup1D(funcIndex, dim0, inst0));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit2D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1,
                                  uint32_t inst1) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
  if (!isInstanceEnabled(dim0, inst0) || !isInstanceEnabled(dim1, inst1)) {
    EJIT_STAT_INC_INSTANCE_DISABLED(state_);
    R.status = EJitCompileOrGetStatus::InstanceDisabled;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  return classifyHit(cacheLookupSeq2D(funcIndex, dim0, inst0, dim1, inst1));
#else
  return classifyHit(cacheLookup2D(funcIndex, dim0, inst0, dim1, inst1));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit3D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1, uint32_t inst1,
                                  uint32_t dim2, uint32_t inst2) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
  if (!isInstanceEnabled(dim0, inst0) || !isInstanceEnabled(dim1, inst1) ||
      !isInstanceEnabled(dim2, inst2)) {
    EJIT_STAT_INC_INSTANCE_DISABLED(state_);
    R.status = EJitCompileOrGetStatus::InstanceDisabled;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  const EJitDimPair d3[3] = {{dim0, inst0}, {dim1, inst1}, {dim2, inst2}};
  return classifyHit(cacheLookupSeq(funcIndex, d3, 3));
#else
  return classifyHit(
      cacheLookup3D(funcIndex, dim0, inst0, dim1, inst1, dim2, inst2));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit4D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1, uint32_t inst1,
                                  uint32_t dim2, uint32_t inst2, uint32_t dim3,
                                  uint32_t inst3) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
  if (!isInstanceEnabled(dim0, inst0) || !isInstanceEnabled(dim1, inst1) ||
      !isInstanceEnabled(dim2, inst2) || !isInstanceEnabled(dim3, inst3)) {
    EJIT_STAT_INC_INSTANCE_DISABLED(state_);
    R.status = EJitCompileOrGetStatus::InstanceDisabled;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  const EJitDimPair d4[4] = {
      {dim0, inst0}, {dim1, inst1}, {dim2, inst2}, {dim3, inst3}};
  return classifyHit(cacheLookupSeq(funcIndex, d4, 4));
#else
  return classifyHit(cacheLookup4D(funcIndex, dim0, inst0, dim1, inst1, dim2,
                                   inst2, dim3, inst3));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::compileOrGet(uint32_t funcIndex, const EJitDimPair *dims,
                                 uint32_t numDims, void *fallback,
                                 const EJitBoundPtrDescriptor *boundPointers,
                                 uint32_t boundCount) {
  EJIT_DIAG_VERBOSE("shared taskpool request func=%u dims=%u fallback=%p",
                    funcIndex, numDims, fallback);
  // Parameter check already done by the C API layer.

  // Fast cache-hit path (§5.2 steps 0-1). On any terminal outcome (hit,
  // disabled instance, not-Ready, or ready-but-not-shareable) return directly.
  CompileOrGetResult R;
  if (!validateBoundPtrDescriptors(boundPointers, boundCount)) {
    EJIT_DIAG("shared taskpool bound pointer reject func=%u count=%u",
              funcIndex, boundCount);
    R.status = EJitCompileOrGetStatus::InvalidParam;
    R.fnPtr = fallback;
    return R;
  }
  R = tryCacheHit(funcIndex, dims, numDims, boundPointers, boundCount);
  if (R.fastPathTerminal) {
    // Non-hit terminals surface the caller's fallback pointer (a hit already
    // carries the cached fnPtr + read token).
    if (R.status != EJitCompileOrGetStatus::CacheHit)
      R.fnPtr = fallback;
    return R;
  }
  // True miss: continue the slow path with the caller's fallback.
  R.fnPtr = fallback;
  // Batched baseline compilation releases the coarse per-function in-flight
  // claim after installing an exact-identity Pending marker. Coalesce only an
  // identical request here so another cell/TRP version of the same function
  // can still enter the batch.
  if (cacheHasPending(funcIndex, dims, numDims)) {
    EJIT_STAT_INC(state_->counters.alreadyPending);
    R.status = EJitCompileOrGetStatus::AlreadyPending;
    return R;
  }
  // Off mode (§5.2 step 2).
  if (state_->mode.loadAcquire() ==
      static_cast<uint32_t>(EJitCompileMode::Off)) {
    EJIT_DIAG_VERBOSE("shared taskpool fallback func=%u: mode off", funcIndex);
    R.status = EJitCompileOrGetStatus::OffMode;
    return R;
  }
  // Sync mode: compile inline on the calling thread (no queue, no worker). Only
  // the owner core has the compile callback; non-owner cores fall back cleanly.
  if (state_->mode.loadAcquire() ==
      static_cast<uint32_t>(EJitCompileMode::Sync)) {
    if (!isOwner_ || !compileFn_) {
      EJIT_DIAG_VERBOSE(
          "shared taskpool sync fallback func=%u: not owner (owner=%u fn=%p)",
          funcIndex, static_cast<unsigned>(isOwner_),
          reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(compileFn_)));
      R.status = EJitCompileOrGetStatus::OffMode;
      return R;
    }
    // Build request with current instance versions, then compile inline.
    EJitCompileRequest ReqLocal{};
    ReqLocal.funcIndex = funcIndex;
    ReqLocal.numDims = numDims;
    for (uint32_t i = 0; i < numDims; ++i) {
      ReqLocal.dims[i] = dims[i];
      ReqLocal.versions[i] =
          instanceVersion(dims[i].dimType, dims[i].instanceId);
    }
    ReqLocal.boundCount = boundCount;
    for (uint32_t i = 0; i < boundCount; ++i)
      ReqLocal.boundPointers[i] = boundPointers[i];
    if (!versionsCurrent(ReqLocal)) {
      EJIT_DIAG(
          "shared taskpool sync bound request drop func=%u: version changed",
          funcIndex);
      R.status = EJitCompileOrGetStatus::CompileFailed;
      return R;
    }
    void *fn = nullptr;
    bool ok = compileFn_(compileCtx_, ReqLocal, &fn);
    if (!ok || !fn) {
      EJIT_STAT_INC(state_->counters.compileFailed);
      EJIT_DIAG("shared taskpool sync compile failed func=%u ok=%u", funcIndex,
                static_cast<unsigned>(ok));
      R.status = EJitCompileOrGetStatus::CompileFailed;
      return R;
    }
    // Sync callers need a callable result immediately. A batched allocator may
    // have linked this address while leaving its page RW/NX, so force the
    // current batch executable instead of publishing an unsafe pointer.
    if (codeReadyFn_ && !codeReadyFn_(codeBatchCtx_, fn) &&
        (!codeBatchFlushFn_ || !codeBatchFlushFn_(codeBatchCtx_) ||
         !codeReadyFn_(codeBatchCtx_, fn))) {
      if (releaseFn_)
        releaseFn_(releaseCtx_, fn);
      EJIT_STAT_INC(state_->counters.compileFailed);
      EJIT_DIAG("shared taskpool sync compile failed func=%u: batch enable",
                funcIndex);
      R.status = EJitCompileOrGetStatus::CompileFailed;
      return R;
    }
    if (!versionsCurrent(ReqLocal)) {
      if (releaseFn_)
        releaseFn_(releaseCtx_, fn);
      EJIT_STAT_INC(state_->counters.compileFailed);
      EJIT_DIAG("shared taskpool sync compile drop func=%u: version changed",
                funcIndex);
      R.status = EJitCompileOrGetStatus::CompileFailed;
      return R;
    }
    EJitCompiledCodeInfo info;
    if (codeRangeFn_)
      codeRangeFn_(codeRangeCtx_, fn, &info);
    EJitPublishStatus PS =
        cachePublish(ReqLocal, fn, info.codeSize ? &info : nullptr);
    if (PS == EJitPublishStatus::Published) {
      EJIT_STAT_INC(state_->counters.asyncCompiles);
      publishCodePoolStats();
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
      SharedLookup Hit2 = cacheLookupSeq(funcIndex, dims, numDims);
      if ((Hit2.hasReadToken || Hit2.noTokenHit) && Hit2.fnPtr) {
        R.status = EJitCompileOrGetStatus::CacheHit;
        R.fnPtr = Hit2.fnPtr;
        R.bucketIndex = Hit2.bucketIndex; // sentinel -> releaseRead no-op
        R.hasReadToken = Hit2.hasReadToken;
        EJIT_DIAG_VERBOSE("shared taskpool sync compiled func=%u fn=%p",
                          funcIndex, Hit2.fnPtr);
        return R;
      }
#else
      SharedLookup Hit2 = cacheLookup(funcIndex, dims, numDims);
      if (Hit2.hasReadToken && Hit2.fnPtr) {
        R.status = EJitCompileOrGetStatus::CacheHit;
        R.fnPtr = Hit2.fnPtr;
        R.bucketIndex = Hit2.bucketIndex;
        R.hasReadToken = true;
        EJIT_DIAG_VERBOSE("shared taskpool sync compiled func=%u fn=%p",
                          funcIndex, Hit2.fnPtr);
        return R;
      }
#endif
    } else {
      if (releaseFn_)
        releaseFn_(releaseCtx_, fn);
    }
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared taskpool sync compile failed func=%u publish=%u",
              funcIndex, static_cast<unsigned>(PS));
    R.status = EJitCompileOrGetStatus::CompileFailed;
    return R;
  }
  // Dedup + enqueue (§5.2 step 3) — Async path. Claim the per-function
  // in-flight slot BEFORE staged-PGO admission. Otherwise a request already in
  // the queue makes us log "profile start 0/N", only for dedupMark below to
  // reject it and immediately log "profile aborted" even though no profiling
  // ever started.
  const bool pgoForRequest = state_->pgoEnabled.loadAcquire() != 0;
  const uint32_t gen = state_->generation.loadAcquire();
  switch (dedupMark(funcIndex, gen)) {
  case EJitDedupResult::AlreadyPending:
    EJIT_STAT_INC(state_->counters.alreadyPending);
    EJIT_DIAG_VERBOSE("shared taskpool coalesced func=%u: already pending",
                      funcIndex);
    R.status = EJitCompileOrGetStatus::AlreadyPending;
    return R;
  case EJitDedupResult::InvalidFuncIndex:
    EJIT_DIAG("shared taskpool reject func=%u: out of range", funcIndex);
    R.status = EJitCompileOrGetStatus::InvalidParam;
    return R;
  case EJitDedupResult::Claimed:
    break;
  }

  bool newlyAdmitted = false;
  if (pgoForRequest && !admitPgoFunction(funcIndex, newlyAdmitted)) {
    // All profiling slots are occupied. Keep this miss on the AOT fallback
    // and do not add work to the compiler queue.
    dedupClear(funcIndex, gen);
    R.status = EJitCompileOrGetStatus::PgoAdmissionDeferred;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_TESTING
  if (pgoForRequest && pgoAdmissionTestHook_)
    pgoAdmissionTestHook_(pgoAdmissionTestHookCtx_);
#endif
  EJitCompileRequest Req{};
  Req.funcIndex = pgoForRequest
                      ? encodeReqTier(funcIndex, kEJitTierInstrumented)
                      : funcIndex;
  Req.numDims = numDims;
  Req.fallbackPtr = reinterpret_cast<uintptr_t>(fallback);
  Req.generation = gen;
  for (uint32_t i = 0; i < numDims; ++i) {
    Req.dims[i] = dims[i];
    Req.versions[i] = instanceVersion(dims[i].dimType, dims[i].instanceId);
  }
  Req.boundCount = boundCount;
  for (uint32_t i = 0; i < boundCount; ++i)
    Req.boundPointers[i] = boundPointers[i];
  if (!versionsCurrent(Req) || state_->generation.loadAcquire() != gen) {
    dedupClear(funcIndex, gen);
    if (newlyAdmitted)
      finishPgoFunction(funcIndex, /*completed=*/false,
                        "lifecycle-changed-during-admission");
    EJIT_DIAG(
        "shared taskpool async bound request drop func=%u: lifecycle changed",
        funcIndex);
    R.status = EJitCompileOrGetStatus::CompileFailed;
    return R;
  }
  if (!queuePush(Req)) {
    dedupClear(funcIndex, gen); // queue full → roll back the in-flight slot.
    if (newlyAdmitted)
      finishPgoFunction(funcIndex, /*completed=*/false, "tier1-queue-full");
    EJIT_STAT_INC(state_->counters.queueFull);
    EJIT_DIAG("shared taskpool fallback func=%u: queue full", funcIndex);
    R.status = EJitCompileOrGetStatus::QueueFullFallback;
    return R;
  }
  EJIT_STAT_INC(state_->counters.asyncEnqueues);
  EJIT_DIAG_VERBOSE("shared taskpool enqueued func=%u gen=%u", funcIndex, gen);
  R.status = EJitCompileOrGetStatus::EnqueuedPending;
  return R;
}

//===----------------------------------------------------------------------===//
// Consumer path (§5.3) — runs on the single owner worker (or a test driver).
//===----------------------------------------------------------------------===//
void EJitSharedTaskPool::publishCodePoolStats() {
  if (!state_ || !codePoolStatsFn_)
    return;
  EJitCodePoolStatsOut s{};
  if (!codePoolStatsFn_(codePoolStatsCtx_, &s))
    return;
  state_->codePoolStats.poolCount.storeRelaxed(s.poolCount);
  state_->codePoolStats.sealedCount.storeRelaxed(s.sealedCount);
  state_->codePoolStats.activeCount.storeRelaxed(s.activeCount);
  state_->codePoolStats.usedBytes.storeRelaxed(s.usedBytes);
  state_->codePoolStats.reservedBytes.storeRelaxed(s.reservedBytes);
  state_->codePoolStats.wastedBytes.storeRelaxed(s.wastedBytes);
  state_->codePoolStats.sealInvocations.storeRelaxed(s.sealInvocations);
  state_->codePoolStats.splitInvocations.storeRelaxed(s.splitInvocations);
  state_->codePoolStats.finalizedRangeCount.storeRelaxed(s.finalizedRangeCount);
  auto PublishDetail = [](EJitSharedCodePoolStats::Detail &Dst,
                          const EJitCodePoolStatsOut::Detail &Src) {
    Dst.poolCount.storeRelaxed(Src.poolCount);
    Dst.sealedCount.storeRelaxed(Src.sealedCount);
    Dst.activeCount.storeRelaxed(Src.activeCount);
    Dst.usedBytes.storeRelaxed(Src.usedBytes);
    Dst.reservedBytes.storeRelaxed(Src.reservedBytes);
    Dst.wastedBytes.storeRelaxed(Src.wastedBytes);
    Dst.sealInvocations.storeRelaxed(Src.sealInvocations);
    Dst.splitInvocations.storeRelaxed(Src.splitInvocations);
    Dst.finalizedRangeCount.storeRelaxed(Src.finalizedRangeCount);
    Dst.baseAddress.storeRelaxed(Src.baseAddress);
    Dst.endAddress.storeRelaxed(Src.endAddress);
    Dst.pendingBytes.storeRelaxed(Src.pendingBytes);
    Dst.pendingRangeCount.storeRelaxed(Src.pendingRangeCount);
    Dst.fallbackCount.storeRelaxed(Src.fallbackCount);
    Dst.full.storeRelaxed(Src.full);
  };
  PublishDetail(state_->codePoolStats.near, s.near);
  for (uint32_t I = 0; I < kEJitNearHotPoolCount; ++I)
    PublishDetail(state_->codePoolStats.nearHot[I], s.nearHot[I]);
  PublishDetail(state_->codePoolStats.far, s.far);
}

#ifndef EJIT_CODE_POOL_FIXED_NEAR_HOT
void EJitSharedTaskPool::compilePendingBatchRequests() {
  if (pendingBatchCompiles_.empty())
    return;

  std::stable_sort(
      pendingBatchCompiles_.begin(), pendingBatchCompiles_.end(),
      [](const PendingBatchCompile &A, const PendingBatchCompile &B) {
        // Layout specializations lexicographically by dimensions. In the
        // product mapping this is cell first and TRP second; the same rule
        // remains deterministic for 0D, 1D, and future wider identities.
        for (uint32_t I = 0; I < kEJitSharedMaxDims; ++I) {
          const bool AHas = I < A.req.numDims;
          const bool BHas = I < B.req.numDims;
          if (AHas != BHas)
            return !AHas;
          if (!AHas)
            break;
          if (A.req.dims[I].dimType != B.req.dims[I].dimType)
            return A.req.dims[I].dimType < B.req.dims[I].dimType;
          if (A.req.dims[I].instanceId != B.req.dims[I].instanceId)
            return A.req.dims[I].instanceId < B.req.dims[I].instanceId;
        }
        const uint32_t AFunc = stripReqTier(A.req.funcIndex);
        const uint32_t BFunc = stripReqTier(B.req.funcIndex);
        if (AFunc != BFunc)
          return AFunc < BFunc;
        return decodeReqTier(A.req.funcIndex) < decodeReqTier(B.req.funcIndex);
      });

  [[maybe_unused]] size_t Dim0Groups = 0;
  bool HavePrevious = false;
  bool PreviousHasDim0 = false;
  EJitDimPair Previous{};
  for (const PendingBatchCompile &P : pendingBatchCompiles_) {
    const bool HasDim0 = P.req.numDims != 0;
    const bool Same =
        HavePrevious && HasDim0 == PreviousHasDim0 &&
        (!HasDim0 || (Previous.dimType == P.req.dims[0].dimType &&
                      Previous.instanceId == P.req.dims[0].instanceId));
    if (!Same) {
      ++Dim0Groups;
      Previous = HasDim0 ? P.req.dims[0] : EJitDimPair{};
    }
    HavePrevious = true;
    PreviousHasDim0 = HasDim0;
  }

  std::vector<PendingBatchCompile> Batch;
  Batch.swap(pendingBatchCompiles_);
  EJIT_DIAG_DEBUG("shared worker batch layout: requests=%zu dim0Groups=%zu",
                  Batch.size(), Dim0Groups);
  for (const PendingBatchCompile &P : Batch) {
    runCompile(P.req, P.hasSharedMarker);
    // Sorted batch compilation happens inside one publish worker step. Yield
    // between expensive ORC compilations just as the normal queue loop does.
    workerThrottle();
  }
}
#endif

#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
bool EJitSharedTaskPool::codeBatchFlushPoolCallbacksIntact() const {
  return codeBatchFlushPoolFn_ == codeBatchFlushPoolExpectedFn_ &&
         codeBatchFlushPoolCtx_ == codeBatchFlushPoolExpectedCtx_ &&
         codeBatchFlushPoolCanary_ ==
             makeCodeBatchFlushCanary(codeBatchFlushPoolFn_,
                                      codeBatchFlushPoolCtx_) &&
         codeBatchFlushPoolLayoutTag_ ==
             makeCodeBatchFlushLayoutTag(this);
}
#endif

void EJitSharedTaskPool::diagnoseCodeBatchCallbacks(const char *Reason,
                                                    const void *OwnerCtx) const {
#if defined(EJIT_CODE_POOL_FIXED_NEAR_HOT) && defined(EJIT_DIAG_ENABLE)
  const bool CallbacksIntact = codeBatchFlushPoolCallbacksIntact();
  const bool CallbackValid =
      CallbacksIntact && codeBatchFlushPoolFn_ && codeBatchFlushPoolCtx_;
  const uint64_t LayoutTag = makeCodeBatchFlushLayoutTag(this);
  const bool LayoutMatches = codeBatchFlushPoolLayoutTag_ == LayoutTag;
  const char *Validation = "matched-target-unverified";
  if (!codeBatchFlushPoolFn_)
    Validation = "callback-target-null";
  else if (!codeBatchFlushPoolCtx_)
    Validation = "callback-context-null";
  else if (!LayoutMatches)
    Validation = "layout-mismatch";
  else if (!CallbacksIntact)
    Validation = "callback-mismatch";
  const unsigned FeatureMask = codeBatchFlushPoolFeatureMask();
  const uintptr_t Base = reinterpret_cast<uintptr_t>(this);
  const size_t PoolFlushFnOffset =
      reinterpret_cast<uintptr_t>(&codeBatchFlushPoolFn_) - Base;
  const size_t PoolFlushCtxOffset =
      reinterpret_cast<uintptr_t>(&codeBatchFlushPoolCtx_) - Base;
  const size_t SplitFnOffset =
      reinterpret_cast<uintptr_t>(&splitPoolFn_) - Base;
  const size_t SplitCtxOffset =
      reinterpret_cast<uintptr_t>(&splitPoolCtx_) - Base;
  const size_t ExpectedFnOffset =
      reinterpret_cast<uintptr_t>(&codeBatchFlushPoolExpectedFn_) - Base;
  const size_t ExpectedCtxOffset =
      reinterpret_cast<uintptr_t>(&codeBatchFlushPoolExpectedCtx_) - Base;
  const size_t CanaryOffset =
      reinterpret_cast<uintptr_t>(&codeBatchFlushPoolCanary_) - Base;
  constexpr unsigned Has4KSeal =
#ifdef EJIT_CODE_POOL_4K_SEAL
      1u;
#else
      0u;
#endif
  constexpr unsigned HasBatchPublish =
#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
      1u;
#else
      0u;
#endif
  constexpr unsigned HasFixedPool =
#ifdef EJIT_FIXED_CODE_POOL
      1u;
#else
      0u;
#endif
  // SRE diagnostic transports commonly cap one formatted record at 512 bytes.
  // Keep each record deliberately short: an oversized callback diagnostic is
  // least useful precisely when this path is diagnosing a target-side fault.
  EJIT_DIAG("near-hot cb stage=%s self=%p owner=%p fn=%p ctx=%p",
      Reason ? Reason : "unspecified",
      static_cast<const void *>(this), OwnerCtx,
      reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(
          codeBatchFlushPoolFn_)),
      codeBatchFlushPoolCtx_);
  EJIT_DIAG("near-hot cb guard stage=%s expectedFn=%p expectedCtx=%p "
            "intact=%u valid=%u layout=%u status=%s",
      Reason ? Reason : "unspecified",
      reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(
          codeBatchFlushPoolExpectedFn_)),
      codeBatchFlushPoolExpectedCtx_,
      CallbacksIntact ? 1u : 0u, CallbackValid ? 1u : 0u,
      LayoutMatches ? 1u : 0u, Validation);
  EJIT_DIAG("near-hot cb abi size=%zu fnOff=%zu ctxOff=%zu expFnOff=%zu "
            "expCtxOff=%zu canaryOff=%zu",
      sizeof(EJitSharedTaskPool), PoolFlushFnOffset, PoolFlushCtxOffset,
      ExpectedFnOffset, ExpectedCtxOffset, CanaryOffset);
  EJIT_DIAG("near-hot cb tags canary=0x%llx saved=0x%llx now=0x%llx "
            "features=0x%x splitOff=%zu/%zu flags=%u/%u/%u",
      static_cast<unsigned long long>(codeBatchFlushPoolCanary_),
      static_cast<unsigned long long>(codeBatchFlushPoolLayoutTag_),
      static_cast<unsigned long long>(LayoutTag),
      FeatureMask, SplitFnOffset, SplitCtxOffset, Has4KSeal,
      HasBatchPublish, HasFixedPool);
#else
  (void)Reason;
  (void)OwnerCtx;
#endif
}

bool EJitSharedTaskPool::flushPendingPublishes(bool compileBatchRequests,
                                               const char *Reason) {
#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
  (void)compileBatchRequests;
  // The fixed near-hot layout is a per-pool commit protocol. Falling back to
  // the legacy all-pools callback here would make a failed pool seal unrelated
  // pools and would invalidate the partial-commit guarantee.
  // The owner callback currently dereferences its context to reach the private
  // ORC engine. Fail closed if either half of the pair is missing: a malformed
  // registration must leave the request pending/AOT, never make a null-context
  // indirect call from the worker.
  if (!nearHotFirstFlushDiagnosed_) {
    nearHotFirstFlushDiagnosed_ = true;
    // The callback context is intentionally not trusted as a driver address.
    // The explicit owner-side diagnostics above are the authoritative driver
    // identity; this line is about the pre-call callback state only.
    diagnoseCodeBatchCallbacks("first-pre-flush", nullptr);
  }
  const auto FlushFn = codeBatchFlushPoolFn_;
  void *const FlushCtx = codeBatchFlushPoolCtx_;
  const bool CallbacksIntact = codeBatchFlushPoolCallbacksIntact();
  const bool LayoutMatches =
      codeBatchFlushPoolLayoutTag_ == makeCodeBatchFlushLayoutTag(this);
  const char *Validation = "matched-target-unverified";
  if (!FlushFn)
    Validation = "callback-target-null";
  else if (!FlushCtx)
    Validation = "callback-context-null";
  else if (!LayoutMatches)
    Validation = "layout-mismatch";
  else if (!CallbacksIntact)
    Validation = "callback-mismatch";
  if (!CallbacksIntact || !FlushFn || !FlushCtx) {
    if (!pendingPublishes_.empty())
      EJIT_DIAG("near-hot pool flush skipped reason=pre-flush "
                "sharedPoolThis=%p flushCtx=%p expectedOwnerCtx=%p "
                "fn=%p ctx=%p expectedFn=%p expectedCtx=%p canary=0x%llx "
                "layoutTag=0x%llx intact=%u validation=%s pending=%zu",
                static_cast<const void *>(this), FlushCtx,
                codeBatchFlushPoolExpectedCtx_,
                reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(
                    FlushFn)),
                FlushCtx,
                reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(
                    codeBatchFlushPoolExpectedFn_)),
                codeBatchFlushPoolExpectedCtx_,
                static_cast<unsigned long long>(codeBatchFlushPoolCanary_),
                static_cast<unsigned long long>(
                    codeBatchFlushPoolLayoutTag_),
                CallbacksIntact ? 1u : 0u, Validation,
                pendingPublishes_.size());
    // Do not let an unavailable callback turn an idle worker into an implicit
    // retry loop. An explicit flush request clears this gate before retrying.
    autoTier2PublishBlocked_ = !pendingPublishes_.empty();
    return pendingPublishes_.empty();
  }

  [[maybe_unused]] size_t Published = 0;
  size_t Dropped = 0;
  std::vector<PendingPublish> Retry;
  // There are exactly 17 semantic near-hot pools. Do not use std::find on a
  // vector<uint32_t> here: libc++ may lower that specialization to wmemchr,
  // which is not a safe executable dependency in the freestanding SRE image.
  uint32_t FlushedPoolBitmap = 0;
  size_t FlushedPoolCount = 0;
  [[maybe_unused]] uint32_t FailedPoolBitmap = 0;
  for (const PendingPublish &P : pendingPublishes_) {
    bool Flushed = false;
    if (P.poolId >= kEJitNearHotPoolCount) {
      EJIT_DIAG("near-hot flush skip invalid pool=%u", P.poolId);
    } else {
      const uint32_t PoolBit = uint32_t{1} << P.poolId;
      if ((FlushedPoolBitmap & PoolBit) != 0)
        continue;
      FlushedPoolBitmap |= PoolBit;
      ++FlushedPoolCount;
      EJIT_DIAG("near-hot flush enter pool=%u fn=%p ctx=%p pending=%zu",
                P.poolId,
                reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(FlushFn)),
                FlushCtx, pendingPublishes_.size());
      Flushed = FlushFn(FlushCtx, P.poolId);
      if (!Flushed)
        FailedPoolBitmap |= PoolBit;
      EJIT_DIAG("near-hot flush return pool=%u ok=%u", P.poolId,
                static_cast<unsigned>(Flushed));
    }
    for (const PendingPublish &Member : pendingPublishes_) {
      if (Member.poolId != P.poolId)
        continue;
      const uint32_t Tier = decodeReqTier(Member.req.funcIndex);
      const uint32_t FuncIndex = stripReqTier(Member.req.funcIndex);
      EJitPublishStatus PS = EJitPublishStatus::Failed;
      EJitCompiledCodeInfo Info{};
      const bool Ready =
          Flushed && codeReadyFn_ && codeReadyFn_(codeBatchCtx_, Member.fn);
      const bool HasRange = Ready && codeRangeFn_ &&
                            codeRangeFn_(codeRangeCtx_, Member.fn, &Info);
      if (HasRange)
        PS =
            cachePublish(Member.req, Member.fn, &Info, Tier == kEJitTierPgoUse);
      if (PS == EJitPublishStatus::Published) {
        ++Published;
        EJIT_STAT_INC(state_->counters.asyncCompiles);
        if (publishFn_)
          publishFn_(publishCtx_, Member.req, true);
        if (Tier == kEJitTierPgoUse) {
          EJIT_STAT_INC(state_->counters.tier2Compiles);
          finishPgoFunction(FuncIndex, /*completed=*/true);
        }
        dedupClear(Member.req.funcIndex, Member.req.generation);
        continue;
      }
      if (Tier == kEJitTierPgoUse && Flushed &&
          PS == EJitPublishStatus::Failed) {
        Retry.push_back(Member);
        EJIT_STAT_INC(state_->counters.publishFailed);
        continue;
      }
      ++Dropped;
      // A fixed near-hot compile is never inserted with its fnPtr before the
      // pool commit, so the pending cache slot (if any) still carries null.
      // Passing Member.fn would leave that slot Pending forever.
      cacheDropPending(Member.req, nullptr);
      if (publishFn_)
        publishFn_(publishCtx_, Member.req, false);
      if (Member.req.generation == state_->generation.loadAcquire() &&
          Tier == kEJitTierPgoUse)
        finishPgoFunction(FuncIndex, /*completed=*/false);
      if (releaseFn_)
        releaseFn_(releaseCtx_, Member.fn);
      EJIT_STAT_INC(state_->counters.publishFailed);
      dedupClear(Member.req.funcIndex, Member.req.generation);
    }
  }
  pendingPublishes_.swap(Retry);
  autoTier2PublishPending_ = !pendingPublishes_.empty();
  // Seal or cache publication failures are explicitly retryable, but must not
  // turn an idle worker into a retry loop. flushCodeBatch() and the shared
  // explicit request clear this gate before retrying.
  autoTier2PublishBlocked_ = !pendingPublishes_.empty();
  publishCodePoolStats();
  EJIT_DIAG("near-hot flush reason=%s pools=%zu published=%zu dropped=%zu "
            "retry=%zu failedPoolBitmap=0x%08x",
            Reason ? Reason : "unspecified", FlushedPoolCount, Published,
            Dropped, pendingPublishes_.size(), FailedPoolBitmap);
  return Dropped == 0 && pendingPublishes_.empty();
#else
  (void)Reason;
  if (compileBatchRequests)
    compilePendingBatchRequests();
  if (!codeBatchFlushFn_)
    return pendingPublishes_.empty() && pendingBatchCompiles_.empty();
  if (!codeBatchFlushFn_(codeBatchCtx_)) {
    EJIT_DIAG("shared worker batch enable failed: pending=%zu",
              pendingPublishes_.size());
    return false;
  }

  [[maybe_unused]] size_t Published = 0;
  [[maybe_unused]] size_t Dropped = 0;
  std::vector<PendingPublish> Retry;
  for (PendingPublish &P : pendingPublishes_) {
    const uint32_t Tier = decodeReqTier(P.req.funcIndex);
    const uint32_t FuncIndex = stripReqTier(P.req.funcIndex);
    EJitCompiledCodeInfo Info{};
    bool Ready = !codeReadyFn_ || codeReadyFn_(codeBatchCtx_, P.fn);
    bool HasRange =
        Ready && codeRangeFn_ && codeRangeFn_(codeRangeCtx_, P.fn, &Info);
    EJitPublishStatus PS =
        Ready ? cachePublish(P.req, P.fn, HasRange ? &Info : nullptr,
                             Tier == kEJitTierPgoUse)
              : EJitPublishStatus::Failed;
    if (PS == EJitPublishStatus::Published) {
      ++Published;
      EJIT_STAT_INC(state_->counters.asyncCompiles);
      if (publishFn_)
        publishFn_(publishCtx_, P.req, true);
      if (Tier == kEJitTierInstrumented) {
        EJIT_STAT_INC(state_->counters.tier1Compiles);
        EJIT_DIAG("PGO Tier-1 batch published func=%u: collecting 0/%u hits",
                  FuncIndex, state_->tier2Threshold.loadAcquire());
      } else if (Tier == kEJitTierPgoUse) {
        EJIT_STAT_INC(state_->counters.tier2Compiles);
        finishPgoFunction(FuncIndex, /*completed=*/true);
      }
    } else if (Tier == kEJitTierPgoUse && PS == EJitPublishStatus::Failed) {
      // The linked Tier-2 code is already executable, but publishing the
      // replacement pointer failed transiently. Keep both the owner-private
      // result and the PGO admission; the live shared slot still points at
      // Tier-1 and a later explicit publish can retry without recompiling.
      Retry.push_back(P);
      EJIT_STAT_INC(state_->counters.publishFailed);
      EJIT_DIAG("PGO Tier-2 publish deferred func=%u; Tier-1 remains active",
                FuncIndex);
      continue;
    } else {
      ++Dropped;
      cacheDropPending(P.req, P.fn);
      if (publishFn_)
        publishFn_(publishCtx_, P.req, false);
      if (P.req.generation == state_->generation.loadAcquire() &&
          (Tier == kEJitTierInstrumented || Tier == kEJitTierPgoUse))
        finishPgoFunction(FuncIndex, /*completed=*/false);
      if (releaseFn_)
        releaseFn_(releaseCtx_, P.fn);
      if (PS == EJitPublishStatus::VersionMismatch)
        EJIT_STAT_INC(state_->counters.compileFailed);
      else
        EJIT_STAT_INC(state_->counters.publishFailed);
    }
    // Staged entries clear this at staging time; unstaged overflow/failure
    // entries retain it until here. Generation-aware CAS makes both safe.
    dedupClear(P.req.funcIndex, P.req.generation);
  }
  [[maybe_unused]] const size_t Total = pendingPublishes_.size();
  pendingPublishes_.swap(Retry);
  publishCodePoolStats();
  EJIT_DIAG_VERBOSE("shared worker batch publish: total=%zu published=%zu "
                    "dropped=%zu retry=%zu",
                    Total, Published, Dropped, pendingPublishes_.size());
  return Dropped == 0 && pendingPublishes_.empty();
#endif
}

bool EJitSharedTaskPool::serviceCodeBatchRequest() {
#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
  if (!state_)
    return false;
  uint32_t Expected =
      static_cast<uint32_t>(EJitCodeBatchRequestState::Requested);
  if (!state_->codeBatchRequestState.compareExchange(
          Expected, static_cast<uint32_t>(EJitCodeBatchRequestState::Running)))
    return false;
  // Include every request that was already queued when this explicit publish
  // began, but do not chase producers indefinitely. In fixed near-hot mode
  // pollOne() links each request directly into its semantic pool; the flush
  // only commits already-linked pool ranges. The legacy path retains its
  // owner-private dimension-sorted batch.
  uint32_t Queued =
      state_->enqueuePos.loadAcquire() - state_->dequeuePos.loadAcquire();
  Queued = std::min(Queued, kEJitSharedQueueSlots);
  while (Queued-- != 0 && pollOne()) {
    // Explicit publication may drain several queued requests inside one
    // worker step. Preserve the normal per-request scheduling gap.
    workerThrottle();
  }
  autoTier2PublishPending_ = false;
  autoTier2PublishBlocked_ = false;
  const bool Ok = flushPendingPublishes(/*compileBatchRequests=*/true,
                                        /*reason=*/"explicit");
  state_->codeBatchRequestState.storeRelease(
      static_cast<uint32_t>(Ok ? EJitCodeBatchRequestState::Succeeded
                               : EJitCodeBatchRequestState::Failed));
  return true;
#else
  return false;
#endif
}

bool EJitSharedTaskPool::serviceAutoTier2Publish() {
#ifndef EJIT_CODE_POOL_BATCHED_PUBLISH
  return false;
#else
  if (!autoTier2PublishPending_ || autoTier2PublishBlocked_ || !state_)
    return false;
  // pollOne() has just observed the queue empty. Re-check both positions so a
  // Tier-1 request that raced that observation is compiled from the far pool
  // before the near-pool Tier-2 batch is sealed and published.
  if (state_->enqueuePos.loadAcquire() != state_->dequeuePos.loadAcquire()) {
    autoTier2IdleTicks_ = 0;
    return false;
  }

#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
  // One empty observation can race a producer that is about to enqueue the
  // next specialization. Require two worker idle observations before sealing
  // any pool; explicit publish remains an immediate override.
  if (++autoTier2IdleTicks_ < 2u)
    return false;
#endif

  autoTier2PublishPending_ = false;
  autoTier2IdleTicks_ = 0;
  EJIT_DIAG("PGO Tier-2 queue drained: auto-publishing %zu linked version(s)",
            pendingPublishes_.size());
  if (!flushPendingPublishes(/*compileBatchRequests=*/false,
                             /*reason=*/"idle"))
    EJIT_DIAG("PGO Tier-2 auto-publish failed: pending=%zu; explicit publish "
              "may retry",
              pendingPublishes_.size());
  return true;
#endif
}

bool EJitSharedTaskPool::requestCodeBatchFlushAndWait() {
#ifndef EJIT_CODE_POOL_BATCHED_PUBLISH
  return false;
#else
  if (!state_ || state_->initState.loadAcquire() !=
                     static_cast<uint32_t>(EJitSharedInitState::Ready))
    return false;
  const uint32_t Generation = state_->generation.loadAcquire();
  constexpr uint32_t MaxWaits = 1u << 20;

  uint32_t Wait = 0;
  for (; Wait < MaxWaits; ++Wait) {
    uint32_t Unlocked = 0;
    if (state_->codeBatchRequestLock.compareExchange(Unlocked, 1))
      break;
    if (workerIdle_)
      workerIdle_(workerIdleCtx_, 1);
    else
      cpuRelax();
  }
  if (Wait == MaxWaits)
    return false;

  uint32_t RequestState = state_->codeBatchRequestState.loadAcquire();
  if (RequestState !=
          static_cast<uint32_t>(EJitCodeBatchRequestState::Requested) &&
      RequestState != static_cast<uint32_t>(EJitCodeBatchRequestState::Running))
    state_->codeBatchRequestState.storeRelease(
        static_cast<uint32_t>(EJitCodeBatchRequestState::Requested));

  bool Ok = false;
  for (Wait = 0; Wait < MaxWaits; ++Wait) {
    if (state_->generation.loadAcquire() != Generation ||
        state_->initState.loadAcquire() !=
            static_cast<uint32_t>(EJitSharedInitState::Ready))
      break;
    RequestState = state_->codeBatchRequestState.loadAcquire();
    if (RequestState ==
            static_cast<uint32_t>(EJitCodeBatchRequestState::Succeeded) ||
        RequestState ==
            static_cast<uint32_t>(EJitCodeBatchRequestState::Failed)) {
      Ok = RequestState ==
           static_cast<uint32_t>(EJitCodeBatchRequestState::Succeeded);
      state_->codeBatchRequestState.storeRelaxed(
          static_cast<uint32_t>(EJitCodeBatchRequestState::Idle));
      break;
    }
    if (workerIdle_)
      workerIdle_(workerIdleCtx_, 1);
    else
      cpuRelax();
  }
  state_->codeBatchRequestLock.storeRelease(0);
  return Ok;
#endif
}

bool EJitSharedTaskPool::readCodePoolStats(EJitCodePoolStatsOut *out) const {
  if (!state_ || !out)
    return false;
  out->poolCount = state_->codePoolStats.poolCount.loadRelaxed();
  out->sealedCount = state_->codePoolStats.sealedCount.loadRelaxed();
  out->activeCount = state_->codePoolStats.activeCount.loadRelaxed();
  out->usedBytes = state_->codePoolStats.usedBytes.loadRelaxed();
  out->reservedBytes = state_->codePoolStats.reservedBytes.loadRelaxed();
  out->wastedBytes = state_->codePoolStats.wastedBytes.loadRelaxed();
  out->sealInvocations = state_->codePoolStats.sealInvocations.loadRelaxed();
  out->splitInvocations = state_->codePoolStats.splitInvocations.loadRelaxed();
  out->finalizedRangeCount =
      state_->codePoolStats.finalizedRangeCount.loadRelaxed();
  auto ReadDetail = [](const EJitSharedCodePoolStats::Detail &Src,
                       EJitCodePoolStatsOut::Detail &Dst) {
    Dst.poolCount = Src.poolCount.loadRelaxed();
    Dst.sealedCount = Src.sealedCount.loadRelaxed();
    Dst.activeCount = Src.activeCount.loadRelaxed();
    Dst.usedBytes = Src.usedBytes.loadRelaxed();
    Dst.reservedBytes = Src.reservedBytes.loadRelaxed();
    Dst.wastedBytes = Src.wastedBytes.loadRelaxed();
    Dst.sealInvocations = Src.sealInvocations.loadRelaxed();
    Dst.splitInvocations = Src.splitInvocations.loadRelaxed();
    Dst.finalizedRangeCount = Src.finalizedRangeCount.loadRelaxed();
    Dst.baseAddress = Src.baseAddress.loadRelaxed();
    Dst.endAddress = Src.endAddress.loadRelaxed();
    Dst.pendingBytes = Src.pendingBytes.loadRelaxed();
    Dst.pendingRangeCount = Src.pendingRangeCount.loadRelaxed();
    Dst.fallbackCount = Src.fallbackCount.loadRelaxed();
    Dst.full = Src.full.loadRelaxed();
  };
  ReadDetail(state_->codePoolStats.near, out->near);
  for (uint32_t I = 0; I < kEJitNearHotPoolCount; ++I)
    ReadDetail(state_->codePoolStats.nearHot[I], out->nearHot[I]);
  ReadDetail(state_->codePoolStats.far, out->far);
  return true;
}

void EJitSharedTaskPool::runCompile(const EJitCompileRequest &req,
                                    bool hasBatchRequestMarker) {
  // PGO (§4.9): the aarch64 exclusive-monitor workaround is a property of the
  // Tier-2 publish, not a caller flag. A Tier-2 (PGOUse) request always follows
  // an arming hit whose hitCount.fetchAdd primed the monitor on this bucket, so
  // derive pgoClearExclusive from the request's encoded tier here in the
  // worker.
  const uint32_t tier = decodeReqTier(req.funcIndex);
  const uint32_t realFuncIndex = stripReqTier(req.funcIndex);
  const bool pgoClearExclusive = tier == kEJitTierPgoUse;
  auto dropBatchRequestMarker = [&] {
    if (hasBatchRequestMarker)
      cacheDropPending(req, nullptr);
  };
  auto finishPgoOnFailure = [&]() {
    if (tier == kEJitTierInstrumented) {
      finishPgoFunction(realFuncIndex, /*completed=*/false,
                        "tier1-compile-or-publish-failed");
    } else if (tier == kEJitTierPgoUse) {
      // Tier-1 is still published and instrumented. Retain admission so a
      // later hit retries Tier-2 instead of freeing this admission slot while
      // its instrumented Tier-1 code remains live.
      EJIT_DIAG("PGO Tier-2 failed func=%u; profile remains active for retry",
                realFuncIndex);
    }
  };
  EJIT_DIAG_VERBOSE("shared worker compile begin func=%u dims=%u gen=%u",
                    req.funcIndex, req.numDims, req.generation);
  // Checkpoint 0 (spec §11): generation guard. A request enqueued under an
  // earlier generation (owner re-init in between) is dropped before compiling.
  // dedupClear is generation-aware, so this never clears a NEW generation's
  // in-flight slot for the same funcIndex.
  if (req.generation != state_->generation.loadAcquire()) {
    dropBatchRequestMarker();
    dedupClear(req.funcIndex, req.generation);
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared worker compile drop func=%u: generation changed",
              req.funcIndex);
    return;
  }
  // Checkpoint 1: invalidated before compile started.
  if (!versionsCurrent(req)) {
    dropBatchRequestMarker();
    dedupClear(req.funcIndex, req.generation);
    if (tier == kEJitTierInstrumented || tier == kEJitTierPgoUse)
      finishPgoFunction(realFuncIndex, /*completed=*/false,
                        "version-changed-before-compile");
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG(
        "shared worker compile drop func=%u: version changed before compile",
        req.funcIndex);
    return;
  }
  void *fn = nullptr;
  bool ok = compileFn_ && compileFn_(compileCtx_, req, &fn);
  if (!ok || !fn) {
    dropBatchRequestMarker();
    dedupClear(req.funcIndex, req.generation);
    finishPgoOnFailure();
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared worker compile failed func=%u ok=%u fn=%p", req.funcIndex,
              static_cast<unsigned>(ok), fn);
    return;
  }
  const EJitCompileRequest PublishReq = requestForPublication(req);
  // Resolve the real executable range for the freshly compiled pointer (from
  // the owner's code-pool finalize metadata) so it can be published into the
  // cache slot for cross-core 4K sealing. Optional: if no provider is wired or
  // the pointer is not pool-backed, the slot carries no range and a 4K peer
  // cleanly falls back.
  EJitCompiledCodeInfo info{};
  if (codeRangeFn_)
    (void)codeRangeFn_(codeRangeCtx_, fn, &info);
  // Checkpoint 2: a generation bump OR a toggle during compilation invalidates
  // the result.
  if (req.generation != state_->generation.loadAcquire() ||
      !versionsCurrent(req)) {
    dropBatchRequestMarker();
    if (publishFn_)
      publishFn_(publishCtx_, PublishReq, false);
    dedupClear(req.funcIndex, req.generation);
    if (req.generation == state_->generation.loadAcquire() &&
        (tier == kEJitTierInstrumented || tier == kEJitTierPgoUse))
      finishPgoFunction(realFuncIndex, /*completed=*/false,
                        "version-changed-after-compile");
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG(
        "shared worker compile drop func=%u: version/gen changed after compile",
        req.funcIndex);
    return;
  }
#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
  if (tier == kEJitTierPgoUse && !nearHotFirstLinkedDiagnosed_) {
    nearHotFirstLinkedDiagnosed_ = true;
    diagnoseCodeBatchCallbacks("first-t2-linked", compileCtx_);
  }
#endif
#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
  // Near code must remain owner-private until its pool is sealed.  Without a
  // per-pool flush callback there is no safe way to make it executable or
  // publish it, so fail closed instead of leaving a permanent pending item.
  auto dropUnpublishedNearResult = [&](const char *Reason) {
    cacheDropPending(req, nullptr);
    if (publishFn_)
      publishFn_(publishCtx_, req, false);
    dedupClear(req.funcIndex, req.generation);
    finishPgoOnFailure();
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    EJIT_STAT_INC(state_->counters.publishFailed);
    EJIT_DIAG("near-hot result dropped func=%u reason=%s", realFuncIndex,
              Reason);
  };
  if (tier != kEJitTierInstrumented &&
      (!codeReadyFn_ || !codeRangeFn_ || !codeBatchFlushPoolFn_)) {
    dropUnpublishedNearResult("missing-near-publish-callback");
    return;
  }
  if (!codeReadyFn_(codeBatchCtx_, fn)) {
    if (tier == kEJitTierInstrumented) {
      dropUnpublishedNearResult("tier1-not-ready");
      return;
    }
    if (info.codeSize == 0 || info.poolId >= kEJitNearHotPoolCount) {
      dropUnpublishedNearResult("missing-pool-range");
      return;
    }
    if (pendingPublishes_.size() >=
        static_cast<size_t>(EJIT_CODE_POOL_FIXED_NEAR_HOT_PENDING_LIMIT)) {
      EJIT_DIAG(
          "near-hot pending capacity reached limit=%u; flushing",
          static_cast<unsigned>(EJIT_CODE_POOL_FIXED_NEAR_HOT_PENDING_LIMIT));
      autoTier2PublishPending_ = true;
      (void)flushPendingPublishes(/*compileBatchRequests=*/false,
                                  /*reason=*/"capacity");
      if (pendingPublishes_.size() >=
          static_cast<size_t>(EJIT_CODE_POOL_FIXED_NEAR_HOT_PENDING_LIMIT)) {
        dropUnpublishedNearResult("pending-capacity");
        return;
      }
    }
    const uint32_t PoolId = info.poolId;
    pendingPublishes_.push_back({req, fn, PoolId});
    autoTier2PublishPending_ = true;
    publishCodePoolStats();
    EJIT_DIAG_DEBUG("near-hot linked pending func=%u pool=%u pending=%zu",
                    realFuncIndex, PoolId, pendingPublishes_.size());
    return;
  }
#else
  if (codeReadyFn_ && !codeReadyFn_(codeBatchCtx_, fn)) {
    if (tier == kEJitTierPgoUse) {
      // Tier-2 has completed compilation/JITLink into the near RW/NX pool, but
      // must not replace the live Tier-1 slot until the worker observes the
      // compile queue empty and seals the batch. Keep only owner-private state
      // and retain the in-flight claim.
      pendingPublishes_.push_back({PublishReq, fn});
      autoTier2PublishPending_ = true;
      EJIT_DIAG_DEBUG("PGO Tier-2 linked pending queue-drain publish func=%u "
                      "fn=%p pending=%zu",
                      realFuncIndex, fn, pendingPublishes_.size());
      return;
    }
    if (tier == kEJitTierInstrumented) {
      // Tier-1 is routed to the far immediate pool. Treat a non-ready result as
      // a pool-routing/configuration failure instead of delaying profiling or
      // replacing the AOT path with an NX pointer.
      if (publishFn_)
        publishFn_(publishCtx_, PublishReq, false);
      dedupClear(req.funcIndex, req.generation);
      finishPgoOnFailure();
      if (releaseFn_)
        releaseFn_(releaseCtx_, fn);
      EJIT_STAT_INC(state_->counters.publishFailed);
      EJIT_DIAG("PGO Tier-1 not executable after compile func=%u",
                realFuncIndex);
      return;
    }
    EJitPublishStatus Staged = cacheStagePending(PublishReq, fn);
    if (Staged == EJitPublishStatus::Published) {
      pendingPublishes_.push_back({PublishReq, fn});
      dedupClear(req.funcIndex, req.generation);
      EJIT_DIAG_DEBUG("shared worker batch staged func=%u fn=%p pending=%zu",
                      req.funcIndex, fn, pendingPublishes_.size());
      return;
    }

    dropBatchRequestMarker();

    // A bucket containing only other Pending identities has no safe eviction
    // victim. Keep the result owner-private with its coarse in-flight claim;
    // the explicit publish call seals all code and then inserts this identity.
    pendingPublishes_.push_back({PublishReq, fn});
    EJIT_DIAG_VERBOSE(
        "shared worker batch retained unstaged func=%u pending=%zu",
        req.funcIndex, pendingPublishes_.size());
    return;
  }
#endif
  if (info.codeSize == 0 && codeRangeFn_)
    (void)codeRangeFn_(codeRangeCtx_, fn, &info);
  EJitPublishStatus PS = cachePublish(PublishReq, fn, &info, pgoClearExclusive);
  switch (PS) {
  case EJitPublishStatus::Published:
    EJIT_STAT_INC(state_->counters.asyncCompiles);
    if (publishFn_)
      publishFn_(publishCtx_, PublishReq, true);
    dedupClear(req.funcIndex, req.generation);
    if (tier == kEJitTierInstrumented) {
      EJIT_STAT_INC(state_->counters.tier1Compiles);
      EJIT_DIAG("PGO Tier-1 published func=%u: collecting 0/%u hits",
                realFuncIndex, state_->tier2Threshold.loadAcquire());
    } else if (tier == kEJitTierPgoUse) {
      EJIT_STAT_INC(state_->counters.tier2Compiles);
      finishPgoFunction(realFuncIndex, /*completed=*/true);
    }
    publishCodePoolStats();
    EJIT_DIAG_VERBOSE("shared worker publish ok func=%u fn=%p", req.funcIndex,
                      fn);
    return;
  case EJitPublishStatus::VersionMismatch:
    if (publishFn_)
      publishFn_(publishCtx_, PublishReq, false);
    dedupClear(req.funcIndex, req.generation);
    if (req.generation == state_->generation.loadAcquire() &&
        (tier == kEJitTierInstrumented || tier == kEJitTierPgoUse))
      finishPgoFunction(realFuncIndex, /*completed=*/false,
                        "version-mismatch-at-publish");
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared worker publish drop func=%u: version mismatch",
              req.funcIndex);
    return;
  case EJitPublishStatus::InvalidParam:
  case EJitPublishStatus::Failed:
    if (publishFn_)
      publishFn_(publishCtx_, PublishReq, false);
    dedupClear(req.funcIndex, req.generation);
    finishPgoOnFailure();
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    EJIT_STAT_INC(state_->counters.publishFailed);
    EJIT_DIAG("shared worker publish failed func=%u status=%u", req.funcIndex,
              static_cast<unsigned>(PS));
    return;
  }
}

bool EJitSharedTaskPool::pollOne() {
  if (!state_)
    return false;

  // All tiers arrive on the same shared MPSC queue. In fixed near-hot mode
  // baseline and Tier-2 compile/link immediately into their selected near
  // pool, while publication waits for the queue-drain commit. The legacy path
  // may retain baseline requests for dimension-sorted batch allocation. Tier-1
  // always compiles into the far pool and starts sampling immediately.
  EJitCompileRequest Req{};
  if (!queuePop(Req))
    return false;
#ifdef EJIT_CODE_POOL_FIXED_NEAR_HOT
  autoTier2IdleTicks_ = 0;
  runCompile(Req);
  return true;
#else
  if (codeReadyFn_ && codeBatchFlushFn_ &&
      decodeReqTier(Req.funcIndex) == kEJitTierBaseline) {
    if (pendingBatchCompiles_.size() >= kEJitSharedQueueSlots) {
      dedupClear(Req.funcIndex, Req.generation);
      EJIT_STAT_INC(state_->counters.queueFull);
      EJIT_DIAG("shared worker batch backlog full func=%u capacity=%u",
                stripReqTier(Req.funcIndex), kEJitSharedQueueSlots);
      return true;
    }
    const bool Marked =
        cacheStageBatchRequest(Req) == EJitPublishStatus::Published;
    pendingBatchCompiles_.push_back({Req, Marked});
    if (Marked)
      dedupClear(Req.funcIndex, Req.generation);
    EJIT_DIAG_VERBOSE(
        "shared worker batch queued func=%u dims=%u marker=%u requests=%zu",
        stripReqTier(Req.funcIndex), Req.numDims, static_cast<unsigned>(Marked),
        pendingBatchCompiles_.size());
    return true;
  }
  runCompile(Req);
  return true;
#endif
}

bool EJitSharedTaskPool::serviceMayConstRankingRequest() {
  if (!state_ || !isOwner_)
    return false;
  const uint32_t Request = state_->mayConstRankingRequest.loadAcquire();
  if (Request == state_->mayConstRankingComplete.loadAcquire())
    return false;

  const bool Ok = mayConstRankingFn_ && mayConstRankingFn_(mayConstRankingCtx_);
  state_->mayConstRankingResult.storeRelaxed(Ok ? 1u : 0u);
  state_->mayConstRankingComplete.storeRelease(Request);
  return true;
}

unsigned EJitSharedTaskPool::pollBudget(unsigned maxItems) {
  unsigned n = 0;
  while (n < maxItems && pollOne())
    ++n;
  return n;
}

EJitWorkerStep EJitSharedTaskPool::workerPollOnce() {
  if (!state_)
    return EJitWorkerStep::Exit;
  uint32_t st = state_->initState.loadAcquire();
  switch (static_cast<EJitSharedInitState>(st)) {
  case EJitSharedInitState::Ready:
    workerConsumeLoops_.fetchAdd(1);
    // Explicit publication must not starve behind a continuously replenished
    // compile queue or diagnostics. serviceCodeBatchRequest() snapshots and
    // drains the work that predates the request before publishing it.
    if (serviceCodeBatchRequest())
      return EJitWorkerStep::Consumed;
    if (serviceMayConstRankingRequest())
      return EJitWorkerStep::Consumed;
    if (pollOne())
      return EJitWorkerStep::Consumed;
    if (serviceAutoTier2Publish())
      return EJitWorkerStep::Consumed;
    return EJitWorkerStep::Idle;
  case EJitSharedInitState::Initializing:
    // The owner is still arming the pool. The SRE task may have been scheduled
    // before the owner published Ready; WAIT for Ready/Failed — never exit
    // early and never read the half-armed queue/cache (spec §11).
    workerWaitedForReady_.storeRelease(1);
    return EJitWorkerStep::WaitForReady;
  case EJitSharedInitState::Uninitialized:
  case EJitSharedInitState::Failed:
  case EJitSharedInitState::Stopping:
  default:
    EJIT_DIAG_DEBUG("shared worker exit: state=%u", st);
    return EJitWorkerStep::Exit;
  }
}

void EJitSharedTaskPool::runWorkerLoop() {
  EJIT_DIAG_VERBOSE("shared worker loop enter");
  // Loop until a terminal state. The worker is a PRODUCTION-lifetime task: it
  // never exits just because the owner is slightly slow to publish Ready (no
  // spin budget). On every non-consuming iteration — waiting through
  // Initializing, or Ready with an empty queue — it YIELDS the CPU via the
  // injected idle hook (platform EJitSreTask::yield), so a high-priority worker
  // cannot starve the core that must publish Ready or enqueue work. The idle
  // hook runs OUTSIDE any bucket lock / queue slot / dedup critical state
  // (pollOne returns before we idle).
  for (;;) {
    EJitWorkerStep s = workerPollOnce();
    if (s == EJitWorkerStep::Exit)
      break;
    if (s == EJitWorkerStep::WaitForReady || s == EJitWorkerStep::Idle) {
      workerIdle(1); // single yield while waiting / empty queue
    } else
      workerThrottle();
  }
  EJIT_DIAG_VERBOSE("shared worker loop leave");
}

void EJitSharedTaskPool::workerIdle(uint32_t ticks) {
  workerIdleYields_.fetchAdd(1);
  if (workerIdle_)
    workerIdle_(workerIdleCtx_,
                ticks); // platform delay(ticks): 1=yield, N=throttle
  else
    cpuRelax(); // step/unit tests with no injected hook
}

void EJitSharedTaskPool::workerThrottle() {
  if (EJIT_SRE_TASKPOOL_WORKER_THROTTLE_MULT == 0u ||
      EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS == 0u)
    return;
  // ONE platform delay(MULT*DELAY_TICKS), not DELAY_TICKS separate yields.
  const uint32_t Delay = EJIT_SRE_TASKPOOL_WORKER_THROTTLE_MULT *
                         EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS;
  workerIdle(Delay);
}

void EJitSharedTaskPool::workerEntryThunk(void *ctx) {
  static_cast<EJitSharedTaskPool *>(ctx)->runWorkerLoop();
}

//===----------------------------------------------------------------------===//
// Diagnostics (§11 observability).
//===----------------------------------------------------------------------===//
uint32_t EJitSharedTaskPool::sharedInitState() const {
  return state_ ? state_->initState.loadAcquire()
                : static_cast<uint32_t>(EJitSharedInitState::Uninitialized);
}

uint32_t EJitSharedTaskPool::pendingCount() const {
  if (!state_)
    return 0;
  uint32_t pending = 0;
  for (uint32_t i = 0; i < kEJitSharedMaxFuncIndex; ++i)
    if (state_->inFlight[i].loadRelaxed() != 0)
      ++pending;
  return pending;
}

bool EJitSharedTaskPool::requestMayConstRanking() {
  if (!state_ || state_->initState.loadAcquire() !=
                     static_cast<uint32_t>(EJitSharedInitState::Ready)) {
    EJIT_DIAG("mayconst-ranking request rejected: shared worker not ready");
    return false;
  }

  const uint32_t Generation = state_->generation.loadAcquire();
  const uint32_t Ticket = state_->mayConstRankingRequest.fetchAdd(1) + 1;
  constexpr uint32_t kMaxWaitRounds = 1u << 20;
  for (uint32_t Round = 0; Round < kMaxWaitRounds; ++Round) {
    if (state_->generation.loadAcquire() != Generation)
      break;
    const uint32_t Complete = state_->mayConstRankingComplete.loadAcquire();
    if (static_cast<int32_t>(Complete - Ticket) >= 0)
      return state_->mayConstRankingResult.loadAcquire() != 0;
    workerIdle(1);
  }
  EJIT_DIAG("mayconst-ranking request timed out ticket=%u owner=%u", Ticket,
            state_->ownerCoreId.loadAcquire());
  return false;
}

void EJitSharedTaskPool::getDiagnostics(EJitSharedDiagnostics &out) const {
  out = EJitSharedDiagnostics{};
  if (!state_)
    return;
  out.initState = state_->initState.loadAcquire();
  out.ownerCoreId = state_->ownerCoreId.loadAcquire();
  out.generation = state_->generation.loadAcquire();
  out.lastInitError = state_->lastInitError.loadAcquire();
  out.initAttempts = state_->initAttempts.loadAcquire();
  out.codeSharingEnabled = state_->codeSharingEnabled.loadAcquire();
  out.workerTaskId = state_->workerTaskId.loadAcquire();
  out.registrationFingerprint = state_->registrationFingerprint.loadAcquire();
  out.queueDepth =
      state_->enqueuePos.loadRelaxed() - state_->dequeuePos.loadRelaxed();
  uint32_t pending = 0;
  for (uint32_t i = 0; i < kEJitSharedMaxFuncIndex; ++i)
    if (state_->inFlight[i].loadRelaxed() != 0)
      ++pending;
  out.pendingCount = pending;
  uint32_t ready = 0;
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s)
      if (state_->buckets[b].slots[s].state.loadAcquire() ==
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        ++ready;
  out.cacheReadyCount = ready;
  out.cacheHits = state_->counters.cacheHits.loadRelaxed();
  out.asyncEnqueues = state_->counters.asyncEnqueues.loadRelaxed();
  out.asyncCompiles = state_->counters.asyncCompiles.loadRelaxed();
  out.alreadyPending = state_->counters.alreadyPending.loadRelaxed();
  out.queueFull = state_->counters.queueFull.loadRelaxed();
  out.compileFailed = state_->counters.compileFailed.loadRelaxed();
  out.publishFailed = state_->counters.publishFailed.loadRelaxed();
  out.instanceDisabled = state_->counters.instanceDisabled.loadRelaxed();
  out.instanceDisabledPreActivate =
      state_->counters.instanceDisabledPreActivate.loadRelaxed();
  out.executePrepareFailed =
      state_->counters.executePrepareFailed.loadRelaxed();
  out.pgoActiveFunctionCount = state_->pgoActiveFunctionCount.loadAcquire();
  out.pgoMaxActiveFunctions = state_->pgoMaxActiveFunctions.loadAcquire();
  out.pgoCompletedFunctions = state_->pgoCompletedFunctions.loadRelaxed();
  out.pgoDeferredMisses = state_->pgoDeferredMisses.loadRelaxed();
  out.tier1Compiles = state_->counters.tier1Compiles.loadRelaxed();
  out.tier2Compiles = state_->counters.tier2Compiles.loadRelaxed();
  out.profileMergeFails = state_->counters.profileMergeFails.loadRelaxed();
}
