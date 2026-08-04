# EJIT Shared Taskpool — Cache-Query Performance Audit & Model

Scope: the cross-core shared taskpool cache-hit **query** path (spec §5.2 steps
0–1), from the AOT wrapper's C ABI call through the read-token / seqlock release.
This document is the performance model behind the Commit 2 optimization; it does
**not** describe the compile/enqueue slow path.

All static instruction counts are AArch64, `clang -Os`, default capacities
(`EJIT_SRE_TASKPOOL_BUCKETS=32`, `EJIT_SRE_SHARED_TASKPOOL_CACHE_SLOTS=16`).
All per-hit measurements are on an aarch64 host via `perf stat`, stats OFF
(`EJIT_STATS_ENABLE` undefined — the production default), 100M iterations. Host
nanoseconds are for **relative comparison only**; they are not board cycles.

Reproduce with the benchmark added in Commit 1:

```
clang++ -std=c++17 -O2 -Illvm/include \
  -DEJIT_SRE_TASKPOOL -DEJIT_SRE_SHARED_TASKPOOL -DEJIT_SRE_TASKPOOL_TESTING \
  -DEJIT_SRE_TASKPOOL_BUCKETS=32 -DEJIT_SRE_TASKPOOL_QUEUE_CAPACITY=1024 \
  [-DEJIT_SRE_SHARED_CODE_POINTERS] [-DEJIT_SRE_TASKPOOL_NO_RECLAIM] \
  llvm/unittests/ExecutionEngine/EJIT/EJitSharedCacheQueryBench.cpp \
  llvm/lib/ExecutionEngine/EJIT/EJitSharedTaskPool.cpp \
  llvm/lib/ExecutionEngine/EJIT/EJitSharedPlatform.cpp -o bench
perf stat -e instructions,cycles ./bench single0d 15 100000000
```

Or via CMake: configure with `-DEJIT_BUILD_CACHE_QUERY_BENCH=ON` (and
`-DEJIT_TEST_NO_RECLAIM=ON` for a benchmark-only seqlock + code-sharing
variant), then build `EJITSharedCacheQueryBench`. Production builds enable the
same implementation with `-DEJIT_SRE_TASKPOOL_NO_RECLAIM=ON`; this requires
`EJIT_SRE_SHARED_TASKPOOL=ON` and is safe only while published JIT code is never
physically reclaimed. The `ejit-minimal-aarch64_be` preset enables it.

---

## 1. Baseline call chain (per layer)

```
wrapper IR / AOT entry
 └─ ejit_taskpool_compile_or_get[_0d/_1d/_2d]     (C ABI shell, EJitRuntime.cpp)
     └─ EJitSharedTaskPool::tryCacheHit[0D/1D/2D]  (Ready gate + instance-enabled)
         └─ cacheLookup[0D/1D/2D] | cacheLookupSeq[0D/1D/2D]
             ├─ hashIdentity (inlined / unrolled)  key
             ├─ bucket = key % 32
             ├─ bucketTryRead  (token: 1 acquire load + 1 RMW; seqlock: 1 load)
             ├─ slot scan 0..15  (per slot: identityHash reject → state acquire →
             │                    generation → identity → version)
             └─ resolveMatchedSlot
                 ├─ owner core / memoized peer → return fnPtr (+ read token)
                 └─ cold peer first touch → peerPrepareSlot (noinline)
         └─ classifyHit  (CacheHit / InstanceDisabled / OffMode / notShareable)
 └─ JIT function indirect call
 └─ releaseRead  (token: 1 RMW; seqlock: no-op sentinel)
```

### Static per-layer instruction counts (AArch64, -Os)

| Layer (function)                 | token | NO_RECLAIM |
|----------------------------------|------:|-----------:|
| `ejit_taskpool_compile_or_get_0d` (C shell) | ~20 | ~20 |
| `tryCacheHit0D`                  | 36 | 43 |
| `tryCacheHit1D`                  | 48 | 54 |
| `tryCacheHit2D`                  | 55 | 61 |
| `tryCacheHit` (generic)          | 55 | 61 |
| `cacheLookup0D` / `cacheLookupSeq0D` | 69 | 68 |
| `cacheLookup1D` / `cacheLookupSeq1D` | 104 | 109 |
| `cacheLookup2D` / `cacheLookupSeq2D` | 140 | 144 |
| `cacheLookup` (generic)          | 136 | 138 |
| `resolveMatchedSlot`             | 36 | 42 |
| `peerPrepareSlot` (cold, noinline) | 150 | ~150 |
| `classifyHit`                    | 22 | 32 |
| `hashIdentity` (generic)         | 16 | 16 |
| `releaseRead`                    | 14 | 1 (no-op) |

Static counts are whole-function sizes (they include the miss/error tails and
the 16-iteration loop body once). The **executed** hit path touches only a
subset — see §3.

### Per-basic-block path classification

For the fixed-dim lookups the blocks partition as:
- **cache-hit blocks**: hash build → `bucketTryRead` success → the *matching*
  slot iteration → `resolveMatchedSlot` owner/memoized return.
- **miss-only blocks**: the loop `continue` edges (rejected slots) and the
  fall-through `bucketReadRelease; return R`.
- **owner-only blocks**: `resolveMatchedSlot` `self == owner` early return.
- **peer-only blocks**: `resolveMatchedSlot` memoized-mask branch + the call to
  `peerPrepareSlot`.
- **error-only blocks**: `numDims > 4` guard (generic only), `!bucketTryRead`
  contended fallback, `fnPtr == 0` racing-publish fallback.

---

## 2. Actual cache-hit executed path (measured, stats OFF)

`insns/hit` and `ns/hit` are the perf-measured loop totals; the benchmark loop
harness is **~14 instructions** (measured from the disassembled loop:
call-arg setup, `gSink` volatile add, status/hasReadToken test, loop counter),
so the executed query path is `insns/hit − 14`.

| Config (owner core, stats off) | slot | insns/hit | ns/hit | exec path insns |
|--------------------------------|-----:|----------:|-------:|----------------:|
| token 0D                       | 0    | 124 | 20.2 | ~110 |
| token 0D                       | 15   | 214 | 22.7 | ~200 |
| token 1D                       | 0    | 184 | 24.1 | ~170 |
| token 2D                       | 0    | 221 | 25.3 | ~207 |
| NO_RECLAIM 0D                  | 0    | 119 | 9.0  | **~105** |
| NO_RECLAIM 0D                  | 15   | 224 | 15.1 | ~210 |
| NO_RECLAIM 1D                  | 0    | 163 | 11.5 | ~149 |
| NO_RECLAIM 2D                  | 0    | 220 | 15.6 | ~206 |
| NO_RECLAIM 0D peer (memoized)  | 0    | 125 | 9.5  | ~111 |

Dominant costs, in order:
1. **Token read/release RMW** (`bucket.readers` fetchAdd + fetchSub, two
   `ldaddal`): ~11 ns of the 20 ns token 0D hit. The seqlock (NO_RECLAIM) build
   removes both → **2.0–2.3× faster** at slot 0.
2. **Linear 16-slot scan**: ~0.8 ns and (post-opt) ~6 instructions per scanned
   non-matching slot. Only material for busy buckets / deep slots.
3. **`resolveMatchedSlot` owner gate**: `EJitCoreId::current()` + `ownerCoreId`
   load + branch.
4. Per-slot `state` **acquire load** (`ldar`) — one per scanned slot in the old
   order; skipped for rejected slots after the reorder (§4).

---

## 3. Slot-depth distribution

`cachePublish` fills the first empty slot, so entries in one bucket stack at
slots 0,1,2,… in publish order; a bucket evicts its slot-0 occupant when full.
With 32 buckets and a good hash, the expected occupancy per bucket for N live
specializations is N/32; the hit slot depth is therefore **0 for the vast
majority** of realistic workloads and only grows for hash-colliding identities
or > 32 hot specializations.

The Commit 1 benchmark (`bench0D`) and the `SlotDepthHitsAtEveryDepth` test
place a target at each depth 0/1/5/15 deterministically (0D funcIndex multiples
of 32 collide into bucket 0). Measured slope: token **+0.83 ns/slot** before,
**+0.16 ns/slot** after the reorder; NO_RECLAIM **+0.92 ns/slot** before,
**+0.41 ns/slot** after.

---

## 4. Candidate directions — benefit / risk

| Dir | Idea | Benefit | Mem cost | Concurrency / version risk | Verdict |
|-----|------|---------|----------|----------------------------|---------|
| A | slot-depth stat | diagnostic only | 0 | none | shipped as bench + test |
| B | per-funcIndex slot **hint** | cuts deep-slot scan | +4–8 B/func shared | must re-validate gen+identity+version; stale slot must not alias | **not shipped** — real depth ≈0; complexity/risk > benefit |
| C | direct-mapped primary slot | O(1) common hit | +shared index | different specialization must not alias same funcIndex | **not shipped** — aliasing risk, marginal at depth 0 |
| D | version digest word | 1 u32 compare vs per-dim loop | +4 B/slot | digest collision must never authorize stale code (hint only) | **report only** — needs ABI bump; per-dim loop already cheap (≤4) |
| E | packed epoch/state word | 1 read vs several | +8 B | epoch may only *reject* / gate slow check | **report only** — needs strict equivalence proof + ABI bump |
| F | owner-only hot/cold split | smaller I-cache footprint | 0 | must not bypass read token | partially present (`peerPrepareSlot` already noinline); owner path already tight |
| G | move `identityHash` into slot line 0 | 1 cache line/scan vs 2 | 0 (reorder) | big-endian safe (by-value); **changes shared layout → ABI bump** | **report only** — see §5 |
| H | hash algo | — | — | — | **not changed**: 0D≈funcIndex, 1D/2D few XOR+mul, `%32`→`&31` already; objdump shows hash is not the bottleneck |
| **I** | **fixed-dim seqlock `cacheLookupSeqNd`** | −20 insns vs generic in NO_RECLAIM | 0 | never-free precondition (already the NO_RECLAIM invariant) | **SHIPPED** (Commit 2) |
| — | **identityHash-first scan reorder** | −32/−36 % ns at depth 15 | 0 | identical semantics (final gate unchanged) | **SHIPPED** (Commit 2) |
| J | wrapper call-site local cache | skips ABI shell | +8 B/site | cross-core/deactivate/version correctness unproven | **not shipped** — see the separate `ejit-callsite-local-cache` experiment; needs strict proof |

---

## 5. What shipped (Commit 2)

1. **Fixed-dimension seqlock specializations** `cacheLookupSeq0D/1D/2D`
   (Direction I / priority #1). In NO_RECLAIM builds `tryCacheHit0D/1D/2D`
   previously fell back to the *generic* `cacheLookupSeq` (generic hash loop +
   `slotIdentityMatches`). They now use unrolled specializations mirroring
   `cacheLookup0D/1D/2D`. Isolated entirely behind `EJIT_SRE_TASKPOOL_NO_RECLAIM`
   (the "code never freed" build), satisfying the never-free precondition
   constraint. No new shared state, no layout/ABI change.
2. **identityHash-first scan reorder** across `cacheLookup`,
   `cacheLookup0D..4D`, `cacheLookupSeq`, and the new `cacheLookupSeq0D/1D/2D`.
   The per-slot loop now rejects on the plain `identityHash` compare **before**
   the acquiring `state` load. Correctness is unchanged: a match still requires
   the acquire `state==Ready` load **plus** the full `funcIndex/numDims/dims`
   identity **plus** the per-dim version check before any pointer is returned;
   `identityHash` is only a fast-reject hint (a 64-bit hash collision falls
   through to the authoritative identity compare, and a stale/false-negative
   read is a benign clean miss under both the bucket read token and the seqlock
   `publishSeq` re-check). No layout/ABI change; big-endian safe (by-value
   scalar compares).

**Before → after** (owner, stats off):
- token 0D slot 15: 304 → 214 insns, 33.2 → 22.7 ns (**−32 %**)
- NO_RECLAIM 0D slot 0: 139 → 119 insns, 9.6 → 9.0 ns
- NO_RECLAIM 0D slot 15: 364 → 224 insns, 23.4 → 15.1 ns (**−36 %**)
- token 0D slot 0 unchanged (single slot, nothing to skip)

Shared-state memory change: **0 bytes** (`sizeof(EJitSharedTaskPoolState)` and
every slot/bucket offset identical; verified by static_assert + offsetof probe).
New shared reads/writes per hit: **0** (both changes are pure code).

---

## 6. Not shipped — reasons

- **B/C slot hint / primary slot**: realistic hit depth is ≈0 (N/32 occupancy),
  so the win is confined to pathological buckets while the correctness surface
  (stale-slot aliasing, generation/version re-validation) grows. Kept as a
  measured direction only.
- **D/E/G packed digest / epoch / hot-field layout**: each requires changing the
  shared `EJitSharedCacheSlot` layout (or adding a word), i.e. an
  `abiVersion` bump and cross-core coordinated redeploy. The audit's hard
  constraint is to not change default ABI / cache identity, and the measured
  benefit (per-dim version loop is ≤4 iterations; `identityHash` already
  fast-rejects) does not justify the ABI churn. Direction G (move `identityHash`
  to the first cache line so a scan touches one line instead of two) is the most
  promising future ABI-bump candidate and is documented here for that purpose.
- **H hash**: objdump confirms the hash is a handful of instructions and not the
  bottleneck; `% 32` already lowers to `& 31`. Changing it risks collisions for
  no measured gain.
- **J wrapper call-site cache**: not correctness-provable against deactivate /
  version-bump / cross-core code sharing without a generation+version guard that
  costs about as much as the lookup it replaces.

---

## 7. The "< 100 instructions" target

The common **NO_RECLAIM owner-core 0D** hit executes **~105** instructions
(stats off, slot 0), just above the 100 target; token 0D is ~110. The residual,
from the disassembly, is:
- the cross-TU `bl` chain (C ABI shell → `tryCacheHit0D` → `cacheLookupSeq0D` →
  `resolveMatchedSlot`) with its arg setup / return-struct spill (~30 insns of
  call glue that a full-image **LTO** build collapses),
- `EJitCoreId::current()` (a real call for the core id) + the owner gate,
- the seqlock `bucketSeqBegin`/`bucketSeqStable` pair (~6 insns),
- `classifyHit`'s outcome classification + `CompileOrGetResult` construction.

With whole-image LTO (the production firmware link, where these functions inline
into the wrapper), the call glue disappears and the path is expected to fall
below 100. On the token path the two `readers` RMWs make the instruction count
secondary to the atomic latency; the seqlock build is the one that both hits
~105 instructions and is 2× faster in cycles.

---

## 8. Correctness invariants preserved

Deactivated instance never served (instance-enabled gate unchanged); version
bump invalidates old slots (per-dim version check still after identity);
generation reset drops old slots (`generation` compare unchanged); identity-hash
collision cannot alias (authoritative identity compare after the hash reject);
peer without execute permission never gets a pointer (`resolveMatchedSlot` /
`peerPrepareSlot` untouched); read token pairing intact (token path unchanged;
seqlock takes no token by construction); publish/overwrite races handled (bucket
write lock / `publishSeq` seqlock unchanged); POD / standard-layout / trivially
destructible shared state unchanged; no dynamic init; stats OFF keeps the hot
path free of counter RMW. Verified by the existing suite plus the new
`SlotDepthHitsAtEveryDepth`, `SlotDepthNoIdentityAliasAcrossBucket`,
`SeqFixedDimMatchesGeneric0D1D2D`, and `SeqFixedDimVersionBumpMisses` tests.

---

## 9. Still to validate on-board

Host cannot model real cross-core cache coherence or SRE `enable_ex`/split
latency; the state machine and return semantics are deterministically tested on
host, but the actual peer first-touch seal cost, the cross-core `ldar`/RMW
latency, and the board cycle counts must be measured on hardware. Host
nanoseconds here are for relative comparison only.
