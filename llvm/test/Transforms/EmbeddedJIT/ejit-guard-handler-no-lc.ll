; RUN: opt -passes=ejit-guard-handler -S %s | FileCheck %s

; Verify pass does nothing when no ejit_period_lc functions
; CHECK-NOT: ejit_deactivate
; CHECK-NOT: ejit_activate

define void @regular_func() {
  ret void
}
