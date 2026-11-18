# EJIT function-body and wrapper cycle timing

> Historical design only. These compiler flags and recorder APIs were removed in #268. Rebuild old instrumented objects; the online PGO audit is separate.

This diagnostic mode compares executed AOT and JIT bodies and reports the
enclosing wrapper cost from the same invocation. The body interval excludes
taskpool lookup, inline-cache probing, dispatch, read-token release, and timing
aggregation. The wrapper interval includes everything from the generated
wrapper's entry through read-token release, but still excludes aggregation.

## Build

Pass the hidden AOT option when compiling translation units that contain
`ejit_entry` functions:

```text
-mllvm -ejit-function-body-timing
```

The option is off by default. With it off, no timestamp or recorder calls are
emitted and the inline-cache hit path keeps its frame-less `musttail` shape.

## Read results

After the workload has reached the window to compare, print the accumulated
samples on each participating core:

```c
ejit_function_body_cycles_print();
```

Example output on SRE:

```text
function_body_cycles: unit=cycles slots=128 dropped=0
  func=process_cell path=AOT count=1000 body_avg=842 body_min=790 body_max=1201 body_total=842000 wrapper_avg=1010 wrapper_min=940 wrapper_max=1410 wrapper_total=1010000 overhead_avg=168 overhead_min=130 overhead_max=240 overhead_total=168000
  func=process_cell path=JIT count=1000 body_avg=516 body_min=488 body_max=901 body_total=516000 wrapper_avg=590 wrapper_min=554 wrapper_max=980 wrapper_total=590000 overhead_avg=74 overhead_min=61 overhead_max=110 overhead_total=74000
```

Use `ejit_function_body_cycles_reset()` immediately before a measurement window
to exclude warm-up. `ejit_function_body_cycles_get()` provides the same
body `count/total/min/max` plus `wrapper_*` and `overhead_*` data without
parsing logs. Each average is its corresponding total divided by `count`.
`overhead` is calculated per invocation as the two intervals outside the body,
so it can be compared directly without subtracting unrelated sample windows.

With the inline cache enabled, the wrapper timestamp is captured before the
probe. On a miss it is passed into the internal slow-path function, so both AOT
fallback and slow-path JIT samples include the failed probe and full lookup.

SRE samples use `SRE_CycleCountGet64()`. Host builds use steady-clock
nanoseconds for structural tests and must not be compared numerically with SRE
cycles. The fixed-capacity table does not allocate. `dropped` reports samples
lost to table capacity or a nested/concurrent recorder; the hot recorder uses a
single try-lock and never waits in the measured workload's return path.

The intervals necessarily contain cycle-counter read costs. Wrapper timing also
disables the frame-less `musttail` form while the diagnostic option is enabled.
Use identical instrumentation for AOT and JIT, compare averages over many
samples, and treat very small functions near the measurement floor with
caution. Production builds with the option off retain the original wrapper.

Only normal returns are sampled. An AOT exit consisting of a `musttail` call and
its required adjacent return is left untouched and is not reported, because
inserting a recorder there would invalidate the IR or change tail-call
behaviour. With the inline cache enabled, any function containing such an exit
keeps an ABI-identical internal miss function and disables AOT fallback samples
for that function. Its slow-path JIT sample starts the wrapper interval inside
the miss function; inline-cache JIT hits retain the full pre-probe interval.
