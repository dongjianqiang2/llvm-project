; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes=ejit-wrapper-gen -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=JIT
; RUN: opt -passes=ejit-wrapper-gen -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=AOT
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=ICACHE
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-wrapper-timing -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=BOTH

; OFF-NOT: @ejit_function_body_cycles_record

; The JIT body interval starts after lookup/dispatch and ends immediately after
; the selected function call. The wrapper interval starts before dispatch and
; ends after read-token release; aggregation is outside both intervals.
; JIT-LABEL: define i32 @timed_body(
; JIT: %ejit_wrapper_begin = call i64 @ejit_taskpool_trace_now()
; JIT-LABEL: jit_dispatch:
; JIT: %ejit_jit_body_begin = call i64 @ejit_taskpool_trace_now()
; JIT-NEXT: {{.*}}call i32 %ejit_fn
; JIT-NEXT: %ejit_jit_body_end = call i64 @ejit_taskpool_trace_now()
; JIT: call void @ejit_taskpool_release_read
; JIT-NEXT: %ejit_wrapper_end = call i64 @ejit_taskpool_trace_now()
; JIT-NEXT: call void @ejit_function_body_cycles_record({{.*}}i32 1, i64 %ejit_wrapper_begin, i64 %ejit_jit_body_begin, i64 %ejit_jit_body_end, i64 %ejit_wrapper_end)

; The original AOT CFG is bracketed independently. Both return sites report an
; AOT sample, including fallback caused by an invalid funcIndex or cache miss.
; AOT-LABEL: aot.positive:
; AOT: %ejit_aot_body_end{{.*}} = call i64 @ejit_taskpool_trace_now()
; AOT-NEXT: %ejit_wrapper_end{{.*}} = call i64 @ejit_taskpool_trace_now()
; AOT-NEXT: call void @ejit_function_body_cycles_record({{.*}}i32 0, i64 %ejit_wrapper_begin, i64 %ejit_aot_body_begin, i64 %ejit_aot_body_end{{.*}}, i64 %ejit_wrapper_end{{.*}})
; AOT-NEXT: ret i32
; AOT-LABEL: aot.nonpositive:
; AOT: %ejit_aot_body_end{{.*}} = call i64 @ejit_taskpool_trace_now()
; AOT-NEXT: %ejit_wrapper_end{{.*}} = call i64 @ejit_taskpool_trace_now()
; AOT-NEXT: call void @ejit_function_body_cycles_record({{.*}}i32 0, i64 %ejit_wrapper_begin, i64 %ejit_aot_body_begin, i64 %ejit_aot_body_end{{.*}}, i64 %ejit_wrapper_end{{.*}})
; AOT-NEXT: ret i32
; AOT-LABEL: jit_fallback:
; AOT: %ejit_aot_body_begin = call i64 @ejit_taskpool_trace_now()

; Inline-cache JIT hits use the same JIT path label and are no longer musttail
; while instrumentation is enabled, because the end timestamp follows the call.
; ICACHE-LABEL: define i32 @timed_body(
; ICACHE: %ejit_wrapper_begin = call i64 @ejit_taskpool_trace_now()
; ICACHE-LABEL: jit_icache_dispatch:
; ICACHE: %ejit_jit_body_begin = call i64 @ejit_taskpool_trace_now()
; ICACHE-NOT: musttail
; ICACHE-NEXT: {{.*}}call i32 %ejit_ic_fn
; ICACHE-NEXT: %ejit_jit_body_end = call i64 @ejit_taskpool_trace_now()
; ICACHE-NEXT: %ejit_wrapper_end = call i64 @ejit_taskpool_trace_now()
; ICACHE-NEXT: call void @ejit_function_body_cycles_record({{.*}}i32 1, i64 %ejit_wrapper_begin, i64 %ejit_jit_body_begin, i64 %ejit_jit_body_end, i64 %ejit_wrapper_end)
; ICACHE-NEXT: ret i32
; The miss helper receives the outer wrapper timestamp, so its AOT/JIT samples
; include the failed inline-cache probe rather than starting late.
; ICACHE: call i32 @timed_body_miss(i32 %cell, i32 %value, i64 %ejit_wrapper_begin)
; ICACHE-LABEL: define internal i32 @timed_body_miss(
; ICACHE-SAME: i64 %[[WRAPPER_BEGIN:[0-9]+]])
; ICACHE-LABEL: miss_fallback:
; ICACHE: %ejit_aot_body_begin = call i64 @ejit_taskpool_trace_now()
; ICACHE: call void @ejit_function_body_cycles_record({{.*}}i32 0, i64 %[[WRAPPER_BEGIN]], i64 %ejit_aot_body_begin,

; Wrapper and body timing coexist for multiple entries, including void returns.
; BOTH-LABEL: define i32 @timed_body(
; BOTH: call void @ejit_taskpool_trace_wrapper(
; BOTH: call void @ejit_function_body_cycles_record(
; BOTH-LABEL: define void @timed_void(
; BOTH-LABEL: jit_icache_dispatch:
; BOTH: call void %ejit_ic_fn
; BOTH: call void @ejit_taskpool_trace_wrapper(
; BOTH: call void @ejit_function_body_cycles_record(
; BOTH-NEXT: ret void

define i32 @timed_body(i32 %cell, i32 %value) !ejit.metadata !0 {
entry:
  %positive = icmp sgt i32 %value, 0
  br i1 %positive, label %aot.positive, label %aot.nonpositive

aot.positive:
  %sum = add i32 %value, 7
  ret i32 %sum

aot.nonpositive:
  %neg = sub i32 0, %value
  ret i32 %neg
}

define void @timed_void(i32 %cell, ptr %out) !ejit.metadata !1 {
entry:
  store i32 %cell, ptr %out
  ret void
}

@data = global [16 x i32] zeroinitializer, !ejit.metadata !10

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
!10 = distinct !{!{!"ejit_group_indexed", !"cell", i32 16}}
