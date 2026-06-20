# REQUIRES: aarch64

# Phase 0c: under -fbasic-block-sections=all (implied by -fbb-cross-reorder=partial,
# Phase 0a), each BB is its own InputSection and one BBAddrMap section (with N
# ranges) describes a whole function. Stage 0 must group per-BB sections by
# function: ONE FuncId per function (not one per section), one node per BB.
#
# Two functions (f1, f2), each with a conditional branch => 3 BBs each after
# -O2 lowering => 6 nodes total, 4 intra-function CFG edges (2 per function),
# 2 anchors (the two entry blocks).

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=partial -c src.c -o src.o
# RUN: ld.lld -e f1 src.o --bb-cross-reorder=foo --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe 2>&1 | FileCheck %s

# CHECK: xbbr-stats: nodes=6 edges=4 funcs=2 anchors=2

#--- src.c
extern int sink_a(int), sink_b(int);
int f1(int n) {
  if (n < 0)
    return sink_a(n);
  return sink_b(n);
}
int f2(int n) {
  if (n < 0)
    return sink_a(n);
  return sink_b(n);
}
