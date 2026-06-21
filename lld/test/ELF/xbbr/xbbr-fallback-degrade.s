# REQUIRES: aarch64

# P0-1: when pinning BBs to satisfy the thunk-byte budget would revert more
# than 30% of migratable BBs, Stage 4 degrades the whole pipeline to
# function-level mode (SPEC §7) — ClusterBBOrders is cleared and the link
# falls back to CGProfile function-level ordering. The degrade emits a
# warning (-Werror friendly) and records degraded=1 in the Stage 4 stats.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null

# A tight budget (1 byte) with a small projected branch range (knob=8) makes
# every over-range B/BL unaffordable; Stage 4 pins until it crosses 30% of the
# 6 migratable BBs (limit=1), then degrades.
# RUN: ld.lld -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-branch-range-for-testing=8 \
# RUN:     --bb-cross-reorder-max-thunk-bytes=1 --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe 2>&1 \
# RUN:     | FileCheck %s --check-prefix=DEGRADE
# DEGRADE: warning: XBBR: {{.*}} migratable BBs reverted over the 30% threshold
# DEGRADE-SAME: degrading to function-level mode
# DEGRADE: xbbr-stage4: overRange=2 estThunkBytes=32 pinned=2 degraded=1

# Degraded layout still links and is reproducible (SPEC §9.3).
# RUN: ld.lld -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-branch-range-for-testing=8 \
# RUN:     --bb-cross-reorder-max-thunk-bytes=1 \
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
