; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s
; RUN: opt -passes=ejit-wrapper-gen -verify-each -disable-output %s
;
; Dispatch must preserve the entry calling convention and parameter/return ABI.
define fastcc i32 @fastcc_entry(i32 %a) !ejit.metadata !0 {
entry:
  ret i32 %a
}

!0 = distinct !{!{!"ejit_entry"}}

; The dispatch call must use the same calling convention as the function.
; CHECK-LABEL: define fastcc i32 @fastcc_entry(i32 %a)
; CHECK: jit_dispatch:
; CHECK: call fastcc i32 {{.*}}(i32 %a)
