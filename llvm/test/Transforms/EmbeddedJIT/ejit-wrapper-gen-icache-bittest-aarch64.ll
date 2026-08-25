; REQUIRES: aarch64-registered-target
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -ejit-icache-direct-dispatch-pads=true -S %s | llc -mtriple=aarch64-unknown-none-elf -O2 | FileCheck %s

; The 1D selector should lower its lower tree levels to single-instruction bit
; tests. The range guard may let CodeGen retain threshold branches near the
; root, but the old all-threshold tree emitted no tbz/tbnz at all.
; CHECK-LABEL: cell_entry:
; CHECK: cmp w0, #16
; CHECK: tbnz w0, #2,
; CHECK: tbnz w0, #1,
; CHECK: tbnz w0, #0,
; CHECK-NOT: blr
; CHECK: .size cell_entry
; CHECK-LABEL: __ejit_icache_pad_cell_entry_0:
; CHECK: b cell_entry_miss

target triple = "aarch64-unknown-none-elf"

define i32 @cell_entry(i32 %cell, i32 %value) !ejit.metadata !0 {
entry:
  %result = add i32 %value, 1
  ret i32 %result
}

@cell_data = global i32 0, !ejit.metadata !1

!0 = distinct !{!{!"ejit_entry"},
                !{!"ejit_period_arr_ind", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
