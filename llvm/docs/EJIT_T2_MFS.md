# EJIT Tier-2 Machine Function Splitting

`EJIT_T2_MFS` is a default-OFF product option that enables profile-guided
machine-function splitting for Tier-2 EJIT code. It is intentionally narrower
than LLVM's general MFS policy: only Instrumentation PGO or Context Sensitive
Instrumentation PGO data may authorize a split, and only a basic block with a
known count of exactly zero may move to the cold range. Missing, mismatched,
synthetic, or nonzero profile data leaves the block in the hot range.

## Build modes

Feature ON requires the fixed near-hot code pool and batched publication:

```sh
cmake -S llvm -B build \
  -DEJIT_SRE_CODE_POOL=ON \
  -DEJIT_CODE_POOL_4K_SEAL=ON \
  -DEJIT_FIXED_CODE_POOL=ON \
  -DEJIT_SRE_SHARED_TASKPOOL=ON \
  -DEJIT_CODE_POOL_FIXED_NEAR_HOT=ON \
  -DEJIT_CODE_POOL_BATCHED_PUBLISH=ON \
  -DEJIT_T2_MFS=ON
cmake --build build --target LLVMCodeGen LLVMEJIT
```

The control build keeps the same prerequisite options and changes only MFS:

```sh
cmake -S llvm -B build \
  -DEJIT_SRE_CODE_POOL=ON \
  -DEJIT_CODE_POOL_4K_SEAL=ON \
  -DEJIT_FIXED_CODE_POOL=ON \
  -DEJIT_SRE_SHARED_TASKPOOL=ON \
  -DEJIT_CODE_POOL_FIXED_NEAR_HOT=ON \
  -DEJIT_CODE_POOL_BATCHED_PUBLISH=ON \
  -DEJIT_T2_MFS=OFF
cmake --build build --target LLVMCodeGen LLVMEJIT
```

Changing only `EJIT_T2_MFS` recompiles these feature-specific objects:

- `MachineFunctionSplitter.cpp` and `TargetPassConfig.cpp` in `LLVMCodeGen`;
- `EJitOptimizer.cpp`, `EJitOrcEngine.cpp`, and `EJitSrePlatform.cpp` in
  `LLVMEJIT`.

Applying this change also rebuilds the ABI and placement plumbing in
`EJit.cpp`, `EJitCodePool.cpp`, `EJitCodePoolMemoryManager.cpp`,
`EJitCompileDriver.cpp`, `EJitRuntime.cpp`, and `EJitSharedTaskPool.cpp` once.

## Linker reservation

The product linker must include `llvm/lib/ExecutionEngine/EJIT/ejit_registry.ld`
and choose a nonzero cold reservation in a multiple of 2 MiB. For example:

```sh
-Wl,-T,llvm/lib/ExecutionEngine/EJIT/ejit_registry.ld \
-Wl,--defsym=__ejit_cold_bytes=0x800000
```

The script creates an executable `.text.ejit_cold` output section bounded by
`__ejit_cold_start` and `__ejit_cold_end`. It follows the existing 36 MiB
near-hot reservation and does not reduce the near or Tier-1 far pool capacity.
The runtime rejects a missing, empty, misaligned, overlapping, or insufficient
reservation. A freestanding feature-ON link must resolve both cold-bound
symbols.

Each Tier-2 version owns one bounded cold companion range. The owner seals and
cache-synchronizes its hot and cold ranges before publishing either. A peer
core prepares both ranges independently and marks the version ready only after
both succeed. Publication remains retryable after a permission failure.

The shared cache ABI for this standalone change is 23: main ABI 20 plus MFS.
ABI 21 and 22 belong to the separate PR212 line and are not included here. A
future combination must allocate another ABI number. The diagnostic cold pool
ID is 18.

## Board acceptance

`ejit_test/ejit_mfs_zero_count_sre_multicore_test.c` is the header-free board
acceptance program. Product startup owns init-array execution; the repeatable
test command never runs constructors. Run it first on worker core 6 and then on
producer core 16 after a reboot. Per-core registration and EJIT setup happen at
most once. The producer continuously exercises all 16 cell identities
until return-address classification proves actual Tier-2 execution. Only then
does it execute the previously zero-count path and validate cold placement and
results for 128 rounds. A completed producer invocation only revalidates the
published versions, concurrent producer commands are rejected, and a failed or
timed-out run remains terminal until a coordinated reset. The print command is
read-only. Build the same file with `MFS_EXPECT_SPLIT=0` for the feature-OFF
control.

`ejit_test/ejit_mfs_zero_count_command_sequence_test.c` is a small host
lifecycle harness for repeated worker, producer, and print commands plus wrong
core, concurrent re-entry, compile failure, and timeout handling. Its
`EJIT_MFS_HOST_TEST` attribute bypass and mocked runtime do not validate real
PGO, MFS placement, or the final product compiler.

Host and public-toolchain validation cannot close these hardware-specific
items; the final board run must verify them explicitly:

- real page-table splitting, permission changes, and required TLBI behavior;
- D-cache clean, I-cache invalidation, barriers, and cross-core visibility;
- worker-6/producer-16 execution with the production shared mappings;
- ABI 23 agreement across every independently built image and component;
- final SRE platform symbols plus the production libc++, libunwind,
  compiler-rt, and linker-script dependencies.
