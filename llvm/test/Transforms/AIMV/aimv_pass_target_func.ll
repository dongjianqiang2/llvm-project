; RUN: opt -passes="aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json -aimv-enable -aimv-target-function=foo 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "function_name":"foo"
; CHECK-NOT: "function_name":"bar"

define void @foo(ptr %a, i32 %n) {
entry:
  ret void
}

define void @bar(ptr %a, i32 %n) {
entry:
  ret void
}

!aimv.diag = !{!0, !1}
!0 = !{!"LoopVectorize", !"CantReorderMemOps", !"foo", !"test.c:5:5", !"msg", !2, !3, !4, !5}
!1 = !{!"LoopVectorize", !"CantReorderMemOps", !"bar", !"test.c:10:5", !"msg", !2, !3, !4, !5}
!2 = !{i32 5, i32 8, i32 4, i32 1}
!3 = !{i32 0}
!4 = !{i32 1, i32 1, i32 0, i32 4, !"stride=1", i32 0, i32 0}
!5 = !{!"loop1", i32 1, i32 5, i32 100, i32 0, i32 0}
