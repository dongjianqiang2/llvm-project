## XBBR mutex (SPEC §6.3): --bb-cross-reorder= must be mutually
## exclusive with --symbol-ordering-file (Propeller). Combining them
## must produce a hard error before any layout work happens.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux-gnu a.s -o a.o
# RUN: not ld.lld -e A a.o \
# RUN:   --bb-cross-reorder=foo.profdata \
# RUN:   --symbol-ordering-file=order.txt \
# RUN:   -o /dev/null 2>&1 | FileCheck %s

## --bb-cross-reorder=none (off) does NOT trigger the mutex check.
# RUN: ld.lld -e A a.o \
# RUN:   --bb-cross-reorder=none \
# RUN:   --symbol-ordering-file=order.txt \
# RUN:   -o %t.ok

# CHECK: --bb-cross-reorder= and --symbol-ordering-file may not be used together (SPEC §6.3)

#--- order.txt
A

#--- a.s
.globl A
A:
  retq
