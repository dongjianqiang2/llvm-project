## XBBR (review-fix #1 regression): the M1 emitter writes 16-bit attr
## words (PLAN §9.3 v0x02), and lld's Stage 0 must read them as 16-bit
## too — earlier the XBBRNode field was uint8_t, silently truncating
## bit 8 (IsNoReturnTail) to zero. This test pins that the bit makes
## it across the boundary.
##
## Two BBs in this object are noreturn-tails (SPEC §5.3 item 7):
##   * the `bad` block in dies() — explicit `abort_with()` then unreachable
##   * the entry of _start — its tail call to dies() is also a noreturn
##     callsite followed by unreachable, satisfying the same predicate.
## Stage 0 must report `noreturntail=2`. Before the fix, both showed 0.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -ffunction-sections -c a.c -o a.o
# RUN: ld.lld -e _start a.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all \
# RUN:     -o /dev/null 2> stderr.txt
# RUN: FileCheck %s < stderr.txt

# CHECK: xbbr-stats:
# CHECK-SAME: noreturntail=2

#--- a.c
__attribute__((noreturn)) extern void abort_with(int);

int dies(int n) {
  if (n < 0)
    return n + 1;
  abort_with(n);
  __builtin_unreachable();
}

int _start(void) { return dies(7); }
