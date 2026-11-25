# EJIT worker-only substitution verifier

This optional diagnostic checks values actually folded by the normal
specialization pass. It emits no verifier IR, call, load, or hit counter into
business or JIT code. The old instrumented verifier instead checked the load
that a business call actually executed and left the original value in use.
The worker check is not a replacement for that coverage or a correctness guard.

Configure the runtime (not the business Clang):

```text
-DEJIT_WORKER_VERIFY_SUBSTITUTION=ON -DEJIT_WORKER_VERIFY_PERIOD=<shared-period>
```

The feature defaults to OFF. It requires the shared taskpool and diagnostics.
Use the normal `ejit_init` / `ejit_init_pgo` in Async mode to get coverage.
Synchronous compilations work as before and do not collect verifier records.
Only one named period is selected per prototype package. The worker performs up
to 32 probe/retirement steps every eight idle iterations after serving compile
and publish work. This is a work budget, not a time or detection guarantee.

The selected source ranges must be coherent shared RAM with the same contents
on worker and producer. The globals, registered pointer slots and any bounded
pointees being scanned must remain mapped and resident through shutdown;
shared mapping alone does not make a freed or reused allocation readable.
Updates must not tear or race ordinary loads; deliberate diagnostic mutations
in the demo use naturally aligned atomic stores. Do not opt a period with
volatile/MMIO or moving pointees into this prototype. A generation check
discards a sample that overlaps a lifecycle change; it does not pin storage.

Coverage is limited to scalar integer/pointer folds with a registered source,
constant offsets and one fully enumerable unsigned remainder, including
`%5` and `%10`. For root pointers it compares the current shared slot against
the materialized pointer. For bounded fields behind them it double-checks the
root and does not follow a newly installed pointer. Leaf scans require a
matching explicit pointer extent; otherwise the root may be verified while the
field is uncovered. Borrowed bound-pointer arguments, PHI, and unknown index
domains are not covered. A load that never folded is not a verifier failure.

Mismatches print from the worker with function, artifact tier, generation,
dimension ids, source object, load ordinal, residue, and expected/actual bits.
There is no automatic fallback or recompile. `ejit_worker_verify_dump` on the
worker core requests a summary; success means queued, not printed or passed.
On the worker keep log level at INFO; producers need no verifier log setting.

- `checked_active=A/B`: candidate reads checked at least once out of tracked
  candidates, not execution coverage. Stale versions retire lazily.
- `mismatch_samples`: cumulative mismatching samples, not unique functions.
  Repeated mismatches at a candidate are counted but normally logged once until
  it recovers. `suppressed` counts log deferrals due to the print limit or a
  disabled worker log.
- `uncovered`, `dropped`, `blocked_root`: unsupported published sites,
  budget loss, and skipped leaf reads after a root changed, respectively.
  In particular `checks=0` is not a pass.

## Board smoke

Link only `ejit_test/ejit_worker_verify_sre_test.c` using normal spec5
annotations and build flags. Enable stats and set
`EJIT_WORKER_VERIFY_PERIOD=worker_verify_cell` for the runtime. Follow the
product's existing init-array and shared section configuration:

1. Core 6: `test_ejit_worker_verify_start`.
2. Core 16: `test_ejit_worker_verify_sample` (waits for T1 and T2).
3. Core 6: `ejit_worker_verify_dump`. Wait for checked coverage and zero
   mismatches. With just this demo, the active T2 has 15 residue candidates.
4. Core 16: `test_ejit_worker_verify_inject`. It changes only residues 4 and 9
   and does not call the optimized function.
5. Core 6: observe worker mismatch rows for `residue=4/5` and
   `residue=9/10`. A dump can also advance one scan slice. Reboot before
   repeating; never use the injection command in a production image.
