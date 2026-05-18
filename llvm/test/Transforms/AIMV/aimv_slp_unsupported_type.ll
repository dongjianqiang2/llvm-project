; RUN: opt -passes="slp-vectorizer,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=slp-vectorizer \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "pass_name":"SLPVectorize"
;
; SLP with unsupported vector type (e.g. i48) should trigger UnsupportedType diagnostic.

define void @test_slp_unsupported(ptr %a, ptr %b) {
  %v1 = load i48, ptr %a
  %v2 = load i48, ptr %b
  %add = add i48 %v1, %v2
  store i48 %add, ptr %a
  ret void
}
