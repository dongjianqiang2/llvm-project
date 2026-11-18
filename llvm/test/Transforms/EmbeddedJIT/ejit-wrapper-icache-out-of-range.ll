; RUN: opt -passes=ejit-wrapper-gen -ejit-icache-section= -S %s | FileCheck %s
;
; Check full-width identity before truncating for a table lookup or the
; 32-bit runtime ABI. An oversized ID must not dispatch to its low bits.
; CHECK: @wide.icache_miss = private constant ptr @wide_miss, align 8
; CHECK-LABEL: define i64 @wide(
; CHECK: icmp ult i64 %cell, 16
; CHECK: %ejit_ic_slot = getelementptr
; CHECK: [[PTR:%.*]] = select i1 {{.*}}, ptr %ejit_ic_slot, ptr @wide.icache_miss
; CHECK: load atomic ptr, ptr [[PTR]] monotonic
; CHECK: musttail call i64
; CHECK-LABEL: define internal i64 @wide_miss(
; CHECK: [[FITS:%.*]] = icmp ule i64 %0, 4294967295
; CHECK: [[LOW:%.*]] = trunc i64 %0 to i32
; CHECK: select i1 [[FITS]], i32 [[LOW]], i32 -1
; CHECK: call i32 @ejit_taskpool_compile_or_get(
define i64 @wide(i64 %cell) !ejit.metadata !0 {
  ret i64 %cell
}

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
