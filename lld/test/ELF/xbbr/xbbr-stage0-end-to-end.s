## XBBR (TASK M2-T01-C1, M2-T04 end-to-end): clang produces .o files
## with the M1 metadata, and ld.lld --bb-cross-reorder= reads them
## through Stage 0 (XBBRGraph::build). The --bb-cross-reorder-stats
## diagnostic confirms that the graph was built and contains the
## expected number of XBBR-enabled functions.
##
## We don't statically link a libc here — the function symbols aren't
## defined; we use --unresolved-symbols=ignore-all to keep ld.lld from
## bailing while still exercising the section-collection / Stage 0
## pipeline on the .o we feed it.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c a.c -o a.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c b.c -o b.o

## Sanity: the .o files carry the XBBR sections we expect.
# RUN: llvm-readelf -SW a.o | FileCheck %s --check-prefix=AOBJ
# RUN: llvm-readelf -SW b.o | FileCheck %s --check-prefix=BOBJ

## Stage 0 reports a graph populated from both .o's.
# RUN: ld.lld -e foo a.o b.o \
# RUN:     --bb-cross-reorder=foo.profdata \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all \
# RUN:     -o /dev/null 2>&1 | FileCheck %s --check-prefix=STATS

## With --bb-cross-reorder=none the stats line must NOT appear.
# RUN: ld.lld -e foo a.o b.o \
# RUN:     --bb-cross-reorder=none \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all \
# RUN:     -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NOSTATS --allow-empty

# AOBJ-DAG: .llvm_bb_addr_map{{.*}}LLVM_BB_ADDR_MAP
# AOBJ-DAG: .llvm_xbbr_attr{{.*}}LLVM_XBBR_ATTR
# BOBJ-DAG: .llvm_bb_addr_map{{.*}}LLVM_BB_ADDR_MAP
# BOBJ-DAG: .llvm_xbbr_attr{{.*}}LLVM_XBBR_ATTR

# Stage 0 must visit both functions. Each function's entry block is an
# anchor, so anchors >= funcs >= 2. Nodes >= sum of MBBs across both
# functions; with -O2 each is at least 2 MBBs.
# STATS: xbbr-stats:
# STATS-SAME: funcs=2
# STATS-SAME: anchors=

# NOSTATS-NOT: xbbr-stats

#--- a.c
extern int callee(int);
int foo(int n) {
  if (n < 0)
    return callee(-n);
  return n + 1;
}

#--- b.c
extern int foo(int);
int bar(int x) {
  if (x > 100)
    return foo(x / 2);
  return x;
}
