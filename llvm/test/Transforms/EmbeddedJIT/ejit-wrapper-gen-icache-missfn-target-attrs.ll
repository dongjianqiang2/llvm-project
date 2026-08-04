; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -S %s | FileCheck %s
;
; Regression: the icache MissFn (A_miss) must inherit F's target-cpu /
; target-features. EJitWrapperGen creates MissFn with Function::Create, which
; only inherits module-default uwtable/frame-pointer -- NOT F's per-function
; target attributes. A bare MissFn then fails TTI::areInlineCompatible's
; subtarget feature-bit superset check, so InlinerPass rejects every
; cost-based inlining into MissFn with "conflicting attributes" (always_inline
; still inlines via the InlineCost.cpp AlwaysInline short-circuit), and the
; AOT fallback body loses ~all helper inlining vs the original function.
; The fix copies F's attributes onto MissFn so it matches F's subtarget.

define i32 @entry_tc(i32 %x) #0 !ejit.metadata !0 {
  ret i32 %x
}

; MissFn exists and uses attribute group [[MISS]].
; CHECK: define internal i32 @entry_tc_miss(i32 %0) #[[MISS:[0-9]+]]

; That same group carries the target-cpu/target-features copied from F
; (not the bare {noinline} that Function::Create would otherwise produce).
; CHECK: attributes #[[MISS]] = {{.*}}"target-cpu"="cortex-a57"{{.*}}"target-features"="+crc,+crypto"

attributes #0 = { noinline "target-cpu"="cortex-a57" "target-features"="+crc,+crypto" }
!0 = !{!{!"ejit_entry"}}
