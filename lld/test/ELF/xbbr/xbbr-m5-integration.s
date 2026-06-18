## XBBR (TASK M5): M5 milestone integration test.
##
## Exercises full mode + partial mode differentiation, physical
## emission preparation, and end-to-end pipeline with all M5 options.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=full \
# RUN:     -ffunction-sections -c hot.c -o hot.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=full \
# RUN:     -ffunction-sections -c indir.c -o indir.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=full \
# RUN:     -ffunction-sections -c main.c -o main.o

## Full mode link.
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata \
# RUN:     --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-cluster-algo=hfsort+ \
# RUN:     --bb-cross-reorder-layout-algo=ext-tsp \
# RUN:     --bb-cross-reorder-weights=icache=4,itlb=2,btb=1,size=2 \
# RUN:     --bb-cross-reorder-fallback=auto \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all \
# RUN:     -o exe.full 2> exe.full.stderr

## Partial mode link for comparison.
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata \
# RUN:     --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-fallback=auto \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all \
# RUN:     -o exe.partial 2> exe.partial.stderr

## Full mode stats show mode=full.
# RUN: FileCheck %s --check-prefix=FULLSTATS < exe.full.stderr
## Partial mode stats show mode=partial.
# RUN: FileCheck %s --check-prefix=PARTSTATS < exe.partial.stderr

## Both produce valid ELF.
# RUN: llvm-readelf -h exe.full | FileCheck %s --check-prefix=ELFOK
# RUN: llvm-readelf -h exe.partial | FileCheck %s --check-prefix=ELFOK

## Reproducibility: full mode.
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.full1
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.full2
# RUN: cmp exe.full1 exe.full2

# FULLSTATS: xbbr-stats:
# FULLSTATS: xbbr-m5: {{.*}}mode=full

# PARTSTATS: xbbr-stats:
# PARTSTATS: xbbr-m5: {{.*}}mode=partial

# ELFOK: Class:{{.*}}ELF64

#--- hot.c
extern int helper(int);
int hot_fn(int n) {
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
extern int hot_fn(int);
extern int via_fnptr(int);
int _start(void) {
  return hot_fn(7) + via_fnptr(11);
}
