; RUN: opt -passes="loop-vectorize" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-vectorize 2>&1 | FileCheck %s
; CHECK: !aimv.diag
;
; Verify that emitAIMVDiagnostic writes !aimv.diag metadata even without
; debug info. The loop has memory aliasing, so CantReorderMemOps should fire.

define void @test_no_dbg(ptr %a, ptr %b, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %gep_a = getelementptr i32, ptr %a, i32 %i
  %gep_b = getelementptr i32, ptr %b, i32 %i
  store i32 0, ptr %gep_a
  %v = load i32, ptr %gep_b
  %i.next = add i32 %i, 1
  %cmp = icmp slt i32 %i.next, 100
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}
