; RUN: opt -passes=ejit-register-bitcode -S %s 2>&1 | FileCheck %s
; RUN: opt -passes=ejit-register-bitcode -ejit-warn-unused-dim=false -S %s 2>&1 | FileCheck %s --check-prefix=OFF

; #2: an ejit_entry that declares ejit_dim(P) but whose closure
; never indexes an ejit_group(P) has an unused specialization dimension.

; CHECK: EJit warning: ejit_entry function 'entry_unused_cell' declares ejit_dim('cell') but its specialization closure never indexes an ejit_group('cell')

; The used dim (trp) on the same function must NOT warn, and an entry that does
; index its declared dim must NOT warn.
; CHECK-NOT: declares ejit_dim('trp')
; CHECK-NOT: ejit_entry function 'entry_used' declares ejit_dim

; With the flag off, no #2 warning at all.
; OFF-NOT: declares ejit_dim

@cell_data = global [16 x i32] zeroinitializer, !ejit.metadata !10
@trp_data = global [8 x i32] zeroinitializer, !ejit.metadata !11

; Declares cell + trp, but only indexes trp_data -> the cell dim is unused.
; (Still has specialization value via trp, so #1 does not fire either.)
define i32 @entry_unused_cell(i32 %c, i32 %t) !ejit.metadata !21 {
  %p = getelementptr [8 x i32], ptr @trp_data, i32 0, i32 %t
  %v = load i32, ptr %p, !ejit.period_const !{}
  ret i32 %v
}

; Declares cell and indexes cell_data -> dim used, no warning.
define i32 @entry_used(i32 %c) !ejit.metadata !20 {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.period_const !{}
  ret i32 %v
}

!10 = distinct !{!{!"ejit_group_indexed", !"cell", i32 16}, !{!"ejit_period_const_field", i32 0}}
!11 = distinct !{!{!"ejit_group_indexed", !"trp", i32 8}, !{!"ejit_period_const_field", i32 0}}
!20 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
!21 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}, !{!"ejit_dim", !"trp", i32 1}}
