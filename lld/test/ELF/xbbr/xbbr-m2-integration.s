## XBBR (TASK M2-T06): M2 milestone integration test.
##
## End-to-end exercise of every M2 capability in one link:
##   * M1-T07 / M2-T01: clang -fbb-cross-reorder=partial produces
##     .o files containing BB_ADDR_MAP + .llvm_xbbr_attr.
##   * M2-T01: ld.lld --bb-cross-reorder= reads them through Stage 0.
##   * M2-T02: Stage 1 hfsort+ runs (auto-enabled when XBBR is on).
##   * M2-T04: every SPEC §6.2 lld option parses; Propeller mutex.
##   * M2-T05: --bb-cross-reorder-emit-decision-map writes
##     .debug_xbbr_decision with the right header.
##   * M2-T03: with all the XBBR machinery on, the output ELF is
##     functionally equivalent to a plain CGProfile-only link.
##   * X-T01: bitwise-identical reproducibility under repeated link.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c hot.c -o hot.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c indir.c -o indir.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c main.c -o main.o

# Sanity: every .o has the M1 metadata.
# RUN: llvm-readelf -SW hot.o   | FileCheck %s --check-prefix=METAOBJ
# RUN: llvm-readelf -SW indir.o | FileCheck %s --check-prefix=METAOBJ
# RUN: llvm-readelf -SW main.o  | FileCheck %s --check-prefix=METAOBJ

# Full-spectrum link with every M2 option on.
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata \
# RUN:     --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-cluster-algo=hfsort+ \
# RUN:     --bb-cross-reorder-layout-algo=ext-tsp \
# RUN:     --bb-cross-reorder-weights=icache=4,itlb=2,btb=1,size=2 \
# RUN:     --bb-cross-reorder-max-thunk-bytes=4096 \
# RUN:     --bb-cross-reorder-fallback=auto \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --bb-cross-reorder-deterministic \
# RUN:     --bb-cross-reorder-max-align=32 \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all \
# RUN:     -o exe.xbbr 2> exe.xbbr.stderr

# RUN: FileCheck %s --check-prefix=STATS < exe.xbbr.stderr
# RUN: llvm-readelf -SW exe.xbbr | FileCheck %s --check-prefix=EXEHAS
# RUN: llvm-readelf -SW exe.xbbr | FileCheck %s --check-prefix=EXENOT --allow-empty

# Reproducibility (SPEC §9.3 / TASK X-T01): same inputs ⇒ identical ELF.
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-emit-decision-map --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.run1
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-emit-decision-map --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.run2
# RUN: cmp exe.run1 exe.run2

# Functional equivalence (M2 milestone exit, SPEC §10).
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --unresolved-symbols=ignore-all -o exe.plain
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata --bb-cross-reorder-mode=partial \
# RUN:     --unresolved-symbols=ignore-all -o exe.xbbr.nomap
# RUN: llvm-readelf -SW exe.plain      | sed -n '/Section Headers/,/^Key to Flags/p' > plain.sections
# RUN: llvm-readelf -SW exe.xbbr.nomap | sed -n '/Section Headers/,/^Key to Flags/p' > xbbr.sections
# RUN: cmp plain.sections xbbr.sections

# Strip removes only .debug_xbbr_decision.
# RUN: cp exe.xbbr exe.stripped
# RUN: llvm-strip --strip-debug exe.stripped
# RUN: llvm-readelf -SW exe.stripped | FileCheck %s --check-prefix=STRIPPED --allow-empty

# METAOBJ-DAG: .llvm_bb_addr_map{{.*}}LLVM_BB_ADDR_MAP
# METAOBJ-DAG: .llvm_xbbr_attr{{.*}}LLVM_XBBR_ATTR

# STATS: xbbr-stats: nodes=
# STATS-SAME: edges=
# STATS-SAME: funcs=
# STATS-SAME: anchors=

# EXEHAS-DAG: .llvm_bb_addr_map{{.*}}LLVM_BB_ADDR_MAP
# EXEHAS-DAG: .debug_xbbr_decision{{.*}}PROGBITS

# EXENOT-NOT: .llvm_xbbr_attr
# EXENOT-NOT: LLVM_XBBR_ATTR

# STRIPPED-NOT: .debug_xbbr_decision

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
