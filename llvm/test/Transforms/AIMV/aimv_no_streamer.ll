; RUN: opt -passes="loop-vectorize,aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json 2>&1 | FileCheck %s
; CHECK-NOT: aimv.diag

; Test: without remark streamer, !aimv.diag is NOT generated

define void @test_no_streamer(ptr %a, ptr %b, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %gep_a = getelementptr i32, ptr %a, i32 %i
  %gep_b = getelementptr i32, ptr %b, i32 %i
  store i32 0, ptr %gep_a
  %v = load i32, ptr %gep_b
  %i.next = add i32 %i, 1
  %cmp = icmp slt i32 %i.next, %n
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}
