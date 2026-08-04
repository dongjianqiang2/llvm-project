# EJIT Multi-Version Inline Cache (Direct-Indexed, Per-Core Private)

> Extends the v2 sticky-monomorphic inline cache (`ejit_icache_design.md`) to
> **polymorphic** specialization: one cached slot per `(instanceId…)` identity,
> so an `ejit_entry` called with different `ejit_dim` parameter values serves
> the correct specialization for each. Baseline is the `ejit-integrate-djq`
> direct-load hit path (`@__ejit_icache_fn_<name>` + `gIcacheFnSlots`).

## 1. Problem

v2 is **monomorphic**: one frozen slot per funcIndex, first resolver wins, read
forever. It is correct only if every `ejit_entry` is always called with the same
dim identity. If a function is called with different `ejit_dim` argument values
(e.g. `funcA(ejit_dim(P0) x)` called with `x=0` then `x=1`), v2 serves the
first-resolved specialization for **all** calls -> wrong code.

This doc makes the cache key on the `ejit_dim` argument values, holding one
frozen fnPtr per identity. Dimensionality = number of `ejit_dim` params.

| Function signature | Cache shape |
|---|---|
| `funcA(ejit_dim(P0) x, ejit_dim(P1) y)` | `g_funcA_cache[D][D]` |
| `funcA(ejit_dim(P0) x, y)` | `g_funcA_cache[D]` |
| `funcA(x, y)` (no `ejit_dim`) | `g_funcA_cache` (scalar = v2, unchanged) |

## 2. Hard preconditions (unchanged from v2, extended per-identity)

1. **Specialization is invariant for process lifetime for a given instanceId.**
   Period toggles (activate/deactivate) do NOT change an instance's baked code;
   a recompile of the same identity produces equivalent code, and under
   `EJIT_SRE_TASKPOOL_NO_RECLAIM` the old fnPtr is never freed and remains
   executable+correct. -> the cached fnPtr never goes stale/wrong. **No version
   check is needed** (decision: same-instanceId stable, no validation).
2. **`ejit_dim` argument values are small non-negative integers in `[0, D)`.**
   `ejit_dim` (`EjitPeriodArrInd`) marks a param as an index into a period
   array, so the value is a dense array index. This is a product contract - the
   hit path does NO bounds check and indexes the array directly with the raw
   argument value; an out-of-range value is undefined (caller's responsibility).
3. **JIT code is never freed in production** (NO_RECLAIM + no releaser wired).
   The reclamation safety gate (§6) auto-disables the cache if a releaser is
   ever wired; production wires none.

If (1) ever fails (mutable per-instance constants), a version field per cell +
hit-path compare becomes mandatory (future work, out of scope here).

## 3. Data structure

### Per-function AOT global (per-core private)
```c
// numDims = 0  -> ptr  g_funcA_cache;                 // scalar (= v2)
// numDims = 1  -> ptr  g_funcA_cache[D];
// numDims = 2  -> ptr  g_funcA_cache[D][D];
// numDims = n  -> ptr  g_funcA_cache[D][D]…[D];        // n dims, row-major
```
**Symbol name** is `@__ejit_icache_fn_<name>` (kept from v2); only the type
varies: scalar for 0-dim, `[D]^n` array for n-dim. `D = EJIT_ICACHE_DIM_SIZE`
(uniform, power-of-2, product-defined). Each cell is one `uintptr_t` (the frozen
fnPtr, 0 = empty), 8-byte aligned. **Only fnPtr is stored** - no
instanceId/version/generation: direct indexing has no collision within bounds,
so no validation fields are needed (this is the key hit-overhead saving vs a
hash+validate cache).

Emitted by `EJitWrapperGen` as an InternalLinkage global in **default section**
(`.bss`). Under the SRE memory model (per-core private by default; only
`EJIT_SHARED_SECTION_ATTR`/`.mc_shared` is cross-core shared), this global is
**per-core private** - exactly like the v2 `@__ejit_icache_fn_<name>`. Each core
owns its own cache array; this is what lets the hit path have no cross-core gate
(see `ejit-runtime-environment-facts` C3, `ejit-icache-slots-per-core-private`).

### Runtime registration table (per-core private)
```c
struct EJitIcacheSlotReg {
  uintptr_t *base;    // &g_funcA_cache (this core's private global)
  uint32_t numDims;        // dimensionality (fixed per function)
};
EJitIcacheSlotReg gIcacheSlots[EJIT_ICACHE_FUNC_SLOTS];  // keyed by funcIndex
```
Replaces v2's `uintptr_t *gIcacheSlots[FUNC_SLOTS]`. `numDims` is stored
so the runtime fill path can linearize. `D` is a compile-time constant
(`EJIT_ICACHE_DIM_SIZE`) shared by AOT and runtime, so it is NOT stored.

## 4. Hit path (frame-less, minimal overhead)

The `ejit_dim` arguments are already in registers (they are ordinary params).
The hit path uses the **argument values directly** as indices - it does NOT load
dimType globals (dimType is the taskpool's key, not the icache's; for a given
function the dimTypes are fixed by the signature, so `(i0,i1,…)` alone identifies
the specialization).

`-ejit-inline-cache` (default off) emits the probe; otherwise the wrapper goes
straight to the funcidx guard / `compile_or_get` (no icache).

### Lever B: frame-less wrapper + noinline MissFn

The wrapper (`ejit_entry` itself) is a **frame-less hit path**: just the probe
(GEP + plain load + null-check) + two **tail calls** (`br spec` on hit, `br
MissFn` on miss). No allocas, no calls, no frame setup -- the hot path never
touches `sp`. The slow path (funcidx guard + `compile_or_get` + dispatch + AOT
fallback) is extracted into a per-function `noinline MissFn` (`<name>_miss`),
which has its own frame. Miss is rare, so the tail-call overhead is irrelevant.

The two tail calls are `musttail` (so they lower to `br` and the wrapper needs
no frame) **only when `!EJitWrapperTiming`**; under `-ejit-wrapper-timing` the
hit path inserts `trace_now`/`trace_wrapper` between the call and `ret`, which
violates the musttail rule, so it becomes a framed plain call + trace + ret
(the miss path stays musttail). MissFn must inherit F's target attributes
(`target-cpu`/`target-features`/`tune-cpu`) via `setAttributes(F->getAttributes())`
-- `Function::Create` only inherits module-default `uwtable`/`frame-pointer`,
and a bare MissFn fails `TTI::areInlineCompatible`, so the CGSCC InlinerPass
rejects all helper inlining into MissFn with "conflicting attributes" (zero
inlining into the AOT fallback).

An `llvm.expect(IHit, true)` hint marks the hit branch likely, so regalloc /
shrink-wrapping keeps the hit path frame-less for all dims (including 0-dim
where the arg is idle in the hit path).

### AOT IR shape (numDims = 2, D power-of-2, icache ON)
```
define i32 @funcA(args...) {
jit_entry:                                     ; frame-less entry
  slot = GEP @__ejit_icache_fn_funcA, 0, i0, i1  ; multi-dim, shifts
  p = load slot, align 8                         ; plain ldr (no atomic)
  hit = expect(p != null, true)
  br hit, label %jit_icache_dispatch, label %jit_miss
jit_icache_dispatch:                           ; hit: tail-call spec
  tail call void %p(args...)
  ret
jit_miss:                                      ; miss: tail-call MissFn
  tail call void @funcA_miss(args...)
  ret
}

define internal noinline void @funcA_miss(args...) {  ; slow path (own frame; inherits F target-cpu/features + section)
  funcidx = load @__ejit_funcidx_funcA
  if funcidx valid: compile_or_get -> dispatch (call spec + release_read) / fallback
  else: fallback (AOT body)
}
```

### Lowered aarch64 (`-Os`, lever B, all dims frame-less)
```
; 0-dim:  adrp x8,@slot; ldr x1,[x8,:lo12:@slot]; cbz x1,.miss; br x1     ; 4 instr
; 1-dim:  adrp x8; add x8,x8,:lo12:; ldr x1,[x8,w0,sxtw#3]; cbz; br        ; 5 instr
; n-dim:  per non-last dim i: sign-extend the ejit_dim arg + add lsl #(3+3*(numDims-1-i))
;         (D=16 strides: 2-dim lsl#6; 3-dim lsl#9,#6; 4-dim lsl#12,#9,#6);
;         the last dim folds its sign-extend into the ldr [.,wN,sxtw#3] addressing.
; .miss:  b <name>_miss     ; cold: tail-call MissFn
```

| numDims | hit instr (measured, `-Os` aarch64_be) |
|---|---|
| 0 | 4 |
| 1 | 5 |
| 2 | 6 |
| 3 | 8 |
| 4 | 9 |

Indexing is pure shifts (`lsl #N`, no multiply -- D is power-of-2); the last
dim folds its sign-extend into the `ldr` addressing. No bounds check, no C call,
no read-token, no `release_read`, no dimType load, no version compare, no frame.
Counts above are the latest measured hit-path instruction totals (probe + tail-call).

### numDims > EJIT_ICACHE_MAX_DIMS (default 4) -> compile error
An `ejit_entry` with more than `EJIT_ICACHE_MAX_DIMS` `ejit_dim` params is
**rejected at compile time** with a diagnostic; the wrapper emits no probe and
does not wrap such a function. `EJIT_ICACHE_MAX_DIMS` defaults to 4, matching
the existing taskpool `DimCount` cap - exceeding it is a programming error, not
a silent fallback.

## 5. Fill path (cold, on miss)

Miss -> `jit_slow` -> `ejit_taskpool_compile_or_get(funcIndex, dims, numDims)`
-> on success (cache hit or fresh compile) `icacheFill` writes this identity's
cell:

```c
void icacheFill(uint32_t funcIndex, void *fnPtr,
                const EJitDimPair *dims, uint32_t numDims) {
  if (!icacheReclamationSafe_) return;
  if (!state_ || !fnPtr || funcIndex >= EJIT_ICACHE_FUNC_SLOTS) return;
  EJitIcacheSlotReg *r = &gIcacheSlots[funcIndex];
  if (!r->base || numDims != r->numDims) return;   // shape mismatch: skip
  // Horner linearization, row-major, dim0 = leftmost ejit_dim param.
  // MUST match the AOT array's declaration order.
  uintptr_t idx = 0;
  for (uint32_t i = 0; i < numDims; ++i)
    idx = idx * EJIT_ICACHE_DIM_SIZE + dims[i].instanceId;
  // Plain store: the slot is per-core private, so this write (calling core) is
  // ordered before the wrapper's read (same core) by program order; the
  // specialization is invariant per identity, so overwrite is harmless. No CAS,
  // no release.
  r->base[idx] = (uintptr_t)fnPtr;
}
```
Called from every `compile_or_get[_Nd]` success path via
`ejitIcacheFillOnSuccess(funcIndex, fnPtr, dims, numDims)` (extend the existing
helper to forward `dims`/`numDims`, which the compile request already carries).

**Plain store (no CAS)**: the slot is per-core private, so the write (calling
core) is ordered before the wrapper's read (same core) by program order. The
specialization is invariant per identity, so overwriting on a later resolve
(same pointer) is harmless. No one-shot CAS, no release.

### Linearization order
Row-major, `dim0` = the leftmost `ejit_dim` param (the order `getPeriodArrIndInfo`
returns, which is also the order the AOT declares the array dims). The AOT GEP
and the runtime Horner loop must agree. `dims[i].instanceId` is the arg value
(`emitInstanceVal` truncs/zexts the arg to i32).

## 6. Safety

### Reclamation gate (same as v2)
`setReleaser(fn,ctx)` sets `icacheReclamationSafe_ = (fn == nullptr)`. Multi-
version does no HP-scan retire; a wired releaser (code may be freed) + any cached
cell = UAF. The gate disables `icacheFill` (no-op) when a releaser is wired.
**Note (unchanged v2 exposure):** the AOT direct-load probe is a plain load with
no runtime gate, so under a releaser it would still fire and UAF - production
wires no releaser, so the gate stays open. Same contract as v2.

### Per-core seal (carries over per-cell)
The icache array is per-core private. A core fills cell `(i0,…)` only on its own
`compile_or_get` slow path, which for a non-owner runs `peerPrepareSlot` ->
`prepareExecForCurrentCore` -> `sealAndSyncCache` (seal + DC CVAU/IC IVAU on THIS
core) before returning the fnPtr. So a cell is non-null on a core only after that
core sealed that fnPtr. A core can never hit a cell before sealing its code on
that core. The per-core-private array is precisely what keeps the gate-free hit
path safe (see `ejit-icache-slots-per-core-private`). Each cell is sealed-then-
filled independently per core.

### Version (no check)
By precondition (§2.1), same instanceId = stable specialization; under NO_RECLAIM
the cached fnPtr never dangles and never goes wrong. No version field, no
hit-path compare.

## 7. Configuration

| Flag / define | Default | Effect |
|---|---|---|
| `-ejit-inline-cache` (AOT cl::opt) | off | emit the multi-version probe |
| `EJIT_ICACHE_DIM_SIZE` (CMake -> `#define` on both LLVMEmbeddedJIT + LLVMEJIT) | 16 (power-of-2) | uniform per-dim bound D |
| `EJIT_ICACHE_MAX_DIMS` (header `#define`) | 4 | max cached dims; >N -> compile error |
| `EJIT_ICACHE_FUNC_SLOTS` | 64 | registration table size (unchanged) |

**Wiring (one CMake var drives both AOT and runtime so D agrees):**
```cmake
set(EJIT_ICACHE_DIM_SIZE 16 CACHE STRING "...")
# AOT pass (LLVMEmbeddedJIT) + runtime (LLVMEJIT) both get the #define:
target_compile_definitions(LLVMEmbeddedJIT PRIVATE EJIT_ICACHE_DIM_SIZE=${EJIT_ICACHE_DIM_SIZE})
target_compile_definitions(LLVMEJIT PRIVATE EJIT_ICACHE_DIM_SIZE=${EJIT_ICACHE_DIM_SIZE})
```
Both use the `#ifndef EJIT_ICACHE_DIM_SIZE` default (16) from `EJitCommon.h` if
CMake doesn't override. The preset sets `EJIT_ICACHE_DIM_SIZE=16`. **Power-of-2 D
is required**; a non-power-of-2 value is **rejected at CMake configure**.

### Memory budget (per function, per core)
`D^numDims * 8` bytes, zero-filled (`.bss`, no ctor).

| D \ numDims | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| 4 | 8B | 32B | 128B | 512B | 2KB |
| 8 | 8B | 64B | 512B | 4KB | 32KB |
| 16 | 8B | 128B | 2KB | 32KB | 512KB |

×32 cores × N `ejit_entry` functions. D=16 + maxDims=4 is a safe default
(≤32KB/func/core at 4 dims); D=16 + maxDims=2 keeps every function ≤2KB. The
product picks D + maxDims to fit the per-core BSS budget.

## 8. AOT changes - `EJitWrapperGen`

1. **Cache global**: `getOrCreateIcacheFnGlobal(M, name, numDims)` emits
   `@__ejit_icache_fn_<name>` (symbol kept from v2) typed as a
   `[D][D]…[D]` (numDims dims) `uintptr_t` array (scalar for numDims=0).
   numDims > `EJIT_ICACHE_MAX_DIMS` -> compile error.
2. **Lever B -- frame-less wrapper + MissFn**: for icache ON, the wrapper
   (`ejit_entry`) emits ONLY the probe (GEP + plain `ldr` + `expect` +
   `cbz`) + two tail calls (`br spec` on hit, `br MissFn` on miss). No
   allocas, no frame. The slow path (funcidx guard + `compile_or_get` +
   dispatch + AOT fallback) is extracted into a per-function `noinline
   MissFn` (`<name>_miss`), which owns the frame and the original body.
   The `emitSlowPath` lambda is shared by icache-off (in F) and icache-on
   (in MissFn). MissFn is created with `Function::Create` then
   `setAttributes(F->getAttributes())` (copy `target-cpu`/`target-features`/
   `tune-cpu` so `TTI::areInlineCompatible` passes and helpers can inline
   into MissFn) + `addFnAttr(NoInline)` + `setSection(F->getSection())`.
3. **Hit-path probe**: multi-dim `GEP` (NO bounds check), plain `load` (no
   atomic/acquire -- per-core-private slot), `expect(hit, true)` (hint hit
   likely), null-check -> `jit_miss` (tail-call MissFn). numDims=0 = scalar
   load (no GEP). Hit: `tail call spec(args); ret` (lowers to `br`).
4. **numDims > MAX_DIMS** (default 4): compile error (diagnostic).
5. **Registration**: `emitIcacheSlotRegistration` calls
   `ejit_register_icache_slot(name, base, numDims)` (3rd arg = numDims).
6. **Idempotency** (`isAlreadyWrapped`): recognizes a `LoadInst` OR
   `GetElementPtrInst` of `@__ejit_icache_fn_<name>` (multi-version GEP)
   in the entry block, in addition to `@__ejit_funcidx_`.
7. **Wrapper timing** (`-ejit-wrapper-timing`): the hit path keeps its
   sentinel (`0xFE`) report; the slow path (in MissFn or F) keeps the
   `trace_now` + `trace_wrapper` sequence. Because trace calls are inserted
   between the call and `ret`, the hit path can no longer be `musttail` and
   demotes from frame-less to a framed plain call + trace + ret (the miss
   path stays musttail). The frame-less cycle figures in §11 assume
   `!EJitWrapperTiming`.

## 9. Runtime changes

1. **Registration table**: `gIcacheSlots[FUNC_SLOTS] = {uintptr_t *base,
   uint32_t numDims}` (per-core private). `ejitIcacheRegisterSlot(funcIndex,
   base, numDims)` and `ejitIcacheClearAll` (null base pointers).
2. **`ejit_register_icache_slot(name, base, numDims)`**: forward `numDims`
   (carried in the `.ejit_period` entry's `size` field; the walker in
   `EJit.cpp` reads it).
3. **`icacheFill(funcIndex, fnPtr, dims, numDims)`**: Horner-linearize and
   **plain store** the cell (no CAS, no atomic -- per-core private, §5).
4. **`icacheTry(funcIndex, dims, numDims, &outFn)`** (test/diagnostic only):
   plain load the cell (no acquire).
5. **`ejitIcacheFillOnSuccess`**: forward `dims`/`numDims` from the compile
   request to `icacheFill` at every success site (0D..4D + generic).
6. **lipo**: `ejit_register_icache_slot` is already in `lipo.py`
   `optional_api`; the symbol name is unchanged (only gained a `numDims`
   arg), so **no lipo change needed**.

## 10. Testing

- **Lit** (`llvm/test/Transforms/EmbeddedJIT/ejit-wrapper-gen-icache.ll`):
  - 0/1/2/3/4-dim probes emit the right `[D]^n` global + GEP shape (no bounds
    checks); 0-dim unchanged vs v2.
  - numDims > MAX_DIMS is a compile error (diagnostic).
  - `-ejit-inline-cache` off emits no probe (regression).
  - No `release_read` on the icache path; idempotency (`isAlreadyWrapped`).
- **Unit** (`EJitSharedTaskPoolTest.cpp`):
  - Fill + hit for a 2-dim identity `(0,0)`, `(0,1)`, `(1,0)` -> distinct cells,
    each serves its own fnPtr (the core multi-version behavior).
  - Re-fill: a second resolve of the same identity overwrites (plain store, no
    CAS); the served pointer is unchanged (same invariant fnPtr).
  - Safety gate: releaser wired -> `icacheFill` no-op, `icacheTry` misses.
  - Per-core-private: two "cores" (test fixtures) each fill their own array; one
    core's fill is not visible to the other (mirrors v2 per-core test).
- **Integration**: call `funcA(ejit_dim x)` with `x=0,1,2` on multiple cores;
  assert each identity gets its own specialization and `icacheHits>0` per
  identity after warmup, with the slow-path count == numIdentities (one miss
  each) per core.

## 11. Performance (measured, lever B frame-less, `-Os` aarch64_be, D=16)

| numDims | LLVM 指令数 | **实测 cycle** | ILP 收益 | vs v2 (~7-8c) |
|---|---|---|---|---|
| 0 | 4 | **4** | 0 (1:1) | 更快 |
| 1 | 5 | **5** | 0 (1:1) | 更快 |
| 2 | 7 | **6** | -1 (`sxtw` 与 `adrp` 重叠) | 更快 |
| 3 | 9 | **8** | -1 | 持平 |
| 4 | 11 | **9** | -2 (索引链更长) | +1~2c |

- **Hit path**: GEP shifts + 1 plain `ldr` + `cbz` + `br` (tail call). No frame,
  no arg save/restore, no allocas, no C call, no cross-core RMW, no slot scan,
  no dimType load, no version compare, no `release_read`.
- **Frame-less for ALL dims** (lever B): the slow path is in a separate
  `noinline MissFn`, so the wrapper never sets up a frame. An `llvm.expect`
  hint ensures the hit branch is preferred.
- **ILP**: 2-4 维的实测 cycle **比指令数还少** -- `sxtw`（ALU 符号扩展）与
  `adrp`（PC 相对寻址，有访存延迟）并行执行，被流水线重叠。维度越高，索引链
  越长，能重叠的越多（4 维省 2 条）。
- **vs v2**: 0-3 维**优于或持平 v2 单态**（lever B 去掉了帧 + ldar，省下的比
  多维索引加的还多）。4 维仅 +1~2 cycle。多版本的代价几乎为零。
- **Cold**: O(1) per miss (Horner linearize + one `str`). No scan. Miss
  tail-calls `MissFn` (1 `b` instruction).

> 注：本节 frame-less / cycle 数据前提是 `!EJitWrapperTiming`（见 §8 item 7）。
> 开启 `-ejit-wrapper-timing` 时 hit 路径退化为带帧 plain call + trace + ret，
> cycle 不适用。

### 逐维度命中路径汇编（`-Os`，入口 -> `br spec`）

#### 0 维（标量槽，4 cycle）
```asm
adrp  x8, @__ejit_icache_fn_<name>
ldr   x1, [x8, :lo12:@__ejit_icache_fn_<name>]   ; :lo12: 折进 ldr
cbz   x1, .miss
br    x1
```

#### 1 维（5 cycle）
```asm
adrp  x8, @__ejit_icache_fn_<name>
add   x8, x8, :lo12:@__ejit_icache_fn_<name>
ldr   x1, [x8, w0, sxtw #3]    ; dim0*8, sxtw 折进 ldr 寻址
cbz   x1, .miss
br    x1
```

#### 2 维（6 cycle）
```asm
sxtw  x8, w0                   ; dim0 符号扩展（与 adrp 并行 -> ILP 隐藏）
adrp  x9, @__ejit_icache_fn_<name>
add   x9, x9, :lo12:@__ejit_icache_fn_<name>
add   x8, x9, x8, lsl #6       ; + dim0*64  (D*8 = 16<<3 = <<7... 实际 D=16: <<7)
ldr   x2, [x8, w1, sxtw #3]    ; + dim1*8,  sxtw 折进 ldr
cbz   x2, .miss
br    x2
```

> 注：上例 stride 基于 D=8（`lsl #6` = `x*D*8 = x*64`）。D=16 时 stride 变为
> `lsl #7`（`x*128`），原理相同（纯移位），指令数不变。

#### 指令结构拆解

每维（非末维）= 1 `sxtw` + 1 `add lsl#N`（2 条）；末维的 `sxtw` 折进 `ldr` 寻址（1 条）。
- `adrp` + `add :lo12:` = 基址（2 条；0 维把 `add` 折进 `ldr` -> 1 条）。
- `cbz` + `br` = 判空 + 尾调用（2 条）。
- `.miss` = `b <name>_miss`（冷路径，不在命中计数内）。

移位量 `#N` = `3 + 3*(numDims-1-i)`（cell 8B, D power-of-2）。纯移位，无乘法。

## 12. Non-goals / future

- **Version invalidation**: if per-instance constants ever become mutable at
  runtime, add a `version` field per cell + hit-path compare (load version,
  compare to `version_[dimType][instanceId]`, miss on mismatch). Out of scope
  under the §2.1 contract.
- **Per-period-array exact bounds**: a per-dim bound = its period array's
  element count (zero memory waste) was rejected for v1 - it forces per-dim
  bound/stride loads (non-power-of-2 strides -> multiplies) on the hit path,
  growing hit overhead. Revisit if memory pressure dominates.
- **3/4-dim memory pressure**: at high D, 3-4-dim functions dominate the BSS
  budget. If this bites, lower D for high-dim functions or cap maxDims at 2.
- **Polymorphic multi-slot within one identity** (e.g. adaptive): out of scope;
  direct-indexed one-cell-per-identity is monomorphic per identity by design.
