# EJIT SVE Vectorization

## Scope

`EJIT_SRE_SVE_VECTORIZATION` enables scalable-vector optimization for EJIT on
AArch64. The production `aarch64_be` preset enables the option.

| Compilation path | L1 | L2/L3 |
| --- | --- | --- |
| Baseline JIT | Disabled | Enabled |
| PGO Tier-1 instrumentation | Disabled | Disabled |
| PGO Tier-2 | Disabled | Enabled |

Tier-1 deliberately keeps the original scalar CFG. This avoids changing the
instrumentation profile layout and keeps profiling overhead bounded. Tier-2
runs vectorization after profile use and value specialization, so constant loop
bounds and hot-path information are available to the vectorizer.

## Pipeline

The SVE-enabled L2/L3 pipeline runs loop vectorization, loop-load elimination,
alignment inference, SLP vectorization, and vector combine after the existing
EJIT simplification and PGO-use passes. The JIT target machine and eligible
functions receive the `+sve` target feature, and PassBuilder uses the AArch64
target transform information for scalable-vector cost modelling.

Enabling the option permits SVE generation; it does not force every loop to be
vectorized. Dependences, aliasing, trip counts, alignment, and the target cost
model can still keep a loop scalar.

## Deployment

The generated code requires an SVE-capable AArch64 processor. Do not deploy an
SVE-enabled EJIT build on hardware without SVE support.

## Verification

Use the existing dump interfaces after the desired Baseline or Tier-2 version
has compiled:

```text
ejit_dump_func("function_name")
ejit_print_dumped("function_name")
```

The optimized IR should contain scalable vector types such as
`<vscale x 4 x i32>`. AArch64 assembly commonly contains instructions such as
`whilelo`, `ptrue`, `ld1w`, `st1w`, or `incw`, depending on the loop.

For PGO, inspect the published Tier-2 version. Tier-1 is intentionally scalar.
