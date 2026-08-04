; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes=ejit-wrapper-gen -ejit-wrapper-timing -S %s | FileCheck %s --check-prefix=ON

; OFF-NOT: @ejit_taskpool_trace_now
; OFF-NOT: @ejit_taskpool_trace_wrapper

; ON-LABEL: define i32 @timed_entry(
; ON: call i64 @ejit_taskpool_trace_now()
; ON: call i32 @ejit_taskpool_compile_or_get_1d
; ON: call i64 @ejit_taskpool_trace_now()
; ON: call i32 %ejit_fn
; ON: call i64 @ejit_taskpool_trace_now()
; ON: call void @ejit_taskpool_release_read
; ON: call i64 @ejit_taskpool_trace_now()
; ON: call void @ejit_taskpool_trace_wrapper

define i32 @timed_entry(i32 %cell) !ejit.metadata !0 {
entry:
  %v = load i32, ptr @data
  ret i32 %v
}

@data = global i32 7, !ejit.metadata !10

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
