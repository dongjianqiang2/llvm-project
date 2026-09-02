; RUN: not opt -passes=ejit-wrapper-gen -S %s 2>&1 | FileCheck %s

; Hand-written IR bypasses Clang Sema, so wrapper generation must enforce the
; same eight-descriptor limit before emitting a runtime-rejected _bound_v call.

; CHECK: error: ejit-wrapper-gen: function has 9 ejit_bound_ptr parameters; at most 8 are supported

define i32 @too_many_bound_ptrs(i32 %cell, ptr %p0, ptr %p1, ptr %p2,
                                 ptr %p3, ptr %p4, ptr %p5, ptr %p6,
                                 ptr %p7, ptr %p8) !ejit.metadata !0 {
entry:
  ret i32 0
}

!0 = distinct !{!1, !2, !3, !4, !5, !6, !7, !8, !9, !10, !11}
!1 = !{!"ejit_entry"}
!2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
!3 = !{!"ejit_bound_ptr", !"cell", i32 1, i64 4}
!4 = !{!"ejit_bound_ptr", !"cell", i32 2, i64 4}
!5 = !{!"ejit_bound_ptr", !"cell", i32 3, i64 4}
!6 = !{!"ejit_bound_ptr", !"cell", i32 4, i64 4}
!7 = !{!"ejit_bound_ptr", !"cell", i32 5, i64 4}
!8 = !{!"ejit_bound_ptr", !"cell", i32 6, i64 4}
!9 = !{!"ejit_bound_ptr", !"cell", i32 7, i64 4}
!10 = !{!"ejit_bound_ptr", !"cell", i32 8, i64 4}
!11 = !{!"ejit_bound_ptr", !"cell", i32 9, i64 4}
