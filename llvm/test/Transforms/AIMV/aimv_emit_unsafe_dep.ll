; RUN: opt -passes="loop-vectorize,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-vectorize \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "remark_id":"UnsafeDep"
; CHECK: "severity":"missed"

define void @test_unsafe_dep(ptr %a, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %gep_cur = getelementptr i32, ptr %a, i32 %i
  %i.next = add i32 %i, 1
  %gep_next = getelementptr i32, ptr %a, i32 %i.next
  %v = load i32, ptr %gep_cur
  store i32 %v, ptr %gep_next
  %cmp = icmp slt i32 %i.next, 100
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}
