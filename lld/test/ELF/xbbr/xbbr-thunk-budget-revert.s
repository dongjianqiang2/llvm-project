# REQUIRES: aarch64

# P0-1: when the projected-VA estimate of thunk bytes exceeds
# --bb-cross-reorder-max-thunk-bytes, Stage 4 pins (reverts to original
# function position) the migratable BBs involved in the most over-range B/BL
# edges until the estimate fits the budget — WITHOUT crossing the 30%
# degrade threshold (SPEC §7). This is the "single-BB revert" path.
#
# The hidden --bb-cross-reorder-branch-range-for-testing= knob shrinks the
# branch range Stage 4 projects against, so a tiny binary exercises the
# budget path instead of a 128 MiB filler. It affects only XBBR's projected
# estimate, not lld's real thunk insertion (always ISA ranges) nor the
# CondInvolved correctness pin.
#
# With knob=16 the 3-function cluster has 2 over-range B/BL edges
# (estThunkBytes=32); budget=16 allows one thunk, so Stage 4 pins one BB,
# the estimate drops to 16 (≤ budget), and the layout converges with
# pinned=1, degraded=0.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null
# RUN: ld.lld -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-branch-range-for-testing=16 \
# RUN:     --bb-cross-reorder-max-thunk-bytes=16 --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe 2>&1 \
# RUN:     | FileCheck %s --check-prefix=STAGE4
# STAGE4: xbbr-stage4: overRange=1 estThunkBytes=16 pinned=1 degraded=0

# The reverted (pinned) BB drops out of the XBBR layout order and lands at its
# original function slot; the link still succeeds and is reproducible.
# RUN: ld.lld -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-branch-range-for-testing=16 \
# RUN:     --bb-cross-reorder-max-thunk-bytes=16 \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe2 2>/dev/null
# RUN: cmp %t/exe %t/exe2

#--- a.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @ext(i32)
define void @a(i32 %n) noinline !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @b(i32 %n)
  br label %done
done:
  call void @ext(i32 1)
  ret void
}
define void @b(i32 %n) noinline !prof !2 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @c(i32 %n)
  br label %done
done:
  call void @ext(i32 2)
  ret void
}
define void @c(i32 %n) noinline !prof !3 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @ext(i32 3)
  br label %done
done:
  call void @ext(i32 4)
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
!2 = !{!"function_entry_count", i64 900}
!3 = !{!"function_entry_count", i64 800}
