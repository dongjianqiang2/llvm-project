; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s
;
; A bound-pointer miss passes its descriptor to the bound lookup API.

; CHECK-LABEL: define i32 @bound_entry(i32 %cell, ptr %cfg, i32 %input)
; CHECK: jit_call:
; CHECK: call i32 @ejit_taskpool_compile_or_get_bound(i32 {{.*}}, ptr {{.*}}, i32 1, ptr %cfg, i32 12, i32 1, ptr {{.*}}, ptr {{.*}})

define i32 @bound_entry(i32 %cell, ptr %cfg, i32 %input) !ejit.metadata !0 {
entry:
  %field = getelementptr { i32, i32, i32 }, ptr %cfg, i32 0, i32 0
  %v = load i32, ptr %field, !ejit.may_const !4
  %r = add i32 %v, %input
  ret i32 %r
}

!0 = distinct !{!1, !2, !3}
!1 = !{!"ejit_entry"}
!2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
!3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 12, !5}
!4 = !{}
!5 = !{i64 0, i64 4}
