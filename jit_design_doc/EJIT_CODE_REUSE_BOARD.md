# PR230 Board Smoke Test (On PR233)

Source: `examples/ejit_reuse_sre_test.c`, a single header-free C file.
Replace the previous file defining `test_ejit_period`; do not link both demos.
The companion `ejit_reuse_host_check.c` is a host-only mock, NOT a board source.

## Build And Startup

- Use the EJIT Clang and runtime from PR230 rebased on PR233
  (`812f6474b706e0bb8f0cd1b93e9b0413bd5457cf`). Stock Clang is rejected.
- Use the existing spec5 AOT annotation/bitcode/static-registration build flow.
  Keep generated wrappers and registration constructors in the linked image.
- Runtime: async shared taskpool, fixed worker core 6, shared code pointers,
  normal online PGO, `EJIT_STATS_ENABLE`, and diagnostic/dump support enabled.
  Keep the representative quota at its current 64-dispatch setting.
- Keep `EJIT_SRE_PGO_BRANCH_AUDIT` enabled. Online PGO plus audit diagnostics
  is supported; it is not audit-only mode. Rebuild the runtime with the PR230
  audit-admission fix if initialization rejects this valid combination with
  `representative sharing requires Async + normal online PGO`. No example or
  public config layout change is needed for this fix.
- This example opts in through `ejit_init_representative`, not `ejit_init_pgo`.
  It does not change the product's default-off policy. Do not initialize EJIT
  elsewhere before running this example.
- Use the same 64-bit image/ABI on cores 6 and 16. Map `.mc_shared` coherently at
  the same VA on both cores. All configuration rows and test result state in
  that section must be shared, not per-core copies. Adjust `REUSE_SHARED` only
  to the product's equivalent shared section.
- Both participating cores run `call_init_array_functions()` once before their
  local EJIT init. Define `REUSE_RUN_INIT_ARRAY=0` only when platform startup
  already ran constructors on BOTH cores. Repeated shell calls and the print
  command never replay constructors or reinitialize EJIT.
- Run after board reset in an otherwise idle test image, from schedulable shell
  tasks. There must be no other callers or configuration writers. Provision
  cache/code/data capacity for 120 live logical entries, 21 representative T1s,
  21 physical T2 objects and their retained profiles. Old T1 allocation need
  not be reclaimed. This is larger than the previous six-version IR sample.

## Shell Sequence

```text
core[6]-> test_ejit_period
          [REUSE230] WORKER_READY
core[6]-> core 16
core[16]-> test_ejit_period
           [REUSE230] PHASE_INITIAL checked ...
           [REUSE230] PHASE_UPDATE checked ...
           [REUSE230] OWNER_DONE ...
core[16]-> core 6
core[6]-> test_ejit_reuse_print
          [REUSE230] PASS ...
```

All four optional shell arguments are ignored. There is no Baseline/PGO mode
argument: this is always representative online PGO. A timeout or failure needs
a reset before another run; printing partial state is safe after failure.

## What Is Checked

Twenty entries (`reuse_0` through `reuse_19`) each use six cells, with TRP 1 as
a second live dimension. Each entry has its own period array. Only `gain` is
may_const; ordinary loads/stores still use the actual cell and TRP arguments.
Every executed call checks the return value and all cells' live fields, so an
accidentally frozen write address cannot pass just because the gain is equal.

1. Initially five cells of `reuse_0` have gain 3; cell 5 has gain 17. All six
   cells match within each other entry. Expect 21 candidate groups, 21 real
   representative sessions, 21 x 64 = 1344 T1 dispatches, 21 physical T2 objects,
   and 120 logical T2 entries. The unequal cell must NOT share the equal group's
   pointer, and its final pointer must differ from its observed T1 pointer.
2. The owner continuously polls ALL 120 identities, executing every successful
   grant immediately and releasing its read token afterwards. It never waits
   for a batch of four profiles before sampling the representatives already
   running. Admission deferral is retried; actual queue/compile/publish failures
   are rejected separately. The same generated wrappers are exercised after
   convergence. This diagnostic C-API polling is not a hot-path benchmark.
3. The owner stops business calls, deactivates cell 5 and waits for the exact
   compiler-source borrow fence. Only `EJIT_OK` permits changing gain 17 to 3.
   A pending/error/timeout never authorizes mutation. The owner reactivates the
   cell and continuously drives all identities again. All 20 renewed cell-5
   entries must reuse their original equal-group T2 pointers; other cells must
   retain those pointers. No new representative samples or physical T2 objects
   are expected. The retained old unequal T2 is still counted, so physical
   objects remain 21, not 20; shared reuse normally totals 119 after renewal.
4. Final acceptance runs on core 6, where the compiler's group registry lives.
   It checks the real physical-code and sampling counters plus drained work.
   `OWNER_DONE` on core 16 alone is NOT acceptance. The printed cache should
   show `tier2=120`, no collecting T1, and all 120 final entries
   `post_publish_seen=yes`. No re-election, schema rejection, independent
   fallback object, cancelled waiter, compile or publish failure is expected.

The dump captures only `reuse_0` to limit output. Inspect the final T2 entry and
module: the gain load should disappear, but the live load/store and real
cell/TRP indexing must remain. The recorded module is the latest capture, not
an archive of all 120 versions; use the cache lines and saved before/after
pointer matrices to compare the whole run.

## Verification Boundary

`ejit_reuse_host_check.c` checks arithmetic, startup order, admission retry,
read-token balance, all 120 identities, mutation fences, failure paths, and
read-only printing with a mock. It also builds against the real public header
to check the header-free declarations. It is not an AOT/JIT test.

The real runtime regression
`RoundRobinTwentyEntriesRejoinSharedTier2AfterBorrowFence` independently runs
the same continuous scheduling and renewal pattern through ORC, online PGO,
the production shared cache and actual generated host code. The native host
tests do not certify AArch64 BE code generation, SRE mappings/cache maintenance,
or board execution. Those require the three board commands above and their
resulting logs. This example does not cover `bound_ptr`, PR231 or every product
may_const site.
