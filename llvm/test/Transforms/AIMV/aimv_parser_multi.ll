; RUN: opt -passes="aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "remark_id":"CantReorderMemOps"
; CHECK: "remark_id":"LoopVectorized"
; CHECK: "function_name":"foo"
; CHECK: "function_name":"bar"

define void @foo(ptr %a, i32 %n) {
entry:
  ret void
}

define void @bar(ptr %a, i32 %n) {
entry:
  ret void
}

!aimv.diag = !{!0, !1}
!0 = !{!"LoopVectorize", !"CantReorderMemOps", !"foo", !"test.c:10:5", !"can't reorder", !2, !3, !4, !5}
!2 = !{i32 5, i32 8, i32 4, i32 1}
!3 = !{i32 1, !{!"Backward", !"store", !"load", !"MayAlias"}}
!4 = !{i32 2, i32 3, i32 0, i32 4, !"non-constant", i32 0, i32 -1}
!5 = !{!"loop1", i32 2, i32 10, i32 100, i32 1, i32 0}
!1 = !{!"LoopVectorize", !"LoopVectorized", !"bar", !"test.c:20:5", !"vectorized", !6, !7, !8, !9}
!6 = !{i32 5, i32 3, i32 4, i32 2}
!7 = !{i32 0}
!8 = !{i32 1, i32 1, i32 0, i32 8, !"stride=1", i32 0, i32 0}
!9 = !{!"loop2", i32 1, i32 8, i32 100, i32 0, i32 0}
