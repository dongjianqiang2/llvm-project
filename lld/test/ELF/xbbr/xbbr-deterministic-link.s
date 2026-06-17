## XBBR (TASK X-T01 / SPEC §9.3): two ld.lld invocations on the same
## inputs with --bb-cross-reorder= must produce bitwise-identical ELFs.
## This is a hard CI gate — Stage 0 currently uses sorted enumeration
## (no DenseMap iteration in node ordering) so this should hold; if a
## future patch leaks DenseMap order into the output, this test fires.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c a.c -o a.o
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c b.c -o b.o

# RUN: ld.lld -e _start a.o b.o --bb-cross-reorder=foo \
# RUN:     --unresolved-symbols=ignore-all -o out1
# RUN: ld.lld -e _start a.o b.o --bb-cross-reorder=foo \
# RUN:     --unresolved-symbols=ignore-all -o out2
# RUN: cmp out1 out2

#--- a.c
extern int helper(int);
int foo(int n) { return n < 0 ? helper(-n) : n + 1; }

#--- b.c
extern int foo(int);
int _start(void) { return foo(42); }
