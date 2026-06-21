# REQUIRES: aarch64

# P1-3: when a migrating B.cond (CONDBR19) / TBZ (TSTBR14) would overflow
# WITHIN .text.hot, Stage 4 pins (reverts) its endpoints — lld cannot thunk
# these branches, so an overflow is a hard link error. The hidden knob
# --bb-cross-reorder-cond-range-for-testing=N shrinks the projected cond range
# so the pin path is exercised with a tiny binary (no 1 MiB filler).
#
# Same IR as xbbr-aarch64-condbr-in-range-migrates.s: {loop, bodyB} is an
# all-hot component that migrates. With the real range it stays in-range
# (pinned=0); with the knob shrunk to 8 bytes the projected loop→bodyB
# distance exceeds 8×0.9, so Stage 4 pins both endpoints (pinned=2). The link
# still succeeds (pinning prevented the overflow) and is reproducible.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir f.ll \
# RUN:     -c -o f.o 2>/dev/null

# Sanity: real range → no cond pin.
# RUN: ld.lld -e f f.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-stats --unresolved-symbols=ignore-all \
# RUN:     -o %t/real 2> %t/real.stats
# RUN: FileCheck %s --check-prefix=REAL < %t/real.stats
# REAL: xbbr-stage4: overRange=0 estThunkBytes=0 pinned=0 degraded=0

# Shrunk cond range → cond over-range → Stage 4 pins the endpoints (pinned=2).
# RUN: ld.lld -e f f.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-cond-range-for-testing=8 --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all -o %t/shrunk 2> %t/shrunk.stats
# RUN: FileCheck %s --check-prefix=PIN < %t/shrunk.stats
# PIN: xbbr-stage4: overRange=0 estThunkBytes=0 pinned=2

# Reproducibility (SPEC §9.3).
# RUN: ld.lld -e f f.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-cond-range-for-testing=8 \
# RUN:     --unresolved-symbols=ignore-all -o %t/shrunk2 2>/dev/null
# RUN: cmp %t/shrunk %t/shrunk2

#--- f.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @gA(i32)
declare void @gB(i32)
declare i32 @h(i32)
define void @f(i32 %n) noinline !prof !0 {
entry:
  %v0 = call i32 @h(i32 0)
  br label %loop
loop:
  %iv = phi i32 [0, %entry], [%iva, %bodyA], [%ivb, %bodyB]
  %c = icmp slt i32 %iv, %n
  br i1 %c, label %bodyA, label %bodyB, !prof !1
bodyA:
  %va = call i32 @h(i32 %iv)
  call void @gA(i32 %va)
  %iva = add i32 %iv, 1
  br label %loop
bodyB:
  %vb = call i32 @h(i32 %n)
  call void @gB(i32 %vb)
  %ivb = add i32 %iv, 2
  br label %loop
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 500, i32 500}
