; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=TIMED
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=AOTSAFE
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=MIXED

; Moving an AOT body with a musttail exit into an augmented MissFn would make
; the caller and callee parameter counts differ. Keep MissFn ABI-identical,
; preserve every original musttail, and time only JIT execution for this entry.
;
; OFF (no timing flags): NumDims=1 and no timing -> the SENTINEL form. The
; table is defined pre-filled with &MissFn and the wrapper is ONE block
; (load + musttail BLR, no guard/miss blocks); musttail survives on the hit
; path exactly as it did in the guarded form.
; OFF: @__ejit_icache_fn_musttail_entry = internal global [16 x ptr] [ptr @musttail_entry_miss, {{.*}}]
; OFF-LABEL: define i32 @musttail_entry(
; OFF-NOT: br
; OFF: musttail call i32 %ejit_ic_fn
; OFF-LABEL: define internal i32 @musttail_entry_miss(i32 %0)
; OFF: musttail call i32 @helper(i32 %0)
;
; TIMED-LABEL: define i32 @musttail_entry(
; TIMED: %ejit_wrapper_begin = call i64 @ejit_taskpool_trace_now()
; TIMED-LABEL: jit_miss:
; TIMED: call i32 @musttail_entry_miss(i32 %cell)
; TIMED-LABEL: define internal i32 @musttail_entry_miss(i32 %0)
; TIMED-LABEL: miss_entry:
; TIMED: %ejit_wrapper_begin = call i64 @ejit_taskpool_trace_now()
; TIMED-LABEL: miss_dispatch:
; TIMED: call void @ejit_function_body_cycles_record({{.*}}i32 1
;
; AOTSAFE-LABEL: define internal i32 @musttail_entry_miss(i32 %0)
; AOTSAFE-LABEL: miss_fallback:
; AOTSAFE-NOT: call void @ejit_function_body_cycles_record
; AOTSAFE: musttail call i32 @helper(i32 %0)

; A mixed normal/musttail AOT CFG also suppresses all AOT samples rather than
; reporting a biased subset of its exits.
;
; MIXED-LABEL: define internal i32 @mixed_entry_miss(i32 %0, i32 %1)
; MIXED-LABEL: miss_fallback:
; MIXED-NOT: ejit_aot_body_begin
; MIXED-NOT: call void @ejit_function_body_cycles_record({{.*}}i32 0
; MIXED: mixed.normal:
; MIXED: ret i32 %1
; MIXED: mixed.tail:
; MIXED: musttail call i32 @mixed_helper(i32 %0, i32 %1)

define i32 @helper(i32 %cell) {
entry:
  ret i32 %cell
}

define i32 @mixed_helper(i32 %cell, i32 %value) {
entry:
  %sum = add i32 %cell, %value
  ret i32 %sum
}

define i32 @musttail_entry(i32 %cell) !ejit.metadata !0 {
entry:
  %r = musttail call i32 @helper(i32 %cell)
  ret i32 %r
}

define i32 @mixed_entry(i32 %cell, i32 %value) !ejit.metadata !1 {
entry:
  %take.tail = icmp eq i32 %value, 0
  br i1 %take.tail, label %mixed.tail, label %mixed.normal

mixed.normal:
  ret i32 %value

mixed.tail:
  %r = musttail call i32 @mixed_helper(i32 %cell, i32 %value)
  ret i32 %r
}

@data = global [16 x i32] zeroinitializer, !ejit.metadata !10

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
!10 = distinct !{!{!"ejit_group_indexed", !"cell", i32 16}}
