; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s

; A bound pointer uses the generic bound slow-path API even when fixed-dim
; entries are enabled. The ordinary icache hit path remains outside this call.

; CHECK-LABEL: define i32 @bound_entry(i32 %cell, ptr %cfg, i32 %input)
; CHECK: jit_call:
; CHECK: call i32 @ejit_taskpool_compile_or_get_bound(i32 {{.*}}, ptr {{.*}}, i32 1, ptr %cfg, i32 12, i32 1, ptr {{.*}}, ptr {{.*}})
; CHECK-NOT: call i32 @ejit_taskpool_compile_or_get_1d

define i32 @bound_entry(i32 %cell, ptr %cfg, i32 %input) !ejit.metadata !0 {
entry:
  %field = getelementptr { i32, i32, i32 }, ptr %cfg, i32 0, i32 0
  %v = load i32, ptr %field, !ejit.period_const !4
  %r = add i32 %v, %input
  ret i32 %r
}

!0 = distinct !{!1, !2, !3}
!1 = !{!"ejit_entry"}
!2 = !{!"ejit_dim", !"cell", i32 0}
!3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 12, !5}
!4 = !{}
!5 = !{i64 0, i64 4}
