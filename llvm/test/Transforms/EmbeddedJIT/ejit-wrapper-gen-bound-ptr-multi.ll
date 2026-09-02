; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s

; Multiple bound objects use the fixed raw-pointer descriptor table. The
; descriptor array is a slow-path alloca; no pointee payload is copied.

; CHECK-LABEL: define i32 @multi_bound_entry(i32 %cell, ptr %cfg_a, ptr %cfg_b, i32 %input)
; CHECK: alloca [2 x { ptr, i32, i32 }]
; CHECK: call i32 @ejit_taskpool_compile_or_get_bound_v(
; CHECK-SAME: i32 2,

define i32 @multi_bound_entry(i32 %cell, ptr %cfg_a, ptr %cfg_b, i32 %input) !ejit.metadata !0 {
entry:
  %a = load i32, ptr %cfg_a, !ejit.may_const !4
  %b = load i32, ptr %cfg_b, !ejit.may_const !4
  %sum = add i32 %a, %b
  %result = add i32 %sum, %input
  ret i32 %result
}

!0 = distinct !{!1, !2, !3, !5}
!1 = !{!"ejit_entry"}
!2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
!3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 4, !6}
!4 = !{}
!5 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 4, !7}
!6 = !{i64 0, i64 4}
!7 = !{i64 0, i64 4}
