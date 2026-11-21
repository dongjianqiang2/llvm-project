; RUN: opt -passes=ejit-wrapper-gen,ejit-wrapper-gen -S %s | FileCheck %s
; RUN: opt -passes=ejit-aot-module,ejit-aot-module -S %s | FileCheck %s

; Idempotency coverage. PASS3 can be invoked more than once on the same module
; (e.g. EJitAotModulePass running in both an O1 and an O2 pipeline). The
; isAlreadyWrapped guard must stop the second run from re-wrapping an
; already-wrapped function — a second wrap would build PHI/branch edges against
; the previous run's jit_entry/jit_fallback blocks and corrupt the function.
; After two runs the function must contain EXACTLY ONE wrapper prologue.

define i32 @entry_twice(i8 %cell) !ejit.metadata !0 {
entry:
  %v = load i32, ptr @data
  ret i32 %v
}

@data = global i32 0, !ejit.metadata !1

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_group_indexed", !"cell", i32 16}}

; IDEMPOTENCY: after one or two PASS3 runs the function must contain EXACTLY ONE
; wrapper prologue. The unified wrapper uses the taskpool API.
; CHECK-LABEL: define i32 @entry_twice(i8 %cell)
; CHECK: jit_entry:
; CHECK: call i32 @ejit_taskpool_compile_or_get
; CHECK: jit_fallback:
; CHECK: load i32, ptr @data
; CHECK: jit_dispatch:
; CHECK: ret i32
; CHECK-NOT: jit_entry:
; CHECK-NOT: jit_dispatch:
