# REQUIRES: aarch64

# P1-3: a B.cond (R_AARCH64_CONDBR19) whose source AND target are hot,
# non-entry BBs in an all-hot cond-branch connected component MAY migrate to
# .text.hot — the pin is relaxed. Pre-P1-3 every CondInvolved BB was pinned.
#
# The IR is a loop with a PHI header (so the loop header is a separate, non-
# entry BB — the PHI blocks the entry/loop fallthrough merge) and two distinct
# hot bodies (bodyA/bodyB). The loop header's `br i1` lowers to B.cond → bodyB
# + B → bodyA, so {loop, bodyB} is an all-hot component: both migrate. With the
# real ±1 MiB B.cond range the projected distance is far in-range, so Stage 4
# pins nothing. The link succeeds, the cond BBs are Moved (not Anchored), and
# the result is reproducible.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir f.ll \
# RUN:     -c -o f.o 2>/dev/null

# Real cond range: cond BBs migrate → Stage 4 pins nothing.
# RUN: ld.lld -e f f.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-stats --bb-cross-reorder-emit-decision-map \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe 2> %t/stats
# RUN: FileCheck %s --check-prefix=STATS < %t/stats
# STATS: xbbr-stage4: overRange=0 estThunkBytes=0 pinned=0 degraded=0

# The cond-branch BBs migrated (≥2 Moved). Pre-P1-3 they were Anchored (Moved=1).
# RUN: llvm-bbreorder-dump --summary %t/exe | FileCheck %s --check-prefix=SUM
# SUM: moved={{[2-9]}}

# Reproducibility (SPEC §9.3): same flags (incl. --emit-decision-map, which
# adds a section) → bitwise-identical.
# RUN: ld.lld -e f f.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-emit-decision-map --unresolved-symbols=ignore-all \
# RUN:     -o %t/exe2 2>/dev/null
# RUN: cmp %t/exe %t/exe2

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
