# XBBR acceptance scaffolding (SPEC §9 / P4-2)

This directory holds the reproducibility gate and pointers to the CI workflow.
The full §9.2 quantitative acceptance (SPEC CPU2017, llvm-test-suite, embedded
and server demos, PMU measurement) depends on external resources that cannot
be exercised in-tree; their status is tracked below.

## Reproducibility gate — `xbbr-repro-check.sh`

SPEC §9.3 requires identical `(source + profile + flags)` to produce
bitwise-identical binaries. The gate compiles once, links twice under
`--bb-cross-reorder`, and compares SHA256:

```sh
xbbr-repro-check.sh <src.ll> <triple> <partial|full> [entry] [ld flags...]
# e.g.
xbbr-repro-check.sh smoke.ll aarch64-linux-gnu full a -pie
```

Verified locally across AArch64 `{partial, full, full-PIE}`, ARM A32 `full`,
Thumb-2 `full`, and x86_64 `full`. Run by `.github/workflows/xbbr-ci.yml` on
every push/PR.

## CI — `.github/workflows/xbbr-ci.yml`

Builds `clang + lld` (X86/AArch64/ARM, Release+assertions), runs the
`lld/test/ELF/xbbr/` lit suite, then the reproducibility gate across the
supported `(arch × mode × output-type)` triples.

## Still-blocked §9.2 work (external resources — not in this tree)

| Item | Blocker | Owner notes |
|---|---|---|
| SPEC CPU2017 + MicroBenchmarks | external `llvm-test-suite` repo + SPEC license | wire as a separate opt-in workflow once a runner with the suite is available |
| Embedded demo (Zephyr / micropython) | external repos + a bare-metal ARM/AArch64 runner | demo script to be added when the target SDK is present |
| Server demo (clang self-host / MySQL) | large external builds + a perf-capable runner | PMU (L1i/iTLB/branch-miss) collection needs `perf` and the matching `--call-graph` |
| PMU measurement (L1i↓≥15%, iTLB↓≥20%, branch-miss↓≥10%) | `perf` + a representative workload | the §9.2 thresholds are evaluated against an IRPGO-only baseline on the SAME workload |

When those land, add the corresponding scripts here and reference them from
`xbbr-ci.yml` as opt-in jobs so the in-tree gate stays fast.
