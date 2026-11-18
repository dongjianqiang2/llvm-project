; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s
; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s
;
; The generic slow path cannot forward unnamed arguments. Keep the AOT body.
define i32 @vararg_entry(i32 %n, ...) !ejit.metadata !0 {
entry:
  ret i32 %n
}

!0 = distinct !{!{!"ejit_entry"}}

; A variadic ejit_entry must be left as-is (no JIT wrapper inserted).
; CHECK-LABEL: define i32 @vararg_entry(i32 %n, ...)
; CHECK-NOT: jit_entry:
; CHECK-NOT: ejit_compile_or_get
