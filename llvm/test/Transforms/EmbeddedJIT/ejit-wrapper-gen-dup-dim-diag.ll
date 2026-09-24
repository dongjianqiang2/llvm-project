; A duplicated lifecycle dimension in !ejit.metadata is rejected, and the
; diagnostic names the period and BOTH argument positions. Hand-written IR
; bypasses Clang Sema, so the pass is the last line of defense; through the
; clang pipeline the same error is anchored at the function definition.

; RUN: not opt -passes=ejit-wrapper-gen -S %s 2>&1 | FileCheck %s
; CHECK: error: {{.*}}in function dup_dim_entry{{.*}}ejit-wrapper-gen: duplicated lifecycle dimension 'cell' in ejit_period_arr_ind metadata (arguments 0 and 1); each lifecycle may be indexed by only one parameter

define i32 @dup_dim_entry(i32 %a, i32 %b) !ejit.metadata !0 {
entry:
  ret i32 0
}

@cell = global [16 x i32] zeroinitializer, !ejit.metadata !10

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"cell", i32 1}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
