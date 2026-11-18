; RUN: opt -passes=ejit-register-bitcode -enable-ejit-global-ctors -S %s | FileCheck %s
;
; The section groups all direct callees (including invoke) before globals.
; The constructor keeps instruction order and registers only ordinary calls.
; Reuse the existing constructor without changing its linkage or its body.

@state = external global i32
declare extern_weak void @may_throw()
declare void @callee(i32)
declare i32 @personality(...)
declare void @existing()

define weak void @ejit_auto_register() {
  call void @existing()
  ret void
}

define internal void @entry() personality ptr @personality !ejit.metadata !0 {
  %v = load i32, ptr @state
  call void @callee(i32 %v)
  invoke void @may_throw() to label %done unwind label %unwind
done:
  ret void
unwind:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}

!0 = !{!{!"ejit_entry"}}

; CHECK: @.ejit.registry.bitcode = private constant
; CHECK-SAME: ptr @entry, i64 0
; CHECK-SAME: ptr @callee, i64 0
; CHECK-SAME: ptr @may_throw, i64 0
; CHECK-SAME: ptr @state, i64 0
; CHECK-SAME: section ".ejit_bitcode"
; CHECK: declare extern_weak void @may_throw()
; CHECK-LABEL: define weak void @ejit_auto_register()
; CHECK: call void @existing()
; CHECK-NEXT: call void @ejit_register_bitcode(
; CHECK-NEXT: call void @ejit_register_symbol({{.*}}ptr @entry)
; CHECK-NEXT: call void @ejit_register_symbol({{.*}}ptr @state)
; CHECK-NEXT: call void @ejit_register_symbol({{.*}}ptr @callee)
; CHECK-NEXT: ret void
