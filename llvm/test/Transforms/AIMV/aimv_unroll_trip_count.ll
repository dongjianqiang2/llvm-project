; RUN: opt -passes="loop-unroll,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-unroll \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "pass_name":"LoopUnroll"
;
; Loop with unknown trip count (variable bound) should trigger unroll diagnostic.

define void @test_unroll_trip(ptr %a, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %gep = getelementptr i32, ptr %a, i32 %i
  store i32 0, ptr %gep
  %i.next = add i32 %i, 1
  %cmp = icmp slt i32 %i.next, %n
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}
