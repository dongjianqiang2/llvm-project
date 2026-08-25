; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=false -S %s | FileCheck %s --check-prefix=DEFAULT
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -S %s | FileCheck %s --check-prefix=SPLIT
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -ejit-icache-direct-dispatch-pads=true -S %s | FileCheck %s --check-prefix=DIRECT

; The default remains the compact, single indirect callsite.
; DEFAULT-NOT: jit_icache_probe_0:
; DEFAULT-COUNT-4: jit_icache_dispatch:

; Eligible one-dimensional cell entries use a bit-test conditional branch tree.
; Values outside [0, 15] bypass the inline table and take the existing miss path.
; Every in-range instance has a distinct constant slot load and indirect call PC.
; SPLIT-LABEL: define i32 @cell_entry(
; SPLIT: %ejit_split_in_range = icmp ult i32 %cell, 16
; SPLIT: br i1 %ejit_split_in_range, label %jit_icache_select_0_16, label %jit_miss
; SPLIT-NOT: switch
; SPLIT: jit_icache_probe_0:
; SPLIT: load atomic ptr, ptr @__ejit_icache_fn_cell_entry monotonic
; SPLIT: jit_icache_dispatch_0:
; SPLIT-COUNT-16: musttail call i32 %ejit_ic_fn_
; SPLIT-DAG: and i32 %cell, 8
; SPLIT-DAG: and i32 %cell, 4
; SPLIT-DAG: and i32 %cell, 2
; SPLIT-DAG: and i32 %cell, 1

; trp is the other supported one-dimensional lifecycle name.
; SPLIT-LABEL: define i32 @trp_entry(
; SPLIT: %ejit_split_in_range = icmp ult i32 %trp, 16
; SPLIT: jit_icache_dispatch_0:
; SPLIT: jit_icache_dispatch_15:

; Two-dimensional entries deliberately retain the existing compact dispatcher.
; SPLIT-LABEL: define i32 @two_dim_entry(
; SPLIT-NOT: jit_icache_probe_0:
; SPLIT: jit_icache_dispatch:

; Direct-pad mode removes the slot load and indirect call from eligible leaves.
; The AOT pad symbols initially tail-call the common miss function; runtime
; publication patches that one B instruction to the JIT body.
; DIRECT-DAG: @__ejit_icache_pad_table_cell_entry = private constant [17 x ptr]
; DIRECT-DAG: @__ejit_icache_pad_table_trp_entry = private constant [17 x ptr]
; DIRECT-DAG: @.ejit.registry.icache_pads = private constant {{.*}} i32 8, {{.*}} ptr @__ejit_icache_pad_table_cell_entry, i64 16
; DIRECT-LABEL: define i32 @cell_entry(
; DIRECT: %ejit_split_in_range = icmp ult i32 %cell, 16
; DIRECT-NOT: load atomic ptr
; DIRECT-COUNT-16: musttail call i32 @__ejit_icache_pad_cell_entry_
; DIRECT-LABEL: define i32 @trp_entry(
; DIRECT-NOT: load atomic ptr
; DIRECT-COUNT-16: musttail call i32 @__ejit_icache_pad_trp_entry_
; DIRECT-LABEL: define i32 @two_dim_entry(
; DIRECT: load atomic ptr
; DIRECT: musttail call i32 %ejit_ic_fn
; DIRECT-DAG: define internal i32 @__ejit_icache_pad_cell_entry_0({{.*}}) {{.*}}section ".text.ejit_pads" align 4
; DIRECT-DAG: musttail call i32 @cell_entry_miss(

; Other one-dimensional lifecycle names also retain the compact dispatcher.
; SPLIT-LABEL: define i32 @other_entry(
; SPLIT-NOT: jit_icache_probe_0:
; SPLIT: jit_icache_dispatch:

define i32 @cell_entry(i32 %cell, i32 %value) !ejit.metadata !0 {
entry:
  %r = add i32 %value, 1
  ret i32 %r
}

define i32 @trp_entry(i32 %trp) !ejit.metadata !1 {
entry:
  ret i32 %trp
}

define i32 @two_dim_entry(i32 %cell, i32 %trp) !ejit.metadata !2 {
entry:
  %r = add i32 %cell, %trp
  ret i32 %r
}

define i32 @other_entry(i32 %slot) !ejit.metadata !3 {
entry:
  ret i32 %slot
}

@cell_data = global i32 0, !ejit.metadata !10
@trp_data = global i32 0, !ejit.metadata !11
@other_data = global i32 0, !ejit.metadata !12

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"trp", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!3 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"slot", i32 0}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 16}}
!12 = distinct !{!{!"ejit_period_arr", !"slot", i32 16}}
