## XBBR (TASK M2-T04): test that the new --bb-cross-reorder family of
## options is parsed without error and that the basic combinations work.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux-gnu a.s -o a.o

## --bb-cross-reorder=none should be a no-op (legal, accepted).
# RUN: ld.lld -e A a.o --bb-cross-reorder=none -o out.none
# RUN: llvm-readelf -h out.none | FileCheck %s --check-prefix=OK

## --bb-cross-reorder=<path> with no extra flags enables Stage 0 and
## auto-promotes the call-graph-profile sort to hfsort+ (M2 default).
# RUN: ld.lld -e A a.o --bb-cross-reorder=foo.profdata -o out.bcr
# RUN: llvm-readelf -h out.bcr | FileCheck %s --check-prefix=OK

## All sub-options should parse without error.
# RUN: ld.lld -e A a.o \
# RUN:   --bb-cross-reorder=foo.profdata \
# RUN:   --bb-cross-reorder-mode=partial \
# RUN:   --bb-cross-reorder-cluster-algo=hfsort+ \
# RUN:   --bb-cross-reorder-layout-algo=ext-tsp \
# RUN:   --bb-cross-reorder-weights=icache=4,itlb=2,btb=1,size=2 \
# RUN:   --bb-cross-reorder-max-thunk-bytes=4096 \
# RUN:   --bb-cross-reorder-fallback=auto \
# RUN:   --bb-cross-reorder-emit-decision-map \
# RUN:   --bb-cross-reorder-deterministic \
# RUN:   --bb-cross-reorder-max-align=32 \
# RUN:   -o out.full
# RUN: llvm-readelf -h out.full | FileCheck %s --check-prefix=OK

# OK: Class:{{.*}}ELF64

#--- a.s
.globl A
A:
  retq
