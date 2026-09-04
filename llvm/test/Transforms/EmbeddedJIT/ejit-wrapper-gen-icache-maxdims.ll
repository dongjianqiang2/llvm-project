; An ejit_entry with more than EJIT_ICACHE_MAX_DIMS (4) ejit_dim params is a
; compile error - the wrapper is not emitted. (The taskpool DimCount cap is 4,
; which the inline cache mirrors as EJIT_ICACHE_MAX_DIMS.)

; RUN: not opt -passes=ejit-wrapper-gen -ejit-inline-cache -S %s 2>&1 | FileCheck %s
; CHECK: error: ejit-wrapper-gen: more than 4 specialization dimensions are not supported

define i32 @five_dim_entry(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e) !ejit.metadata !0 {
entry:
  ret i32 0
}

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11
@data3 = global i32 0, !ejit.metadata !12
@data4 = global i32 0, !ejit.metadata !13
@data5 = global i32 0, !ejit.metadata !14

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"p1", i32 0}, !{!"ejit_period_arr_ind", !"p2", i32 1}, !{!"ejit_period_arr_ind", !"p3", i32 2}, !{!"ejit_period_arr_ind", !"p4", i32 3}, !{!"ejit_period_arr_ind", !"p5", i32 4}}
!10 = distinct !{!{!"ejit_period_arr", !"p1", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"p2", i32 32}}
!12 = distinct !{!{!"ejit_period_arr", !"p3", i32 48}}
!13 = distinct !{!{!"ejit_period_arr", !"p4", i32 64}}
!14 = distinct !{!{!"ejit_period_arr", !"p5", i32 80}}
