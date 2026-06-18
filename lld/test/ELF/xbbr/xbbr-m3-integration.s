## XBBR (TASK M3-T06): M3 milestone integration test.
##
## End-to-end: clang -fbb-cross-reorder=partial → .o with metadata →
## ld.lld --bb-cross-reorder= runs Stages 0-4 pipeline → runnable ELF.
## Verifies: stats, section presence/absence, reproducibility.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -ffunction-sections -c hot.c -o hot.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -ffunction-sections -c indir.c -o indir.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -ffunction-sections -c main.c -o main.o

## Full M3 link.
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata \
# RUN:     --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-cluster-algo=hfsort+ \
# RUN:     --bb-cross-reorder-layout-algo=ext-tsp \
# RUN:     --bb-cross-reorder-weights=icache=4,itlb=2,btb=1,size=2 \
# RUN:     --bb-cross-reorder-fallback=auto \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all \
# RUN:     -o exe.xbbr 2> %t.stderr

## Stage 0 stats always present.
# RUN: FileCheck %s --check-prefix=STATS < %t.stderr

## Output sections.
# RUN: llvm-readelf -SW exe.xbbr | FileCheck %s --check-prefix=SECTIONS
# RUN: llvm-readelf -SW exe.xbbr | FileCheck %s --check-prefix=NEG

## Reproducibility: same inputs => identical ELF.
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.run1
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.run2
# RUN: cmp exe.run1 exe.run2

# STATS: xbbr-stats:
# STATS-SAME: nodes=
# STATS-SAME: edges=
# STATS-SAME: funcs=

# SECTIONS-DAG: .text
# SECTIONS-DAG: .llvm_bb_addr_map
# SECTIONS-DAG: .debug_xbbr_decision

# NEG-NOT: LLVM_XBBR_ATTR

#--- hot.c
extern int helper(int);
int hot(int n) {
  if (__builtin_expect(n < 0, 0))
    return -1;
  return helper(n);
}

#--- indir.c
typedef int (*fnptr_t)(int);
extern fnptr_t pickee(void);
int via_fnptr(int n) {
  fnptr_t fp = pickee();
  return fp(n);
}

#--- main.c
extern int hot(int);
extern int via_fnptr(int);
int _start(void) {
  return hot(7) + via_fnptr(11);
}
