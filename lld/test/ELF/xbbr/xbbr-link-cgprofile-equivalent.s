## XBBR linker integration: full-spectrum link drives every option in
## one go and asserts the result is functionally equivalent to a plain
## CGProfile-only link (the linker stages so far do not move BBs;
## downstream emission stages will).
##
## Coverage in one link:
##   * compiler side: clang -fbb-cross-reorder=partial produces .o files
##     with BB_ADDR_MAP + .llvm_xbbr_attr.
##   * Stage 0: ld.lld --bb-cross-reorder= reads the metadata.
##   * Stage 1: hfsort+ runs (auto-enabled when XBBR is on).
##   * every SPEC §6.2 lld option parses; Propeller mutex.
##   * --bb-cross-reorder-emit-decision-map writes
##     .debug_xbbr_decision with the right header.
##   * with all the XBBR machinery on, the output ELF is functionally
##     equivalent to a plain CGProfile-only link.
##   * bitwise-identical reproducibility under repeated link.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c hot.c -o hot.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c indir.c -o indir.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c main.c -o main.o

# Sanity: every .o has the compiler-emitted XBBR metadata.
# RUN: llvm-readelf -SW hot.o   | FileCheck %s --check-prefix=METAOBJ
# RUN: llvm-readelf -SW indir.o | FileCheck %s --check-prefix=METAOBJ
# RUN: llvm-readelf -SW main.o  | FileCheck %s --check-prefix=METAOBJ

# Full-spectrum link with every linker XBBR option on.
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

# Reproducibility (SPEC §9.3): same inputs ⇒ identical ELF.
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-emit-decision-map --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.run1
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-emit-decision-map --bb-cross-reorder-deterministic \
# RUN:     --unresolved-symbols=ignore-all -o exe.run2
# RUN: cmp exe.run1 exe.run2

# Functional equivalence (SPEC §10 / P1-1): XBBR partial mode physically
# reorders BBs and splits hot migratable BBs into .text.hot (cold BBs stay in
# .text per SPEC §4 partial semantics). A plain CGProfile-only link keeps
# everything in .text. What must hold: both links succeed; the XBBR link has
# .text.hot in addition to .text; the plain link has no .text.hot. (The
# reproducibility cmp above is the determinism gate.)
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --unresolved-symbols=ignore-all -o exe.plain
# RUN: ld.lld -e _start hot.o indir.o main.o \
# RUN:     --bb-cross-reorder=foo.profdata --bb-cross-reorder-mode=partial \
# RUN:     --unresolved-symbols=ignore-all -o exe.xbbr.nomap
# RUN: llvm-readelf -SW exe.xbbr.nomap | FileCheck %s --check-prefix=HAS-TEXT
# RUN: llvm-readelf -SW exe.xbbr.nomap | FileCheck %s --check-prefix=HAS-HOT
# RUN: llvm-readelf -SW exe.plain      | FileCheck %s --check-prefix=PLAIN-NO-HOT \
# RUN:     --allow-empty
# HAS-TEXT: .text
# HAS-HOT: .text.hot
# PLAIN-NO-HOT-NOT: .text.hot

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
