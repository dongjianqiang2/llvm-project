; RUN: opt -passes="loop-vectorize,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-vectorize \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s --check-prefix=JSON < %t.json
; JSON: "pass_name": "LoopVectorize"
; JSON: "severity": "missed"
; JSON: "cost_model"

define void @test_cost_reject(ptr %a, ptr %b, i32 %n) {
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
