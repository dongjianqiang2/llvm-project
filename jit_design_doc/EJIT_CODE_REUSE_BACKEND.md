# Prepared-Code Backend Milestone

This is an experimental compiler-side foundation for PR230, not the completed
sharing feature. It follows the repaired preserved-dimension stage1 at
`aaf2c016191f985bc78689aff141096346ad0bc5`, based on spec5 and the specification
at `5cc911ef9edfca71fb3f5a2dd8e769b397d6a84b`.

## What Runs Now

`EJitPreparedCode` takes ownership of an already optimized final module. It
checks eligibility, verifies the IR, finalizes local entry visibility and
builds a canonical comparison clone. The real emission module is not
normalized and has no mutable accessor. An owner-side audit can use the same
identity without emitting code.

`EJitPreparedCodeEmitter` performs exact matching before creating ORC claims.
On a miss it creates a physical JITDylib and adds the original final module
directly to `IRCompileLayer`. On a hit it returns the existing physical code
identity/address, with no duplicate CodeGen, object linking or allocation.
This boundary must not run IRTransform or another optimization pipeline after
the identity decision.

There are no changes to `ejit_init`, taskpool dispatch, PGO admission, wrapper
generation, shared state layout, code-pool selection or sealing policy. No
product runtime path instantiates the emitter yet. The stage1 experiment flag
still controls only the preserved-dimension replacement strategy; it does not
enable production code sharing.

## Exact Identity

The key retains the entry, source-bitcode digest, exact compiler-policy string,
binding generation, complete sorted absolute binding list and full final IR.
Cell/TRP values and logical lifecycle versions are not added to this key.
Real constants, dynamic addresses, callee definitions, constant initializers,
global names, attributes, target triple/DataLayout and semantic metadata stay
in the comparison. Only comparison-clone debug information, module paths and
local SSA/block/argument display names are removed.

SHA-256 provides a candidate filter, never proof. The bounded physical
directory compares all identity material even when bucket hashes collide.
It intentionally prefers false-negative sharing opportunities to unsafe
normalization. Private function/global names and type names are not renamed.

The first version rejects independent mutable definitions, TLS, inline asm,
aliases/ifuncs, blockaddress and address-observable private definitions.
Private constant globals require global `unnamed_addr`. Ordinary dynamic
accesses to registered external data remain valid when their exact bindings
match. Referenced external declarations need explicit, nonzero, type-compatible
function/data bindings. A binding cannot shadow a module definition.

Backend-generated libcalls also belong in the supplied binding environment.
The physical JITDylib is bare: it has no implicit process/platform link order.
A missing backend symbol therefore fails linking instead of silently resolving
outside the compared environment. Symbols synthesized by a later backend must
be covered by the compiler policy and the complete binding list.

## Ownership, Limits And Failures

The emitter is serialized by one compile owner. Its nonzero owner epoch must be
unique within its LLJIT; physical IDs are monotonic within that epoch. Physical
JITDylibs are not keyed or destroyed by a logical cell's cache key. Successful
code stays owned by LLJIT until shutdown, including after the directory object
is destroyed. LLJIT must outlive all callers of the code. No logical reference
count, publication waiter or reclaim policy is implemented here.

Default experimental limits are 128 physical objects, 16 MiB total retained
identity material, 1 MiB canonical IR per module, 1 MiB defined constant data
per module, and 262144 function/block/instruction/global nodes. These are not
yet product CMake settings. They limit this component, not complete LLVM peak
memory, profile bundles, requests, retries or code-pool reservations. The
remaining PR230 resource budgets must be enforced by their own owners.

Creation/emission failures are recoverable LLVM errors. Unsupported sharing
uses `operation_not_supported`; exhausted identity/data/code budgets use
`no_buffer_space`; invalid input/bindings use `invalid_argument`. The future
adapter must not handle budget exhaustion by starting unlimited independent
compilations. Link failure removes the unpublished failed JITDylib and does not
retain a reusable identity or consume a successful-code budget slot. Existing
code remains usable; the same identity can retry. SRE NO_RECLAIM allocation
stranding remains governed by the existing memory manager, not this directory.

**Linked does not mean published or executable on a producer.** The returned
address may be RW/NX in SRE's near pool. Runtime integration must obtain the
real executable ranges, run the existing codeReady/flush and per-core
preparation protocol, recheck logical identity/generation, and only then fill
the relevant Ready/icache state. This component must never fill wrapper slots.

## Native Evidence

`EJitPreparedCodeTest.cpp` uses actual optimizer passes, native InstrProf
counters, CodeGen, ORC linking and generated-function execution. The main
fixture manually selects one representative per entry and freezes its real
64-call profile; it is not a taskpool/profile-session simulation.

For 20 entries and six candidate cells each:

- 20 real representative T1 objects, each executed 64 times;
- 120 independent final IR optimization runs with distinct logical cache keys;
- 20 emitted T2 objects and 100 exact reuse hits;
- the shared functions execute with both TRP values on each of six cells,
  preserving each cell/TRP's dynamic reads, stores and real parameter values;
- one changed cell gain generates a distinct T2; restoring it reuses the old
  physical object without another CodeGen.

Separate tests cover forced bucket collisions, constants/metadata/helpers,
external-binding differences, forbidden private state, target mismatch,
capacity exhaustion, a tiny IR hiding a huge constant array, unresolved
backend symbols, failed-object retry and 300 repeated reuse hits.

Host tests are not SRE execution/publication evidence. The optional sanitizer
run instruments the new prepared-code implementation and its tests; reused
LLVM/pass libraries and generated JIT code are not sanitizer-instrumented.
Function-type sanitizer is disabled for generated code without UBSan function
metadata; vptr checks are disabled for the existing no-RTTI LLVM build.

## Remaining Before Product Enablement

Follow the PR230 specification: common-prefix/schema exact matching, attempt
tokens and separate completion events, representative session isolation for
all collector kinds, complete immutable ProfileBundle, bounded waiter/borrow
lifetimes, final logical-to-physical publication/invalidations, unified
diagnostics, and all supported-mode gates. The backend primitive does not
replace any of those state machines and does not complete specification stage2
or stage3 by itself.

The current native fixture's profile has no IC/memop/scalar-loop VP sites.
Feature-enabled compilation is not proof of collector session isolation.
Target AArch64 big-endian SRE archive/ELF dependency checks, real peer permission
tests and the header-free worker6/producer16 acceptance demo remain required.
