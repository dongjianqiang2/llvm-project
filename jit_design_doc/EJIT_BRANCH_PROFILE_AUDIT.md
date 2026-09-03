# EJIT Branch Profile Audit

## Purpose

`EJIT_SRE_PGO_BRANCH_AUDIT` is an experimental, default-off diagnostic for
finding EJIT annotations whose extracted modules show little branch-profile
optimization opportunity. It reuses the normal online-PGO Tier-1 edge counters
and adds a build-option-gated per-may_const-site counter array to that same
temporary tier.

The build-script switch enables the existing EJIT diagnostics as well. If the
CMake option is enabled directly while diagnostics are compiled out, the audit
does not arm the temporary tier, performs no IR scan, and has no runtime cost.

The audit runs after `PGOInstrumentationUse` restores branch weights and before
ICP or module inlining changes the original function boundaries. It reports:

- root execution count, defined functions, and IR instruction count in each
  entry module;
- conditional branch sites and sites carrying valid profile weights;
- strongly biased sites, where one successor has at least 95% of the weight;
- balanced sites, where no successor has more than 60% of the weight;
- zero-count edges.

Independently of applying PGO optimizations, it instruments only loads
recognized by `EJitStructFieldPass` as `ejit_may_const`, using the same per-load
metadata and global field-offset fallback as the real replacement pass. One
monotonic atomic counter is inserted at each original load site before EJIT
constant specialization. The load may then disappear, but the temporary T0
code still counts every dynamic execution of that source site.

The profile-consumption build carries an identical zeroed counter shape through
the shared light-optimization prefix so Tier-1 and Tier-2 keep matching CFG
hashes. Those temporary increments and their global are removed immediately
after `PGOInstrumentationUse`; they never reach stable Baseline/PGOUse code.

The 64-entry window counts only calls that actually dispatch an executable
Tier-1 pointer. A peer that cannot execute the shared pointer, a null pointer,
execute-permission preparation failure, or any other AOT fallback does not
consume the quota. The 64th real dispatch still executes Tier-1 and atomically
freezes the timestamp and entry count; later calls use AOT while Tier-2 waits or
compiles. The compile driver therefore snapshots the site counters without
including queue, worker-throttle, or Tier-2 compile delay in `sample_cycles`.
`ejit_init()` publishes ordinary Baseline code; `ejit_init_pgo()` continues to
normal PGOUse code. The INFO line reports the current version plus the
unique-cache-key aggregate for that entry:

```text
[EJIT] mayconst-audit entry=FuncA key=0x1234 tier=2 versions=30 \
  mayconst=42/3/0 removed=42 direct=39 pipeline=3 runtime_hits=99872 \
  hit_sites=17 removed_runtime_hits=95000 removed_hit_sites=16 \
  sampled_entries=64 sample_cycles=1900000 avg_removed=40 \
  weighted_permille=952 min=36 max=42
```

`mayconst=input/specialized/final` counts only may-const loads. Negative removal is
preserved rather than clamped, because it identifies specializations where
optimization introduced more may-const loads than it removed. Recompiling the
same cache key replaces its prior sample and does not inflate the version
average.

The INFO line is one summary per profiled EJIT entry module. DEBUG adds one line
per defined function. These numbers are evidence for ranking candidates; they
do not by themselves prove that an annotation causes a performance regression.
A final A/B run with the candidate annotation disabled is still required. At
DEBUG, every sampled specialization also lists each input candidate as a
`mayconst-site` with its runtime `hits`, function, root global, field offset,
and source line/column. Unknown roots or offsets are reported honestly. A hit
count is a snapshot of completed atomic increments when the replacement compile
starts; concurrently executing T0 calls may finish after that snapshot. The
summary remains a net module-level load count, which keeps it meaningful across
function inlining and cloning.

## Build and run

```sh
./build.sh release aarch64_be \
  --sre-taskpool --sre-shared-taskpool \
  --sre-taskpool-no-reclaim --sre-shared-code-pointers \
  --sre-pgo-branch-audit \
  --sre-pgo-max-concurrent-profiles=4 --stats
```

Start the worker as usual, call every workload path whose annotations must be
audited, and wait for the final publication. Use `ejit_init()` for audit-only
Baseline output or `ejit_init_pgo()` to apply PGO as well. Set the EJIT
diagnostic level to DEBUG when per-function and per-may_const-site lines are
needed. Audit sampling requires Async mode; Sync and Off keep their normal
Baseline/off behavior because there is no later hit-triggered publication.

After the sampling windows and final compilations have completed, explicitly
print the per-`ejit_entry` ranking from any attached core:

```c
ejit_print_mayconst_ranking();
```

The call is read-only and does not start profiling or compilation. In a shared
taskpool build, a non-owner core posts a diagnostic request and waits while the
owner worker prints its local optimizer data. Each Tier-1 may-const load carries
a stable audit site ID. A site contributes to `removed_runtime_hits` only when
all copies of that ID are absent after final optimization; unknown provenance is
matched conservatively by field/source identity, and a site with no recoverable
identity is kept. Rows are ordered by removed dynamic load executions per
million platform timestamp units:

```text
[EJIT] mayconst-ranking entries=2 sort=benefit_per_mcycle_desc
[EJIT] rank=1 entry=FuncA versions=6 benefit_per_mcycle=71225.850 \
  entry_benefit_density=1187.097 density_valid=1 hot_code_bytes=23040 \
  size_source=fn size_valid_versions=6/6 hot_icache_lines=360 \
  fingerprinted_lines=360 cross_version_matching_lines=90 \
  partial_jit_candidate_lines=75 partial_jit_candidate_ratio=20.8% \
  removed_per_entry=20882.518 removed_runtime_hits=8018887 \
  sampled_entries=384 sample_cycles=112583000 avg_active_sites=17.000 \
  hit_sites=102 runtime_hits=9000000 avg_removed=40 total_removed=240 \
  min=36 max=42
```

`sample_cycles` uses `SRE_CycleCountGet64()` ticks on SRE and steady-clock
nanoseconds on hosts. All ratios use wide integer intermediates, round to the
nearest fixed-point value, and saturate to `uint64_t`; diagnostics use no
floating point. The primary score is
`removed_runtime_hits * 1,000,000 / sample_cycles`. The equivalent decomposition
is `(removed_runtime_hits / sampled_entries) *
(sampled_entries / sample_cycles)`: work removed per entry multiplied by entry
frequency. With the default threshold, six completed versions therefore report
`sampled_entries=384`. Queueing and compilation after the threshold cannot
change either that count or the frozen interval.

`hot_code_bytes` and `hot_icache_lines` use the finalized entry symbol's exact
`fnPtr + fnSize`, not the containing JITLink allocation. The byte range is read
only after publication and only when it lies completely inside the finalized
executable allocation; a zero/missing/out-of-range size is reported as
`size_source=unknown` or `mixed`, with `density_valid=0`. In that case density
and all aggregate line-fingerprint metrics are omitted rather than estimated
from a partial version set or allocation bytes.
`entry_benefit_density` divides `benefit_per_mcycle` by the sum of per-version
`ceil(fnSize / 64)` I-cache lines.

`avg_active_sites` is `hit_sites / versions` and remains an explanatory metric;
three decimal places are printed using integer fixed-point arithmetic.
`runtime_hits` is the sum of dynamic T0 executions of recognized may-const
load sites; `hit_sites` is the sum of sites that executed at least once in each
sampled version. Entries tied on `benefit_per_mcycle` are ordered by
`removed_per_entry`, `avg_active_sites`, `runtime_hits`, `avg_removed`, then
name. An entry appears only after at
least one specialization has completed its audit window and final compilation.
The aggregation state stays local to the compile-owner worker; only a
request/completion sequence and a boolean result cross shared memory.

## Coverage boundary

Tier-1 instruments every defined function in the extracted module, so callees
are included even when they are not separate `ejit_entry` roots. The scheduler
eventually profiles every *executed* EJIT entry specialization in batches; the
concurrency limit only controls how many are active at once. A never-called
entry or specialization has no dynamic profile and cannot appear in this audit.

The same source annotation may create multiple cache keys for different period
dimensions. Each executed key is audited separately because its branch behavior
can differ.
