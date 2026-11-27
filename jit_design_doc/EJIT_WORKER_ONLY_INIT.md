# EJIT worker-only initialization (spec5)

Enable `EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT=ON` on both the compiler and runtime.
The option defaults to OFF, including in the AArch64 BE preset. It requires
fixed-worker election, shared code pointers, and the SRE code pool. The AOT
fixups default to the configured shared inline-cache section. Use the same
image and build settings on all cores, with the fixups, inline-cache cells, and
runtime shared state in coherent shared memory at matching virtual addresses.
The fixup section must retain its initialized data; do not link it as NOLOAD.

- The designated worker runs the image's initialization array and calls
  `ejit_init` or `ejit_init_pgo`. Generated AOT EJIT registration constructors
  default to OFF in this build; registration walks `.ejit_bitcode` and
  `.ejit_group` once on the worker. Do not override the AOT ctor or fixup
  defaults in the deployment.
- Other cores run neither the initialization array nor `ejit_init`.
  Before the worker publishes Ready they take AOT without enqueuing, waiting,
  or accepting an activation that would be lost. Their first Ready lookup
  constructs only a per-core taskpool facade, with execute/RW page hooks wired
  before the facade is published. The engine and compiler remain worker-only.
- Wrapper fixups use atomic loads, and the worker publishes them with release
  stores. Inline-cache probes use the guarded null-cell form in this mode.
  The shared registration table lets a lifecycle toggle drain all cores'
  cache cells. This changes the probe shape only in the shared-fixup build.
- Lifecycle activate/deactivate/query and dispatch diagnostics work without
  producer initialization. Plain configuration periods without a lifecycle
  and registry/metadata/active-list dumps remain worker-only. Pre-Ready
  control calls return `EJIT_ERR_NOT_ACTIVE` (queries return false).
- Quiesce producer use of EJIT before worker shutdown or reinitialization.
  This version does not define live worker handover or mixed-image attachment.
  The taskpool blob ABI is unchanged.

## Board smoke

Compile `ejit_test/ejit_zero_init_multicore_test.c` with the matching Clang,
runtime, and `-mllvm -ejit-inline-cache`. Keep `EJIT_STATS_ENABLE=ON`. Verify
the link map places `__ejit_funcidx_*` / `__ejit_dimtype_*` in the initialized
shared section and that the business object has no EJIT auto-register
constructor. Then, on a fresh boot:

1. Core 16, without an init array or EJIT init: call
   `test_ejit_zero_init_before_worker`. Expect AOT and return 0.
2. Core 6, after its normal initialization array: call
   `test_ejit_zero_init_worker`. Expect return 0.
3. Core 16, still without initialization: call
   `test_ejit_zero_init_producer`. Expect producer activation, real T1/T2
   completions and cache hits, successful deactivate, and return 0.

Host tests simulate core IDs and page-permission callbacks. They do not
validate an actual firmware's shared mappings or per-core page tables.
