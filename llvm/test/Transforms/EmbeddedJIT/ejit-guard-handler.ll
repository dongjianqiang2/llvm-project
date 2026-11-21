; RUN: opt -passes=ejit-guard-handler -S %s | FileCheck %s

; CHECK: define void @lc_single(i32 %cell_idx)
; CHECK: call void @ejit_deactivate(ptr {{.*}}, i32 %cell_idx)
; CHECK: call void @ejit_activate(ptr {{.*}}, i32 %cell_idx)

; Multi-lc: deactivate in order (cell, trp), activate same order (cell, trp)
; CHECK: define void @lc_multi(i32 %cell_idx, i32 %trp_idx)
; CHECK: call void @ejit_deactivate(ptr {{.*}}, i32 %cell_idx)
; CHECK: call void @ejit_deactivate(ptr {{.*}}, i32 %trp_idx)
; CHECK: call void @ejit_activate(ptr {{.*}}, i32 %cell_idx)
; CHECK: call void @ejit_activate(ptr {{.*}}, i32 %trp_idx)

@cell_data = global [10 x i32] zeroinitializer, !ejit.metadata !10
@trp_data = global [5 x i32] zeroinitializer, !ejit.metadata !12

define void @lc_single(i32 %cell_idx) !ejit.metadata !20 {
entry:
  ret void
}

define void @lc_multi(i32 %cell_idx, i32 %trp_idx) !ejit.metadata !22 {
entry:
  ret void
}

!10 = distinct !{!{!"ejit_group_indexed", !"cell", i32 10}}
!12 = distinct !{!{!"ejit_group_indexed", !"trp", i32 5}}
!20 = distinct !{!{!"ejit_guard", !"cell"}, !{!"ejit_dim", !"cell", i32 0}}
!22 = distinct !{!{!"ejit_guard", !"cell"}, !{!"ejit_guard", !"trp"}, !{!"ejit_dim", !"cell", i32 0}, !{!"ejit_dim", !"trp", i32 1}}
