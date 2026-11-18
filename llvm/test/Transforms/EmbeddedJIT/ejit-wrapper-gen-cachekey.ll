; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s
;
; The generic lookup receives dimension type/instance pairs in argument order.
; CHECK-LABEL: define void @two_dim_i8(i8 %cell, i8 %trp)
; CHECK: load i32, ptr @__ejit_funcidx_two_dim_i8
; CHECK: load i32, ptr @__ejit_dimtype_cell
; CHECK: zext i8 %cell to i32
; CHECK: load i32, ptr @__ejit_dimtype_trp
; CHECK: zext i8 %trp to i32
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 2, ptr {{.*}}, ptr {{.*}})
; CHECK: call void @ejit_taskpool_release_read
define void @two_dim_i8(i8 %cell, i8 %trp) !ejit.metadata !0 {
entry:
  ret void
}

; --- 1D on arg 1 (not arg 0): reads %cell (arg 1), skips %ignored. ---
; CHECK-LABEL: define void @dim_on_arg1(i32 %ignored, i8 %cell)
; CHECK: load i32, ptr @__ejit_funcidx_dim_on_arg1
; CHECK: load i32, ptr @__ejit_dimtype_cell
; CHECK: zext i8 %cell to i32
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 1, ptr {{.*}}, ptr {{.*}})
; CHECK: call void @ejit_taskpool_release_read
define void @dim_on_arg1(i32 %ignored, i8 %cell) !ejit.metadata !1 {
entry:
  ret void
}

; --- i64 dim: truncated to i32 before the generic API call. ---
; CHECK-LABEL: define void @dim_i64(i64 %cell)
; CHECK: trunc i64 %cell to i32
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 1, ptr {{.*}}, ptr {{.*}})
; CHECK: call void @ejit_taskpool_release_read
define void @dim_i64(i64 %cell) !ejit.metadata !2 {
entry:
  ret void
}

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 1}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
