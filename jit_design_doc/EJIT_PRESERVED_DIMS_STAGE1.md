# Preserved dimensions: version-reuse stage 1

## Status and scope

Implementation baseline: `5cc911ef9edfca71fb3f5a2dd8e769b397d6a84b`, branch
`codex/ejit-spec5-code-reuse`, PR230. The PR base remains
`3f0e190dd54752f9c1dcb4bcd1fcb5d6349ae59c` (`ejit_dev_spec5`).
The normative design is `EJIT_VERSION_CODE_REUSE_SPEC.md`; this note does not
relax its completion checklist or replace it with the spec4 implementation.

This change supplies the first compiler stage: preserved-dimension, load-only
specialization. It does NOT implement version groups, common profile sessions,
physical code deduplication, shared publication, or their board diagnostics.
The complete LLVM C++ build and the new EJIT gtests have NOT been run in the
delivery environment. Do not treat this as an accepted/deployable version-reuse
feature or remove Draft from PR230.

`EJIT_EXPERIMENTAL_PRESERVED_DIMS` defaults to **OFF** and is defined privately
on LLVMEJIT. It selects a fixed optimizer policy, not a public runtime sharing
mode. The explicit `EJitOptimizer(registry, bool)` overload lets tests compare
both policies without rebuilding LLVM. The one-argument constructor uses the
library's compiled default. There is no live policy-switch API.

With the option OFF, whole-parameter specialization and the legacy address
patterns remain selected. Reinitializing a StructFieldPass now clears stale
metadata maps in either mode. No byte-identical library/footprint claim is made:
the added cold compiler code still needs the real SRE lipo/dependency audit.

Do not enable this experiment in deployed SRE before its focused validation.
The eventual version-sharing API must independently enforce Async + normal
online PGO, reject unsupported combinations before admission side effects, and
supply all remaining lifecycle/session guarantees. This compiler-stage flag is
not an implementation of that API or its combination rejection.

## What changes in the compiler

1. `preReplacePeriodIndices` does not replace arguments in preserved mode.
   Real cell/TRP arguments, uses in ordinary loads, store addresses, and call
   actuals stay in IR. The logical cache identity and lifecycle version are not
   changed. Only an authorized scalar load result is replaced.
2. `EJitStructFieldPass::setPreservedDimensions` copies identity values, not IR
   pointers. Each replace round rebuilds the root/formal/GV maps from the
   current module. The last replace after unrolling receives the original
   context in preserved mode, including borrowed views. No round is omitted.
3. The existing Gen/Use common prefix and existing optimizers are reused.
   No duplicate PGO pipelines, extra inliner, vector tier, or new annotation is
   added. After directly invoking a preserved-mode StructFieldPass, the driver
   clears cached analysis results before the next optimization step.
4. Only the selected, unreferenced entry seeds real dimension assumptions.
   A local non-address-taken helper may receive an assumed integer only when
   EVERY incoming direct call proves the same value and belongs to the closed
   region rooted at that entry. Unknown actuals, conflicting callers, outside
   callers, other entry boundaries, and address-taken helpers refuse the proof.
5. The PR223 integer evaluator is reused, including its poison-flag checks.
   Existing `free_dim` zero witnesses remain load-only: they are merged into
   the load environment, never used to prove a relationship across a call.
6. A failed preserved proof never enters legacy pointer-base or absolute-address
   replacement. In particular, an unmarked pointer-slot load is never frozen
   merely because a marked field is accessed through the pointer it returned.

### Read authorization and bounds

The load must be non-atomic, non-volatile, and authorized by the existing
per-load marker or existing field metadata fallback. Supported values are
integers up to 64 bits, float, and double. Pointer-valued and vector loads are
left dynamic in this phase.

For a registered array, the lookup is by the exact global symbol name, and the
period name, declared array count, and registry count must match. The entire
read must lie within the currently tracked period element, not just somewhere
inside the enclosing array. Reading `cell + 1` while only `cell` is a lifecycle
dependency is refused. Static period objects currently require instance zero.
The original product contract that a registration actually denotes an object
of its declared type/extent still applies; the registry has no separate byte
extent for arbitrary pointer-form pointees.

For a bounded borrowed argument, the metadata size must fit the descriptor,
the period instance must match a context dependency, and duplicate descriptors
or root contracts are refused. The original pointer argument is not replaced.
An explicit mismatched descriptor instance is not silently rewritten to the
current cell. The legacy unspecified-instance sentinel can be populated from
the context. This change introduces no group-level delayed borrow or new borrow
completion/cancellation event; those remain required future runtime work.

The checked GEP walk supports integral address-space-zero 64-bit pointers and
64-bit indices. It rejects negative steps, unsized/scalable types, vector GEPs,
unknown roots, inttoptr addresses, and overflowing products/sums. Rejecting
negative steps also prevents an invalid intermediate inbounds GEP from being
hidden by a later offset in the opposite direction. Bounds rejection happens
before any memory read.

`EJitPreservedScalar.h` decodes bytes in the target DataLayout's byte order.
APInt uses the actual integer width, and APFloat is constructed from bits, not
from a host float/double conversion. Negative zero and NaN payloads are retained.
The helper itself uses no libc, logging, allocation, or atomic operations.

### Cold-path inference limits

Dimension count uses the existing `EJIT_ICACHE_MAX_DIMS` cap (default four).
The new inference region is limited to 256 module functions, 4096 call sites,
1024 defined-function arguments, and 16384 incoming-edge evaluations. Pointer
walk depth and GEP index count are each capped at 16, and the existing integer
evaluator keeps its depth cap. Exceeding a proof budget retains loads; it never
re-enables global parameter RAUW. These are compiler proof limits, NOT completed
budgets for future groups, requests, bundles, waiters, or code pools.

The conservative root-use, negative-index, pointer-form, and helper-boundary
restrictions can lose optimization. They are not claims that these valid source
patterns are unsupported by ordinary EJIT execution: the load remains dynamic.

## Tests supplied

`EJitPreservedDimsTest.cpp` is included in the existing `EJITTests` target.
It exercises load/store/call operand preservation, 6 cells x 20 entry names,
one differing scalar, missing dependencies, other-cell reads, atomic/volatile
loads, unknown/poison indices, pointer-base refusal, all-edge helper inference,
address escape/outside callers, map rebuild after deleted IR, the last round
after unrolling, explicit legacy behavior, bounded borrowed descriptors,
big-endian FP/integer bits, address-space/overflow rejection, inference-budget
exhaustion, and the separation of free_dim load witnesses from call proofs.

The ELF-host native test `EJitPreservedDimsNative.RealGenUseAndDynamicStores` uses real
LLJIT, Gen instrumentation, 64 actual calls, the actual counter globals and
`synthesizeProfileBuffer`, Use, and execution of the emitted final function.
It checks dynamic stores across 6 cells and 2 TRP values whose gains agree.
There is no mock compiler, fabricated profile, or precomputed final function.
This test skips non-ELF hosts. It is supplied but NOT compiled/executed in this
delivery environment.
Even when it passes, it will validate the compiler pipeline, not runtime group
admission, session isolation, shared publication, or real SRE execution.

The 6 x 20 test is an IR specialization test, NOT an end-to-end proof that 120
logical cache identities map to 20 physical functions. No such mapping exists
in this change.

## Reusing an existing build

Do not start an unrelated full LLVM build. With an already configured host
build, first keep the default policy OFF and build the existing target:

```sh
cmake -S llvm -B "$EXISTING_HOST_BUILD" \
  -DEJIT_EXPERIMENTAL_PRESERVED_DIMS=OFF
cmake --build "$EXISTING_HOST_BUILD" --target EJITTests
"$EXISTING_HOST_BUILD/unittests/ExecutionEngine/EJIT/EJITTests" \
  --gtest_filter='PreservedDimsTest.*:EJitPreservedDimsNative.*'
```

Also run the complete existing EJITTests regression suite with the policy OFF.
The focused tests explicitly select both policies; ON-build coverage and any
existing tests that assume whole-parameter replacement must be reviewed before
claiming a tested compiled-default-ON configuration. Neither configuration's
full C++ suite has been run here.

For a reused AArch64 big-endian SRE build, retain its existing toolchain,
freestanding options, cache geometry, and lipo command. Incremental LLVMEJIT
builds, the final lipo output's unresolved-symbol diff, and real board execution
are required. In particular check new `memchr`/`wmemchr`, atomic helpers, and log
or libc dependencies in the ACTUAL final EJIT object, not just the small helper.

The dependency-free production helper can be checked separately:

```sh
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -I llvm/include \
  llvm/unittests/ExecutionEngine/EJIT/Inputs/PreservedScalarProbe.cpp \
  -o /tmp/preserved-scalar
/tmp/preserved-scalar
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I llvm/include \
  llvm/unittests/ExecutionEngine/EJIT/Inputs/PreservedScalarProbe.cpp \
  -o /tmp/preserved-scalar-sanitized
/tmp/preserved-scalar-sanitized
clang++ --target=aarch64_be-none-elf -std=c++17 -O2 -ffreestanding \
  -fno-exceptions -fno-rtti -mno-outline-atomics -DEJIT_PROBE_NO_MAIN \
  -I llvm/include -c \
  llvm/unittests/ExecutionEngine/EJIT/Inputs/PreservedScalarProbe.cpp \
  -o /tmp/preserved-scalar-aarch64-be.o
readelf -h /tmp/preserved-scalar-aarch64-be.o
nm -u /tmp/preserved-scalar-aarch64-be.o
```

## Original web-delivery evidence

- Six complete changed baseline files were fetched at the specified commit and
  their Git blob SHA-1 values matched the repository before editing.
- The standalone production byte-reader probe was built with Clang 17 and
  executed on x86-64 at O2 and with ASan/UBSan; both exited zero.
- Clang 17 emitted a freestanding ELF64 big-endian AArch64 relocatable probe
  object. It contains dynamic-input reader/bounds functions (not only a folded
  constant-return test), and `nm -u` was empty. The object was NOT executed.
- Twenty-two representative IR fixtures passed the installed LLVM 19 parser
  and verifier. This checks IR inputs only, not the new C++ pass or gtests.
- Patch application/reversal and UTF-8/LF/no-BOM/diff checks are recorded in the
  accompanying delivery bundle. No source commit or PR update was pushed.

There was no reusable LLVM build or development-header tree in the container,
and direct git/curl access to GitHub failed. The connector provided source reads
but no executable repository-write action. At that delivery point there was no
host real-JIT pass result, full AArch64 EJIT artifact, SRE link result, or board
acceptance result.

## Coordinator continuation validation (2026-09-10)

The original patch was applied to its exact `5cc911ef9edf` Git baseline in an
isolated server worktree. Two compile blockers were corrected: the scalar
reader's namespace is fully qualified and native tests pass `Triple`, not a
string, to `Module::setTargetTriple`.

The native fixture now matches the production ORC ordering: add the original
module, specialize/instrument in the IR transform, claim generated counters via
`defineMaterializing`, then lookup and execute. Instrumenting before adding the
module had caused an ORC Weak-flag mismatch for `__llvm_profile_raw_version`.
The fix changes only the test setup, not production symbol handling. Analysis
caches are cleared while their source module remains alive.

Focused host results, with current EJIT source objects and compatible existing
generic LLVM/gtest libraries, LLVM assertions enabled:

- Default experiment OFF: 15/15 pass. Tests explicitly choose the replacement
  policy; this is not a complete default-OFF product regression.
- `EJIT_EXPERIMENTAL_PRESERVED_DIMS`, `EJIT_SRE_PGO_BRANCH_AUDIT`,
  `EJIT_DIAG_ENABLE`, and `EJIT_SRE_PGO_VALUE_PROFILE` enabled together: 15/15 pass.
- The native case executes 64 T1 calls, synthesizes their profile, compiles T2
  and verifies the consumed entry count is 64. The T2 pointer is then called
  with all six cell and two TRP values, checking dynamic loads, stores and
  return values. The native fixture has no indirect-call/scalar-loop VP sites;
  enabling VP here checks build compatibility, not collector-session behavior.
- All 14 IR/bounds cases pass, including 6 x 20 substitutions and target-BE bit
  decoding. This still does not prove physical code reuse for those identities.

No full exact-preset CMake build, full AArch64 BE LLVMEJIT artifact, SRE link or
board execution has been performed in this continuation. The host platform
implementation is used for the focused host build; it is not a replacement for
the SRE permission/allocator contracts. The actual shared-mode runtime remains
unfinished as listed below.

## Explicitly unfinished version-reuse work

- Common-prefix candidate grouping with PGO schema/binding identity and exact
  comparison under forced hash collisions; final full emission-module and
  effective-binding comparison before CodeGen/ORC claims, including splitting.
- One representative per group, 64-entry window boundaries, non-representative
  AOT waiting, public profile-driven T2 without private T1, session isolation
  for edge/IC/memop/scalar data, and immutable complete ProfileBundle snapshots.
- Per-attempt request tokens; independent admission/borrow/publication
  completion; cancel/recreate/old-callback safety; acknowledged delayed-borrow
  read completion; bounded timeout, resource exhaustion, retry and publish-fail
  state machines; unsupported-combination and live-switch rejection.
- Physical T2 reuse and skipped CodeGen/JITLink/allocation; logical cache and
  invalidation aliases; unified near-T2/far-T1 ranges; no-reclaim and existing
  publication triggers; shared physical-range diagnostics and deduplicated
  accounting; compatible hits/cycles window reporting.
- Product validation of the existing first-use, refill, and migration paths
  for the SAME identity on multiple cores. The wrapper direct-hit route is
  distinct from taskpool-local preparation; actual table mapping, per-core
  executable preparation and migration coverage still need evidence. No new
  all-core barrier or hot-path permission check is added here.
- Header-free, single-C worker6/producer16 board demo, sustained sampling of all
  identities, independent read-only printing, and full board acceptance.

Do not infer any of these from the scalar probe, IR verification, or the new
native test's name. They remain required before PR230 can become ready.
