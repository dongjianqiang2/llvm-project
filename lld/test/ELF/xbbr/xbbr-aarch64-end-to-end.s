## XBBR AArch64 end-to-end (SPEC §10): minimum AArch64 acceptance —
## clang→ld.lld→dump full pipeline on AArch64 inputs without crashing
## and producing a valid .debug_xbbr_decision section.
##
## Note: aarch64 host environment runs the binary directly; we don't,
## but the link itself proves the format is consistent.

# REQUIRES: aarch64

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -ffunction-sections -c hot.c -o hot.o
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -ffunction-sections -c main.c -o main.o

## Partial-mode link.
# RUN: ld.lld -e _start hot.o main.o \
# RUN:     --bb-cross-reorder=foo \
# RUN:     --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all -o exe.aa 2> stderr.partial

## Full-mode link.
# RUN: ld.lld -e _start hot.o main.o \
# RUN:     --bb-cross-reorder=foo \
# RUN:     --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all -o exe.aa.full 2> stderr.full

## Pipeline stats line is present (stable string, decoupled from milestone).
# RUN: FileCheck %s --check-prefix=STATS < stderr.partial
# RUN: FileCheck %s --check-prefix=STATS < stderr.full

## Decision map section exists and is non-allocatable.
# RUN: llvm-readelf -SW exe.aa | FileCheck %s --check-prefix=SECTION
# RUN: llvm-readelf -SW exe.aa.full | FileCheck %s --check-prefix=SECTION

## Dump tool can parse the AArch64-linked binary.
# RUN: llvm-bbreorder-dump --summary exe.aa | FileCheck %s --check-prefix=SUM
# RUN: llvm-bbreorder-dump --summary exe.aa.full | FileCheck %s --check-prefix=SUMF

## Stripping the debug section removes the decision map (SPEC §9.4).
## (Use llvm-objcopy since llvm-strip may not be in the build).
# RUN: llvm-objcopy --remove-section=.debug_xbbr_decision exe.aa exe.aa.stripped
# RUN: not llvm-bbreorder-dump --summary exe.aa.stripped 2>&1 | FileCheck %s --check-prefix=ERR

## Reproducibility: same inputs → identical output.
# RUN: ld.lld -e _start hot.o main.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.aa.r1
# RUN: ld.lld -e _start hot.o main.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.aa.r2
# RUN: cmp exe.aa.r1 exe.aa.r2

# STATS: xbbr-pipeline:

# SECTION: .debug_xbbr_decision
# SECTION-NOT: .debug_xbbr_decision {{.*}} A {{.*}}

# SUM:  xbbr-dump: entries={{[1-9]}}
# SUM:  xbbr-dump: functions={{[1-9]}}
# SUMF: xbbr-dump: entries={{[1-9]}}

# ERR:  error: no valid .debug_xbbr_decision section found

#--- hot.c
extern int helper(int);
int hot(int n) {
  if (__builtin_expect(n < 0, 0))
    return -1;
  return helper(n);
}

#--- main.c
extern int hot(int);
int _start(void) {
  return hot(7);
}
