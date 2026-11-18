; RUN: not opt -passes=ejit-wrapper-gen -disable-output %s 2>&1 | FileCheck %s
; CHECK: error: ejit-wrapper-gen: invalid period name in ejit_period_arr_ind:

define i32 @entry(i32 %idx) !ejit.metadata !0 {
  ret i32 %idx
}

!0 = !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"", i32 0}}
