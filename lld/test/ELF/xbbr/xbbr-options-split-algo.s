## XBBR cluster/layout option split: with --bb-cross-reorder=, the
## function clustering algorithm is selected by
## --bb-cross-reorder-cluster-algo=, independent of the BB-layout
## algorithm chosen by --bb-cross-reorder-layout-algo=. Verify both
## values are accepted and produce consistent output.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux-gnu a.s -o a.o

# RUN: ld.lld -e A a.o \
# RUN:   --bb-cross-reorder=foo \
# RUN:   --bb-cross-reorder-cluster-algo=hfsort+ \
# RUN:   --bb-cross-reorder-layout-algo=ext-tsp \
# RUN:   -o out
# RUN: llvm-nm --numeric-sort out | FileCheck %s

# RUN: ld.lld -e A a.o \
# RUN:   --bb-cross-reorder=foo \
# RUN:   --bb-cross-reorder-cluster-algo=c3 \
# RUN:   --bb-cross-reorder-layout-algo=ph \
# RUN:   -o out2
# RUN: llvm-nm --numeric-sort out2 | FileCheck %s

# CHECK: T A

#--- a.s
.globl A
A:
  retq
