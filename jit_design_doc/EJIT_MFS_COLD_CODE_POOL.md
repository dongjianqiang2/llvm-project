# EJIT Tier-2 MFS Cold Code Pool

## Purpose

`EJIT_MFS_COLD_CODE_POOL` enables LLVM MachineFunctionSplitter for profiled
JIT code. Final online-PGO Tier-2 machine functions keep hot blocks in the
fixed `.text.ejit` pool and place MFS-generated `.text.split.<function>` blocks
in a second fixed RX region, `.text.ejit_cold`.

Tier-1 instrumented code remains in the dynamic far pool. Baseline and Tier-1
functions without profile data are not split by MFS.

## Build

The feature requires all three options:

```text
EJIT_SRE_CODE_POOL=ON
EJIT_FIXED_CODE_POOL=ON
EJIT_MFS_COLD_CODE_POOL=ON
```

The `ejit-minimal-aarch64_be` preset enables the feature. The trimmed backend
restores only MachineFunctionSplitter; the other AutoFDO and generic basic
block section machinery remains trimmed.

## Linker-script contract

The final product link must define both fixed regions. Keep both within direct
AArch64 branch reach of AOT `.text` and each other.

```ld
.text.ejit : ALIGN(4096) {
  __ejit_code_start = .;
  . += 16M;
  __ejit_code_end = .;
} > CODE

.text.ejit_cold : ALIGN(4096) {
  __ejit_cold_code_start = .;
  . += 8M;
  __ejit_cold_code_end = .;
} > CODE
```

The runtime aligns each region start up to 2 MiB. Budget at least one complete
2 MiB pool after that alignment. In a freestanding feature build the cold
symbols are strong: omitting them is an intentional link error.

## Runtime layout and cross-core safety

JITLink normally merges all RX sections into one allocation segment. EJIT
intercepts `.text.split.*` blocks before `BasicLayout`, assigns them working
memory and target addresses from the cold manager, and leaves the remaining
graph to the normal hot/far layout.

A split Tier-2 publication carries two real executable extents: the hot range
containing the entry and one cold companion range. A peer core splits and seals
both pools before receiving the JIT pointer. Shared taskpool ABI v17 carries
this companion range.

## Diagnostics

`ejit_print_code_pool_stats()` reports:

```text
code pool total: ...
code pool near-hot(final): ...
code pool near-cold(mfs): ...
code pool far(tier1): ...
```

`ejit_get_code_pool_stats_v2()` remains ABI compatible: `near` is the sum of
hot and cold fixed pools. `ejit_get_code_pool_stats_v3()` exposes
`nearHot`, `nearCold`, and `farTier1` separately.

`ejit_taskpool_print_compiled()` reports `mfs_cold=yes/no`; VERBOSE output also
includes `cold_ranges` and `cold_size`.

## Board validation

1. Rebuild the full runtime and business image because shared ABI changed to
   v17 and the linker script gained strong symbols.
2. Run the existing multi-core `test_ejit_period` flow until Tier-2 publishes.
3. Confirm `near-cold(mfs)` has non-zero `used` and at least one finalized
   range for a function with a sufficiently cold branch.
4. Trigger both hot and cold paths from a non-owner core. Neither path may
   produce an execute-permission abort.
5. Compare `L1I miss`, hot-pool used bytes, end-to-end cycles, and the number
   of cross hot/cold branches against the unsplit build.
