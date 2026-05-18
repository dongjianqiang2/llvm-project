; RUN: opt -passes="aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: not FileCheck %s --check-prefix=JSON < %t.json
; JSON-NOT: "pass_name"
;
; Module without !aimv.diag → no JSON output produced.

define void @empty_func(ptr %a, i32 %n) {
entry:
  ret void
}
