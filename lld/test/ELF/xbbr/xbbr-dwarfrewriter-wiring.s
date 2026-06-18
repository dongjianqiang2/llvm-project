## XBBR rewriteDWARF wiring: the stub must be called from XBBRPipeline.
## Earlier review found the stub had zero callers (dead code). This
## test pins that --bb-cross-reorder-stats produces the wired diagnostic
## so a regression to no-callsite status fails immediately.
##
## When the real DWARF rewrite pass replaces the stub, update this test
## to assert the post-emit DWARF transform instead.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -ffunction-sections -c a.c -o a.o

# RUN: ld.lld -e foo a.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all -o exe 2> stderr.txt
# RUN: FileCheck %s < stderr.txt

# CHECK: DWARF rewrite stub invoked

#--- a.c
extern int helper(int);
int foo(int n) {
  if (n < 0) return -1;
  return helper(n);
}
