; RUN: opt -passes=ejit-wrapper-gen -ejit-icache-section= -S %s | FileCheck %s --check-prefixes=OFF,MIXED
;
; Moving AOT blocks into the miss function preserves original musttail exits.
; OFF: @__ejit_icache_fn_musttail_entry = internal global [16 x ptr] [ptr @musttail_entry_miss, {{.*}}]
; OFF-LABEL: define i32 @musttail_entry(
; OFF-NOT: br
; OFF: musttail call i32 %ejit_ic_fn
; OFF-LABEL: define internal i32 @musttail_entry_miss(i32 %0)
; OFF: musttail call i32 @helper(i32 %0)
; MIXED-LABEL: define internal i32 @mixed_entry_miss(i32 %0, i32 %1)
; MIXED-LABEL: miss_fallback:
; MIXED: mixed.normal:
; MIXED: ret i32 %1
; MIXED: mixed.tail:
; MIXED: musttail call i32 @mixed_helper(i32 %0, i32 %1)

define i32 @helper(i32 %cell) {
entry:
  ret i32 %cell
}

define i32 @mixed_helper(i32 %cell, i32 %value) {
entry:
  %sum = add i32 %cell, %value
  ret i32 %sum
}

define i32 @musttail_entry(i32 %cell) !ejit.metadata !0 {
entry:
  %r = musttail call i32 @helper(i32 %cell)
  ret i32 %r
}

define i32 @mixed_entry(i32 %cell, i32 %value) !ejit.metadata !1 {
entry:
  %take.tail = icmp eq i32 %value, 0
  br i1 %take.tail, label %mixed.tail, label %mixed.normal

mixed.normal:
  ret i32 %value

mixed.tail:
  %r = musttail call i32 @mixed_helper(i32 %cell, i32 %value)
  ret i32 %r
}

@data = global [16 x i32] zeroinitializer, !ejit.metadata !10

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
