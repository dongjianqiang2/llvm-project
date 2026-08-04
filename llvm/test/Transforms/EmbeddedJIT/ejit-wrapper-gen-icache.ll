; -ejit-inline-cache (ON): emits an inline probe DIRECTLY in the ejit_entry
; wrapper - a GEP into the per-function @__ejit_icache_fn_<name> [D]^numDims
; array (D = EJIT_ICACHE_DIM_SIZE, power-of-2) by the ejit_dim argument values,
; then a plain load + null-check; the hit path (jit_icache_dispatch) calls the
; cached specialization directly with NO ejit_icache_try call and NO
; ejit_taskpool_release_read. numDims=0 is a scalar slot (no GEP). Default
; (OFF): the original 4-block wrapper, no icache. Idempotent: running the pass
; twice does not duplicate the probe.

; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -S %s | FileCheck %s --check-prefix=ICACHE
; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s --check-prefix=NOICACHE
; RUN: opt -passes=ejit-wrapper-gen,ejit-wrapper-gen -ejit-inline-cache -S %s | FileCheck %s --check-prefix=IDEM
; --- -ejit-wrapper-timing + -ejit-inline-cache: the hit path emits trace calls
;     AFTER the specialization call, so it must NOT be musttail (musttail must
;     immediately precede a ret). Verify the module is valid (opt verifies by
;     default) and the hit path is a plain call. Regression for the broken-IR
;     boot crash where codegen silently dropped the ejit_entry function. ---
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-wrapper-timing -disable-output %s
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-wrapper-timing -S %s | FileCheck %s --check-prefix=TIMING

; --- icache globals: [D]^numDims, D=8 (EJIT_ICACHE_DIM_SIZE). 0D is scalar. ---
; ICACHE-DAG: @__ejit_icache_fn_zero_dim_entry = internal global ptr null, align 8
; ICACHE-DAG: @__ejit_icache_fn_one_dim_entry = internal global [16 x ptr] zeroinitializer, align 8
; ICACHE-DAG: @__ejit_icache_fn_two_dim_entry = internal global [16 x [16 x ptr]] zeroinitializer, align 8

; --- 0D entry: scalar slot, direct plain load (NO GEP). ---
; ICACHE-LABEL: define i32 @zero_dim_entry(
; ICACHE-NOT: ejit_icache_try
; ICACHE-NOT: getelementptr
; ICACHE: load ptr, ptr @__ejit_icache_fn_zero_dim_entry, align 8
; ICACHE-LABEL: jit_icache_dispatch:
; ICACHE-NOT: call void @ejit_taskpool_release_read
; ICACHE: call {{.*}} %ejit_ic_fn
; ICACHE: ret

; --- 1D entry: [16 x ptr] slot, GEP by the single dim arg + plain load. ---
; ICACHE-LABEL: define i32 @one_dim_entry(
; ICACHE-NOT: ejit_icache_try
; ICACHE: getelementptr {{.*}} ptr @__ejit_icache_fn_one_dim_entry, i32 0, i32 {{.*}}
; ICACHE: load ptr, ptr {{.*}}, align 8
; ICACHE-LABEL: jit_icache_dispatch:
; ICACHE-NOT: call void @ejit_taskpool_release_read
; ICACHE: call {{.*}} %ejit_ic_fn
; ICACHE: ret

; --- 2D entry: [16 x [16 x ptr]] slot, 2-subscript GEP. ---
; ICACHE-LABEL: define i32 @two_dim_entry(
; ICACHE: getelementptr {{.*}} ptr @__ejit_icache_fn_two_dim_entry, i32 0, i32 {{.*}}, i32 {{.*}}

; --- registration carries numDims (3rd arg): 0 / 1 / 2 (DAG: order-independent). ---
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_zero_dim_entry, i32 0)
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_one_dim_entry, i32 1)
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_two_dim_entry, i32 2)

; --- Default (flag OFF): no icache anywhere; original compile_or_get path. ---
; NOICACHE-LABEL: define i32 @one_dim_entry(
; NOICACHE-NOT: __ejit_icache_fn
; NOICACHE-NOT: ejit_register_icache_slot
; NOICACHE: call i32 @ejit_taskpool_compile_or_get_1d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})

; --- Idempotent: two passes emit each probe exactly once. ---
; IDEM-LABEL: define i32 @one_dim_entry(
; IDEM-COUNT-1: getelementptr {{.*}} @__ejit_icache_fn_one_dim_entry, i32 0, i32 {{.*}}

; --- timing hit path: plain call (NOT musttail) + trace + ret. The miss path
;     stays musttail (no trailing calls), so we only forbid musttail within the
;     hit block. ---
; TIMING-LABEL: define i32 @zero_dim_entry(
; TIMING-LABEL: jit_icache_dispatch:
; TIMING-NOT: musttail
; TIMING: call i32 %ejit_ic_fn
; TIMING: call i64 @ejit_taskpool_trace_now
; TIMING: call void @ejit_taskpool_trace_wrapper
; TIMING: ret

define i32 @zero_dim_entry(i32 %x) !ejit.metadata !0 {
entry:
  %v1 = load i32, ptr @data
  ret i32 0
}

define i32 @one_dim_entry(i32 %idx1) !ejit.metadata !1 {
entry:
  %v1 = load i32, ptr @data
  ret i32 0
}

define i32 @two_dim_entry(i32 %a, i32 %b) !ejit.metadata !2 {
entry:
  %v1 = load i32, ptr @data
  %v2 = load i32, ptr @data2
  ret i32 0
}

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 32}}
