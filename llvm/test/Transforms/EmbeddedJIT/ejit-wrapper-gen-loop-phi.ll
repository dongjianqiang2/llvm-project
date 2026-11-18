; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s

; The loop header PHIs' entry-incoming edge must name jit_fallback after the
; original entry block is spliced.  The wrapper uses the unified taskpool API.

define i32 @loop_entry(i32 %n) !ejit.metadata !0 {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %acc.next = add i32 %acc, %i
  %i.next = add i32 %i, 1
  %c = icmp slt i32 %i.next, %n
  br i1 %c, label %loop, label %done
done:
  ret i32 %acc
}

!0 = distinct !{!{!"ejit_entry"}}

; CHECK-LABEL: define i32 @loop_entry(i32 %n)
; CHECK: jit_entry:
; CHECK: load i32, ptr @__ejit_funcidx_loop_entry
; CHECK: icmp ne
; CHECK: br {{.*}}label %jit_call{{.*}}label %jit_fallback

; The original blocks are ordered before jit_call (IRBuilder insertion).
; PHI edges that named %entry now name %jit_fallback.
; CHECK: %i = phi i32 [ 0, %jit_fallback ], [ %i.next, %loop ]
; CHECK: %acc = phi i32 [ 0, %jit_fallback ], [ %acc.next, %loop ]

; Wrapper blocks follow.
; CHECK: jit_call:
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr null, i32 0, ptr {{.*}}, ptr {{.*}})
; CHECK: jit_fallback:
; CHECK: br label %loop
; CHECK: jit_dispatch:
; CHECK: call i32 {{.*}}(i32 %n)
; CHECK: call void @ejit_taskpool_release_read
; CHECK: ret i32
