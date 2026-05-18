; RUN: opt -passes="aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "function_name":"func_a"
; CHECK: "function_name":"func_b"
; CHECK: "remark_id":"CantReorderMemOps"
; CHECK: "remark_id":"UnsafeDep"

define void @func_a(ptr %a, i32 %n) {
entry:
  ret void
}

define void @func_b(ptr %a, i32 %n) {
entry:
  ret void
}

!aimv.diag = !{!0, !1}
!0 = !{!"LoopVectorize", !"CantReorderMemOps", !"func_a", !"test.c:5:5", !"msg", !2, !3, !4, !5}
!1 = !{!"LoopVectorize", !"UnsafeDep", !"func_b", !"test.c:10:5", !"msg", !2, !3, !4, !5}
!2 = !{i32 5, i32 8, i32 4, i32 1}
!3 = !{i32 0}
!4 = !{i32 1, i32 1, i32 0, i32 4, !"stride=1", i32 0, i32 0}
!5 = !{!"loop1", i32 1, i32 5, i32 100, i32 0, i32 0}
