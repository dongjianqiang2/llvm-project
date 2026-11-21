; RUN: opt -passes=ejit-register-bitcode \
; RUN:     -ejit-report-period-const \
; RUN:     -ejit-warn-no-specialization=false -ejit-warn-unused-dim=false \
; RUN:     -S %s 2>&1 | FileCheck %s

; Option A: an info-level (non-gating) report of per-ejit_entry ejit_period_const
; read counts over the specialization closure: K total, J inside loops.

; A single period_const read inside a self-loop -> 1 total, 1 in loops.
; The loop must resist preOptimizeBitcode's full O2 function-simplification
; round: a constant trip count gets fully unrolled (one read per unrolled
; body, no loop left) and a loop-invariant load gets LICM'd out. Hence the
; opaque %n bound and the loop-variant GEP index - the shape real hot loops
; have (g_arr[i].field).
; CHECK: EJit info: ejit_entry function 'entry_loop': 1 ejit_period_const read (1 in loops)

; A single period_const read in straight-line code -> 1 total, 0 in loops.
; CHECK: EJit info: ejit_entry function 'entry_noloop': 1 ejit_period_const read (0 in loops)

; No period_const read -> 0 total, 0 in loops.
; CHECK: EJit info: ejit_entry function 'entry_plain': 0 ejit_period_const reads (0 in loops)

@cell_data = global [16 x i32] zeroinitializer, !ejit.metadata !10

declare void @sink(i32)

define i32 @entry_loop(i32 %c, i32 %n) !ejit.metadata !20 {
entry:
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%i.next, %loop]
  %acc = phi i32 [0, %entry], [%acc.next, %loop]
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %i
  %v = load i32, ptr %p, !ejit.period_const !{}
  call void @sink(i32 %v)
  %acc.next = add i32 %acc, %v
  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret i32 %acc.next
}

define i32 @entry_noloop(i32 %c) !ejit.metadata !20 {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.period_const !{}
  ret i32 %v
}

define void @entry_plain(i32 %c) !ejit.metadata !30 {
  %x = add i32 %c, 1
  ret void
}

!10 = distinct !{!{!"ejit_group_indexed", !"cell", i32 16}, !{!"ejit_period_const_field", i32 0}}
!20 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
!30 = distinct !{!{!"ejit_entry"}}
