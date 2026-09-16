; REQUIRES: asserts
; RUN: not opt -passes=ejit-register-bitcode -ejit-force-inline-hinted-helpers -disable-output %s 2>&1 | FileCheck %s

; CHECK: -ejit-force-inline-hinted-helpers requires an NDEBUG compiler build

define i32 @root(i32 %x) !ejit.metadata !0 {
  ret i32 %x
}

define internal i32 @hint(i32 %x) #0 {
  ret i32 %x
}

attributes #0 = { inlinehint }
!0 = distinct !{!{!"ejit_entry"}}
