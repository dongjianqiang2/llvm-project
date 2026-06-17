## XBBR (TASK M2-T03 / SPEC §10 M2 exit condition): linking with
## --bb-cross-reorder= must produce a runnable static x86_64 executable
## whose section layout is functionally equivalent to CGProfile-only
## (M2 doesn't yet do per-BB reordering).
##
## Specifically:
##   * The output ELF must NOT contain .llvm_xbbr_attr (SHF_EXCLUDE
##     must be honored — these are XBBR's compile-time-only metadata,
##     never present in the loadable image; PLAN §9.3).
##   * SHT_LLVM_BB_ADDR_MAP IS retained (lld preserves it for perf
##     and downstream tools — that's existing behavior, not XBBR).
##   * The sections list with --bb-cross-reorder= must equal the
##     sections list without it (functional equivalence in M2).

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c a.c -o a.o

## With XBBR enabled.
# RUN: ld.lld -e _start a.o --bb-cross-reorder=foo.profdata \
# RUN:     -o out.xbbr
## Without XBBR (control).
# RUN: ld.lld -e _start a.o -o out.plain

## .llvm_xbbr_attr must NOT appear in either output (SHF_EXCLUDE).
# RUN: llvm-readelf -SW out.xbbr  | FileCheck %s --check-prefix=NOXBBRATTR
# RUN: llvm-readelf -SW out.plain | FileCheck %s --check-prefix=NOXBBRATTR

## Both outputs must keep .llvm_bb_addr_map (existing lld behavior).
# RUN: llvm-readelf -SW out.xbbr  | FileCheck %s --check-prefix=BBMAP
# RUN: llvm-readelf -SW out.plain | FileCheck %s --check-prefix=BBMAP

## Functional equivalence in M2: same section list / sizes regardless
## of whether --bb-cross-reorder= was passed.
# RUN: llvm-readelf -SW out.xbbr  | sed -n '/Section Headers/,/^Key to Flags/p' > out.xbbr.sections
# RUN: llvm-readelf -SW out.plain | sed -n '/Section Headers/,/^Key to Flags/p' > out.plain.sections
# RUN: cmp out.xbbr.sections out.plain.sections

# NOXBBRATTR-NOT: LLVM_XBBR_ATTR
# NOXBBRATTR-NOT: .llvm_xbbr_attr

# BBMAP: LLVM_BB_ADDR_MAP

#--- a.c
int foo(int n) { return n < 0 ? -n : n + 1; }
int _start(void) { return foo(42); }
