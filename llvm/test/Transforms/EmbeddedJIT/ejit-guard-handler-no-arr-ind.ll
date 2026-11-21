; RUN: opt -passes=ejit-guard-handler -S %s 2>&1 | FileCheck %s

; Verify diagnostic warning when ejit_guard has no matching ejit_dim.
; CHECK: EJit warning: function 'lc_no_arr_ind' has ejit_guard("cell") but no matching ejit_dim("cell") parameter

; Function with both lc and arr_ind — no warning expected.
; CHECK-NOT: EJit warning: function 'lc_ok' has ejit_guard

@cell_data = global [10 x i32] zeroinitializer, !ejit.metadata !10

; Bad: ejit_guard without matching ejit_dim
define void @lc_no_arr_ind(i32 %x) !ejit.metadata !20 {
entry:
  ret void
}

; Good: ejit_guard WITH matching ejit_dim
define void @lc_ok(i32 %cell_idx) !ejit.metadata !22 {
entry:
  ret void
}

!10 = distinct !{!{!"ejit_group_indexed", !"cell", i32 10}}
!20 = distinct !{!{!"ejit_guard", !"cell"}}
!22 = distinct !{!{!"ejit_guard", !"cell"}, !{!"ejit_dim", !"cell", i32 0}}
