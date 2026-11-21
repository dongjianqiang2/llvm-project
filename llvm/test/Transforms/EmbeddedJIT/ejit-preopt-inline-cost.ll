; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S <%s >/dev/null
; RUN: opt -S %t-dump/*.bc | FileCheck %s
;
; The inliner inside preOptimizeBitcode must see CLEANED IR, not the raw
; frontend IR. On raw CodeGen IR every C local is an alloca + store/load
; pair, which inflates the inliner's computed callee cost ~4x (measured:
; cost=785 vs threshold=487 for this helper raw, no remark after cleanup),
; so an inline-hinted helper with several call sites that the AOT O2
; pipeline inlines stays a real call in the embedded bitcode - permanently,
; because the JIT pipeline runs no inliner of its own and relies on this
; AOT-time inlining (see EJitOptimizer::runPipeline).
;
; preOptimizeBitcode therefore runs a frontend-cleanup round (mirroring the
; host O2 EarlyFPM + GlobalCleanupPM: LowerExpect/SimplifyCFG/SROA/EarlyCSE/
; InstCombine) BEFORE AlwaysInliner + the O2 module inliner. This test locks
; that ordering in: the helper below is sized so that its raw-IR cost exceeds
; the inliner threshold while its post-cleanup cost does not.
;
; NOTE: preOptimizeBitcode is a no-op in debug builds (cyclic link
; dependency LLVMPasses <-> LLVMEmbeddedJIT); run EJIT lit tests on release
; builds (see CLAUDE.md).

@cell_data = global [16 x i32] zeroinitializer, !ejit.metadata !10

; helper: inlinehint + internal, mimics frontend IR (allocas + loads/stores)
define internal i32 @hint_helper(i32 %a, i32 %b) #0 {
entry:
  %x = alloca i32
  %y = alloca i32
  store i32 %a, ptr %x
  store i32 %b, ptr %y
  %la0 = load i32, ptr %x
  %lb0 = load i32, ptr %y
  %s0 = add i32 %la0, %lb0
  store i32 %s0, ptr %x
  %la1 = load i32, ptr %x
  %lb1 = load i32, ptr %y
  %s1 = mul i32 %la1, %lb1
  store i32 %s1, ptr %y
  %la2 = load i32, ptr %x
  %lb2 = load i32, ptr %y
  %s2 = xor i32 %la2, %lb2
  store i32 %s2, ptr %x
  %la3 = load i32, ptr %x
  %lb3 = load i32, ptr %y
  %s3 = and i32 %la3, %lb3
  store i32 %s3, ptr %y
  %la4 = load i32, ptr %x
  %lb4 = load i32, ptr %y
  %s4 = sub i32 %la4, %lb4
  store i32 %s4, ptr %x
  %la5 = load i32, ptr %x
  %lb5 = load i32, ptr %y
  %s5 = or i32 %la5, %lb5
  store i32 %s5, ptr %y
  %la6 = load i32, ptr %x
  %lb6 = load i32, ptr %y
  %s6 = add i32 %la6, %lb6
  store i32 %s6, ptr %x
  %la7 = load i32, ptr %x
  %lb7 = load i32, ptr %y
  %s7 = mul i32 %la7, %lb7
  store i32 %s7, ptr %y
  %la8 = load i32, ptr %x
  %lb8 = load i32, ptr %y
  %s8 = xor i32 %la8, %lb8
  store i32 %s8, ptr %x
  %la9 = load i32, ptr %x
  %lb9 = load i32, ptr %y
  %s9 = and i32 %la9, %lb9
  store i32 %s9, ptr %y
  %la10 = load i32, ptr %x
  %lb10 = load i32, ptr %y
  %s10 = sub i32 %la10, %lb10
  store i32 %s10, ptr %x
  %la11 = load i32, ptr %x
  %lb11 = load i32, ptr %y
  %s11 = or i32 %la11, %lb11
  store i32 %s11, ptr %y
  %la12 = load i32, ptr %x
  %lb12 = load i32, ptr %y
  %s12 = add i32 %la12, %lb12
  store i32 %s12, ptr %x
  %la13 = load i32, ptr %x
  %lb13 = load i32, ptr %y
  %s13 = mul i32 %la13, %lb13
  store i32 %s13, ptr %y
  %la14 = load i32, ptr %x
  %lb14 = load i32, ptr %y
  %s14 = xor i32 %la14, %lb14
  store i32 %s14, ptr %x
  %la15 = load i32, ptr %x
  %lb15 = load i32, ptr %y
  %s15 = and i32 %la15, %lb15
  store i32 %s15, ptr %y
  %la16 = load i32, ptr %x
  %lb16 = load i32, ptr %y
  %s16 = sub i32 %la16, %lb16
  store i32 %s16, ptr %x
  %la17 = load i32, ptr %x
  %lb17 = load i32, ptr %y
  %s17 = or i32 %la17, %lb17
  store i32 %s17, ptr %y
  %la18 = load i32, ptr %x
  %lb18 = load i32, ptr %y
  %s18 = add i32 %la18, %lb18
  store i32 %s18, ptr %x
  %la19 = load i32, ptr %x
  %lb19 = load i32, ptr %y
  %s19 = mul i32 %la19, %lb19
  store i32 %s19, ptr %y
  %la20 = load i32, ptr %x
  %lb20 = load i32, ptr %y
  %s20 = xor i32 %la20, %lb20
  store i32 %s20, ptr %x
  %la21 = load i32, ptr %x
  %lb21 = load i32, ptr %y
  %s21 = and i32 %la21, %lb21
  store i32 %s21, ptr %y
  %la22 = load i32, ptr %x
  %lb22 = load i32, ptr %y
  %s22 = sub i32 %la22, %lb22
  store i32 %s22, ptr %x
  %la23 = load i32, ptr %x
  %lb23 = load i32, ptr %y
  %s23 = or i32 %la23, %lb23
  store i32 %s23, ptr %y
  %la24 = load i32, ptr %x
  %lb24 = load i32, ptr %y
  %s24 = add i32 %la24, %lb24
  store i32 %s24, ptr %x
  %la25 = load i32, ptr %x
  %lb25 = load i32, ptr %y
  %s25 = mul i32 %la25, %lb25
  store i32 %s25, ptr %y
  %la26 = load i32, ptr %x
  %lb26 = load i32, ptr %y
  %s26 = xor i32 %la26, %lb26
  store i32 %s26, ptr %x
  %la27 = load i32, ptr %x
  %lb27 = load i32, ptr %y
  %s27 = and i32 %la27, %lb27
  store i32 %s27, ptr %y
  %la28 = load i32, ptr %x
  %lb28 = load i32, ptr %y
  %s28 = sub i32 %la28, %lb28
  store i32 %s28, ptr %x
  %la29 = load i32, ptr %x
  %lb29 = load i32, ptr %y
  %s29 = or i32 %la29, %lb29
  store i32 %s29, ptr %y
  %la30 = load i32, ptr %x
  %lb30 = load i32, ptr %y
  %s30 = add i32 %la30, %lb30
  store i32 %s30, ptr %x
  %la31 = load i32, ptr %x
  %lb31 = load i32, ptr %y
  %s31 = mul i32 %la31, %lb31
  store i32 %s31, ptr %y
  %la32 = load i32, ptr %x
  %lb32 = load i32, ptr %y
  %s32 = xor i32 %la32, %lb32
  store i32 %s32, ptr %x
  %la33 = load i32, ptr %x
  %lb33 = load i32, ptr %y
  %s33 = and i32 %la33, %lb33
  store i32 %s33, ptr %y
  %la34 = load i32, ptr %x
  %lb34 = load i32, ptr %y
  %s34 = sub i32 %la34, %lb34
  store i32 %s34, ptr %x
  %la35 = load i32, ptr %x
  %lb35 = load i32, ptr %y
  %s35 = or i32 %la35, %lb35
  store i32 %s35, ptr %y
  %la36 = load i32, ptr %x
  %lb36 = load i32, ptr %y
  %s36 = add i32 %la36, %lb36
  store i32 %s36, ptr %x
  %la37 = load i32, ptr %x
  %lb37 = load i32, ptr %y
  %s37 = mul i32 %la37, %lb37
  store i32 %s37, ptr %y
  %la38 = load i32, ptr %x
  %lb38 = load i32, ptr %y
  %s38 = xor i32 %la38, %lb38
  store i32 %s38, ptr %x
  %la39 = load i32, ptr %x
  %lb39 = load i32, ptr %y
  %s39 = and i32 %la39, %lb39
  store i32 %s39, ptr %y
  %r = load i32, ptr %x
  ret i32 %r
}

define i32 @entry(i32 %c) !ejit.metadata !20 {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.period_const !{}
  %r1 = call i32 @hint_helper(i32 %v, i32 %c)
  %r2 = call i32 @hint_helper(i32 %r1, i32 %v)
  %sum = add i32 %r1, %r2
  ret i32 %sum
}

attributes #0 = { inlinehint }

!10 = distinct !{!{!"ejit_group_indexed", !"cell", i32 16}, !{!"ejit_period_const_field", i32 0}}
!20 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}

; The two calls must have been inlined into the entry at AOT time: nothing
; between the entry's definition and its return references the helper, and
; the period_const load must survive the whole preopt round (reAnnotatePeriodConst
; is the safety net if any pass drops it).
; CHECK: define i32 @entry(
; CHECK-DAG: load i32, ptr {{.*}}, align 4, !ejit.period_const
; CHECK-NOT: @hint_helper
; CHECK: ret i32
