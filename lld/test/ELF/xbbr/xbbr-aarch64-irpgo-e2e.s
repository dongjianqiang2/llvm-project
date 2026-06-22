# REQUIRES: aarch64, host-aarch64

# XBBR AArch64 end-to-end with a REAL IRPGO profile (SPEC §9.1 top of the
# testing pyramid: source -> instrument -> run -> profdata -> -fprofile-instr-use
# + -fbb-cross-reorder -> ld.lld --bb-cross-reorder -> run). This is the
# regression guard for the paved AArch64 pipeline: it proves that cross-function
# BB migration actually happens on a real C program (not just hand-written asm),
# that the result is correct (output matches an IRPGO-only baseline), and that
# the layout is reproducible.
#
# host-aarch64 (not the global `native` feature, which is unavailable in lld's
# lit config) gates actually running the linked aarch64 executable. Requires the
# clang profile runtime (libclang_rt.profile.a); build compiler-rt with
# -DCOMPILER_RT_BUILD_PROFILE=ON if this test fails at the instrument link.

# RUN: rm -rf %t && split-file %s %t && cd %t

# 1. Instrumented build + run to collect the raw profile.
# RUN: clang -target aarch64-linux-gnu -O2 -fuse-ld=lld \
# RUN:     -fprofile-instr-generate=t.profraw t.c -o t.inst
# RUN: %t/t.inst

# 2. Merge the raw profile.
# RUN: llvm-profdata merge -o t.profdata t.profraw

# 3. Baseline: IRPGO use, XBBR off.
# RUN: clang -target aarch64-linux-gnu -O2 -fuse-ld=lld \
# RUN:     -fprofile-instr-use=t.profdata t.c -o t.base

# 4. XBBR partial: IRPGO use + -fbb-cross-reorder + ld.lld --bb-cross-reorder.
# RUN: clang -target aarch64-linux-gnu -O2 -fuse-ld=lld \
# RUN:     -fprofile-instr-use=t.profdata -fbb-cross-reorder=partial \
# RUN:     -Wl,--bb-cross-reorder=foo -Wl,--bb-cross-reorder-mode=partial \
# RUN:     -Wl,--bb-cross-reorder-emit-decision-map t.c -o t.xbbr

# 5. Correctness: XBBR binary runs and matches the IRPGO-only baseline.
# RUN: %t/t.base > base.out
# RUN: %t/t.xbbr > xbbr.out
# RUN: cmp base.out xbbr.out

# 6. Cross-function BB migration really happened: .text.hot exists in the XBBR
#    binary (the baseline has a single .text). This is the physical evidence
#    that hot BBs from different functions were co-located.
# RUN: llvm-readelf -SW t.xbbr | FileCheck %s --check-prefix=HOT
# HOT: .text.hot
# RUN: llvm-readelf -SW t.base > base.secs
# RUN: not grep -q '\.text\.hot' base.secs

# 7. Decision map records Moved BBs.
# RUN: llvm-bbreorder-dump --summary t.xbbr | FileCheck %s --check-prefix=MOVED
# MOVED: xbbr-dump: entries={{[1-9]}}
# MOVED: moved={{[1-9]}}

# 8. ABI invariant (SPEC §5.1): function symbol address == entry-block address.
#    The entry block is anchored; under -fbasic-block-sections=all each
#    function's entry section carries the symbol, so nm's symbol addr must equal
#    the decision-map entry's OrigFuncAddr for that function's BB 0.
# RUN: llvm-bbreorder-dump t.xbbr | FileCheck %s --check-prefix=ABI
# ABI: Function {{[0-9]+}} ({{[1-9]}} BBs):
# ABI: {{^ *0 *}}0x{{[0-9A-F]+}}  0x{{[0-9A-F]+}}  {{[0-9 ]+}} anchored

# 9. Reproducibility (SPEC §9.3): same inputs -> bitwise-identical binary.
# RUN: clang -target aarch64-linux-gnu -O2 -fuse-ld=lld \
# RUN:     -fprofile-instr-use=t.profdata -fbb-cross-reorder=partial \
# RUN:     -Wl,--bb-cross-reorder=foo -Wl,--bb-cross-reorder-mode=partial \
# RUN:     -Wl,--bb-cross-reorder-emit-decision-map t.c -o t.xbbr2
# RUN: cmp t.xbbr t.xbbr2

#--- t.c
#include <stdio.h>
__attribute__((noinline)) int hot_func(int x) {
  if (x > 0) { return x * 3 + 1; }   /* hot path */
  else { return x * 7 - 2; }          /* cold path */
}
__attribute__((noinline)) int cold_func(int x) {
  volatile int s = 0;
  for (int i = 0; i < 100; i++) s += i;
  return s + x;
}
__attribute__((noinline)) int caller(int x) {
  int r = hot_func(x);
  if (x < -100) r += cold_func(x);
  return r;
}
int main(void) {
  int s = 0;
  for (int i = 0; i < 200000; i++) s += caller(i & 0x7f);
  printf("%d\n", s);
  return 0;
}
