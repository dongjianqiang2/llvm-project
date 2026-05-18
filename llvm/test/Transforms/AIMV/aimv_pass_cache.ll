; RUN: opt -passes="aimv-feedback,aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK-COUNT-2: "pass_name":"LoopVectorize"

define void @test_func(ptr %a, i32 %n) {
entry:
  ret void
}

!aimv.diag = !{!0}
!0 = !{!"LoopVectorize", !"CantReorderMemOps", !"test_func", !"test.c:5:5", !"msg", !1, !2, !3, !4}
!1 = !{i32 5, i32 8, i32 4, i32 1}
!2 = !{i32 0}
!3 = !{i32 1, i32 1, i32 0, i32 4, !"stride=1", i32 0, i32 0}
!4 = !{!"loop1", i32 1, i32 5, i32 100, i32 0, i32 0}
