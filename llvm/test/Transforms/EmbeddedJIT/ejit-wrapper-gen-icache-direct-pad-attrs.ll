; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -ejit-icache-direct-dispatch-pads=true -S %s | FileCheck %s

; Direct pads must preserve ABI-impacting parameter attributes so both musttail
; edges remain valid for real entry signatures, not only scalar test functions.

%S = type { i64, i64 }

; CHECK-LABEL: define void @cell_sret(
; CHECK-COUNT-16: musttail call void @__ejit_icache_pad_cell_sret_
; CHECK-LABEL: define internal void @__ejit_icache_pad_cell_sret_0(ptr noalias sret(%S) align 8 %{{.*}}, i32 %{{.*}})
; CHECK: musttail call void @cell_sret_miss(ptr noalias sret(%S) align 8 %{{.*}}, i32 %{{.*}})

define void @cell_sret(ptr noalias sret(%S) align 8 %out, i32 %cell) !ejit.metadata !0 {
entry:
  %field = getelementptr inbounds %S, ptr %out, i32 0, i32 0
  store i64 7, ptr %field, align 8
  ret void
}

@cell_data = global i32 0, !ejit.metadata !10

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 1}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
