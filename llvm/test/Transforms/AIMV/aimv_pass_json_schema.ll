; RUN: opt -passes="aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "pass_name":"LoopVectorize"
; CHECK: "remark_id":"CantReorderMemOps"
; CHECK: "remark_text":"can't reorder mem ops"
; CHECK: "severity":"missed"
; CHECK: "function_name":"test_func"
; CHECK: "loop_location":"test.c:5:5"
; CHECK: "source_context"
; CHECK: "ir_snippet"
; CHECK: "source_accuracy"
; CHECK: "cost_model"
; CHECK: "scalar_cost":5
; CHECK: "vector_cost":8
; CHECK: "vf":4
; CHECK: "interleave_count":1
; CHECK: "dependencies"
; CHECK: "dep_type":"Backward"
; CHECK: "source_ptr":"store"
; CHECK: "sink_ptr":"load"
; CHECK: "alias_result":"MayAlias"
; CHECK: "memory_info"
; CHECK: "num_stores":2
; CHECK: "num_loads":3
; CHECK: "max_alignment":4
; CHECK: "stride":"non-constant"
; CHECK: "memory_check_count":0
; CHECK: "memory_check_cost":-1
; CHECK: "loop_info"
; CHECK: "num_blocks":2
; CHECK: "num_instructions":10
; CHECK: "trip_count":100
; CHECK: "num_branches":1
; CHECK: "num_calls":0
; CHECK: "target"
; CHECK: "triple"
; CHECK: "cpu"
; CHECK: "features"
; CHECK: "vector_width"

define void @test_func(ptr %a, i32 %n) {
entry:
  ret void
}

!aimv.diag = !{!0}
!0 = !{!"LoopVectorize", !"CantReorderMemOps", !"test_func", !"test.c:5:5", !"can't reorder mem ops", !1, !2, !3, !4}
!1 = !{i32 5, i32 8, i32 4, i32 1}
!2 = !{i32 1, !{!"Backward", !"store", !"load", !"MayAlias"}}
!3 = !{i32 2, i32 3, i32 0, i32 4, !"non-constant", i32 0, i32 -1}
!4 = !{!"loop1", i32 2, i32 10, i32 100, i32 1, i32 0}
