; RUN: not opt -passes=ejit-wrapper-gen -disable-output %s 2>&1 | FileCheck %s
;
; Reject excess lifecycle dimensions before creating a wrapper or a cache key.

define void @too_many_dims(i8 %a, i8 %b, i8 %c, i8 %d,
                           i8 %e, i8 %f, i8 %g, i8 %h, i8 %i)
                           !ejit.metadata !0 {
entry:
  ret void
}

!0 = distinct !{!{!"ejit_entry"},
  !{!"ejit_period_arr_ind", !"d0", i32 0}, !{!"ejit_period_arr_ind", !"d1", i32 1},
  !{!"ejit_period_arr_ind", !"d2", i32 2}, !{!"ejit_period_arr_ind", !"d3", i32 3},
  !{!"ejit_period_arr_ind", !"d4", i32 4}, !{!"ejit_period_arr_ind", !"d5", i32 5},
  !{!"ejit_period_arr_ind", !"d6", i32 6}, !{!"ejit_period_arr_ind", !"d7", i32 7},
  !{!"ejit_period_arr_ind", !"d8", i32 8}}

; CHECK: error: ejit-wrapper-gen: module references 9 distinct lifecycle dimensions but at most 8 are supported
