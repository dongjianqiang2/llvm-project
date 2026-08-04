; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s
;
; An ejit_entry function carrying always_inline must not also receive
; noinline from EJitWrapperGen: the two are mutually exclusive and would
; abort the verifier ("Attributes 'noinline and alwaysinline' are
; incompatible!"). Sema rejects this combination at the source level
; (warn_ejit_always_inline_conflict drops always_inline); this test
; backstops hand-written IR by verifying the pass skips noinline when
; alwaysinline is present - the function keeps alwaysinline and noinline
; is added nowhere in the module.

define void @always_inline_entry() alwaysinline !ejit.metadata !0 {
entry:
  ret void
}

!0 = distinct !{!{!"ejit_entry"}}

; CHECK: define void @always_inline_entry() #[[A:[0-9]+]]
; CHECK: attributes #[[A]] = { {{.*}}alwaysinline{{.*}} }
; CHECK-NOT: noinline
