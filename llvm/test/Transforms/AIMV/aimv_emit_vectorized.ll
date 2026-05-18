; RUN: opt -passes="loop-vectorize,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-vectorize \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "remark_id"
; CHECK: "function_name":"test_vectorized"

define void @test_vectorized(ptr noalias %a, ptr noalias %b, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %gep_a = getelementptr i32, ptr %a, i32 %i
  %gep_b = getelementptr i32, ptr %b, i32 %i
  %v = load i32, ptr %gep_a
  store i32 %v, ptr %gep_b
  %i.next = add i32 %i, 1
  %cmp = icmp slt i32 %i.next, 256
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}
