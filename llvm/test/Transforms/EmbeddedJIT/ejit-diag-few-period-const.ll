; RUN: opt -passes=ejit-register-bitcode -ejit-warn-few-period-const=4 \
; RUN:     -ejit-warn-no-specialization=false -ejit-warn-unused-dim=false \
; RUN:     -S %s 2>&1 | FileCheck %s
; RUN: opt -passes=ejit-register-bitcode -ejit-warn-few-period-const=0 -S %s 2>&1 \
; RUN:     | FileCheck %s --check-prefix=OFF

; #3: warn when the specialization closure has fewer than N ejit_period_const
; reads. N=4 means 0..3 reads trigger the warning.

; 1 read → warn (below threshold 4).
; CHECK: EJit warning: ejit_entry function 'entry_one' has only 1 ejit_period_const read

; 0 reads → warn (below threshold 4).
; CHECK: EJit warning: ejit_entry function 'entry_zero' has only 0 ejit_period_const reads

; 3 reads in the closure (direct + callee) → warn.
; Warnings are emitted in module order (entry_one, entry_zero, then
; entry_three_via_helper).
; CHECK: EJit warning: ejit_entry function 'entry_three_via_helper' has only 3 ejit_period_const reads

; 4 reads → at threshold → no warning.
; CHECK-NOT: EJit warning: ejit_entry function 'entry_four'

; 5 reads → above threshold → no warning.
; CHECK-NOT: EJit warning: ejit_entry function 'entry_five'

; 1 read in a loop → K=1 is below threshold 4, but J=1 > 0 → the loop
; load is high specialization value → no warning.
; CHECK-NOT: EJit warning: ejit_entry function 'entry_one_in_loop'

; OFF threshold (0) → no warning at all.
; OFF-NOT: has only {{[0-9]+}} ejit_period_const read

@cell_data = global [16 x i32] zeroinitializer, !ejit.metadata !10

; ── 1 read: below threshold 4 → warn ──
define i32 @entry_one(i32 %c) !ejit.metadata !20 {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.period_const !{}
  ret i32 %v
}

; ── 0 reads: below threshold → warn ──
define i32 @entry_zero(i32 %c) !ejit.metadata !20 {
  %x = add i32 %c, 1
  ret i32 %x
}

; ── 4 reads: at threshold → no warn ──
define i32 @entry_four(i32 %c) !ejit.metadata !20 {
  %p0 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 0
  %v0 = load i32, ptr %p0, !ejit.period_const !{}
  %p1 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 1
  %v1 = load i32, ptr %p1, !ejit.period_const !{}
  %p2 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 2
  %v2 = load i32, ptr %p2, !ejit.period_const !{}
  %p3 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 3
  %v3 = load i32, ptr %p3, !ejit.period_const !{}
  %sum = add i32 %v0, %v1
  %sum2 = add i32 %sum, %v2
  %sum3 = add i32 %sum2, %v3
  ret i32 %sum3
}

; ── 5 reads: above threshold → no warn ──
define i32 @entry_five(i32 %c) !ejit.metadata !20 {
  %p0 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 0
  %v0 = load i32, ptr %p0, !ejit.period_const !{}
  %p1 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 1
  %v1 = load i32, ptr %p1, !ejit.period_const !{}
  %p2 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 2
  %v2 = load i32, ptr %p2, !ejit.period_const !{}
  %p3 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 3
  %v3 = load i32, ptr %p3, !ejit.period_const !{}
  %p4 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 4
  %v4 = load i32, ptr %p4, !ejit.period_const !{}
  %s0 = add i32 %v0, %v1
  %s1 = add i32 %s0, %v2
  %s2 = add i32 %s1, %v3
  %s3 = add i32 %s2, %v4
  ret i32 %s3
}

; ── 3 reads in closure (1 direct + 2 in callee) → below threshold → warn ──
define i32 @entry_three_via_helper(i32 %c) !ejit.metadata !20 {
  %v = call i32 @helper_two(i32 %c)
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %direct = load i32, ptr %p, !ejit.period_const !{}
  %sum = add i32 %v, %direct
  ret i32 %sum
}
define internal i32 @helper_two(i32 %c) {
  %p0 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 0
  %v0 = load i32, ptr %p0, !ejit.period_const !{}
  %p1 = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 1
  %v1 = load i32, ptr %p1, !ejit.period_const !{}
  %sum = add i32 %v0, %v1
  ret i32 %sum
}

; ── 1 read in a loop: below threshold 4, but in-loop → high value → no warn ──
; Use an outer function call to tether the loop so the back-edge
; survives preOptimize's SimplifyCFG.
declare void @sink(i32)
define i32 @entry_one_in_loop(i32 %n) !ejit.metadata !20 {
entry:
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%i.next, %loop]
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %i
  %v = load i32, ptr %p, !ejit.period_const !{}
  call void @sink(i32 %v)
  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret i32 %v
}

!10 = distinct !{!{!"ejit_group_indexed", !"cell", i32 16}, !{!"ejit_period_const_field", i32 0}}
!20 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
