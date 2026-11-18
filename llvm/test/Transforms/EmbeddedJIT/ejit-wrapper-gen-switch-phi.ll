; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s
; RUN: opt -ejit-inline-cache=false -passes='default<O2>' -S %s -o /dev/null

; PHI-incoming-block regression coverage with a multi-successor terminator.
; The original fix used replaceSuccessorsPhiUsesWith, which must walk EVERY
; successor of the entry block — not just the two targets of a conditional
; branch. A switch in the entry block fans out to several successors, and each
; one (plus the common merge block) may carry a PHI listing %entry as an
; incoming predecessor. All such edges must be rewritten to jit_fallback.

define i32 @switch_entry(i32 %sel) !ejit.metadata !0 {
entry:
  switch i32 %sel, label %def [ i32 0, label %a
                                i32 1, label %b ]
a:
  %pa = phi i32 [ 10, %entry ]
  br label %def
b:
  %pb = phi i32 [ 20, %entry ]
  br label %def
def:
  %r = phi i32 [ 0, %entry ], [ %pa, %a ], [ %pb, %b ]
  ret i32 %r
}

!0 = distinct !{!{!"ejit_entry"}}

; CHECK-LABEL: define i32 @switch_entry(i32 %sel)
; CHECK: jit_entry:
; Every PHI incoming edge that referenced the erased entry block — in the
; direct switch successors AND in the shared merge block — must now name
; jit_fallback. (The original switch lands in jit_fallback.)
; CHECK: %pa = phi i32 [ 10, %jit_fallback ]
; CHECK: %pb = phi i32 [ 20, %jit_fallback ]
; CHECK: %r = phi i32 [ 0, %jit_fallback ], [ %pa, %a ], [ %pb, %b ]
; CHECK: jit_call:
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr null, i32 0, ptr {{.*}}, ptr {{.*}})
; CHECK: jit_fallback:
; CHECK: switch i32 %sel
; CHECK: jit_dispatch:
; CHECK: call i32 {{.*}}(i32 %sel)
; CHECK: call void @ejit_taskpool_release_read
