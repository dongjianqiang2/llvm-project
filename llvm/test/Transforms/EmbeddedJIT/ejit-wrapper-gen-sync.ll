; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s
;
; Sync and async share one generic wrapper ABI.

; CHECK: @__ejit_dimtype_cell = internal global i32 -1
; CHECK: @__ejit_funcidx_sync_entry = internal global i32 -1

; CHECK-LABEL: define i32 @sync_entry(i32 %cell)
; CHECK: alloca ptr
; CHECK: alloca i32
; CHECK: load i32, ptr @__ejit_funcidx_sync_entry
; CHECK: icmp ne
; CHECK: br {{.*}}label %jit_call{{.*}}label %jit_fallback

; CHECK: jit_call:
; CHECK: load i32, ptr @__ejit_dimtype_cell
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 1, ptr {{.*}}, ptr {{.*}})
; CHECK: icmp eq
; CHECK: icmp ne
; CHECK: br {{.*}}label %jit_dispatch{{.*}}label %jit_fallback

; CHECK: jit_fallback:
; CHECK: load i32, ptr @data
; CHECK: ret i32

; CHECK: jit_dispatch:
; CHECK: call i32 {{.*}}(i32 %cell)
; CHECK: call void @ejit_taskpool_release_read
; CHECK: ret i32

define i32 @sync_entry(i32 %cell) !ejit.metadata !0 {
entry:
  %v = load i32, ptr @data
  ret i32 %v
}

@data = global i32 7, !ejit.metadata !1

!0 = distinct !{!{!"ejit_entry"},
                 !{!"ejit_period_arr_ind", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
