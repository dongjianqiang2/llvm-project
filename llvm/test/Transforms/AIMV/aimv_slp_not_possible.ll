; RUN: opt -passes="slp-vectorizer,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=slp-vectorizer \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "pass_name":"SLPVectorize"
;
; Non-consecutive memory accesses should make SLP not possible.

define void @test_slp_not_possible(ptr %a, ptr %b, ptr %c) {
  %v1 = load i32, ptr %a
  %v2 = load i32, ptr %b
  %v3 = load i32, ptr %c
  %add1 = add i32 %v1, %v2
  %add2 = add i32 %v1, %v3
  store i32 %add1, ptr %a
  store i32 %add2, ptr %b
  ret void
}
