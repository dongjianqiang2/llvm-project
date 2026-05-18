; RUN: opt -passes="aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "source_accuracy":"approximate"

define void @test_func(ptr %a, i32 %n) {
entry:
  ret void
}

!aimv.diag = !{!0}
!0 = !{!"LoopVectorize", !"CantReorderMemOps", !"test_func", !"unknown", !"can't reorder", !1, !2, !3, !4}
!1 = !{i32 5, i32 8, i32 4, i32 1}
!2 = !{i32 0}
!3 = !{i32 1, i32 1, i32 0, i32 4, !"stride=1", i32 0, i32 0}
!4 = !{!"loop1", i32 1, i32 5, i32 100, i32 0, i32 0}
