; RUN: opt < %s -passes=simplifycfg -hoist-common-insts=true -verify-each -S | FileCheck %s
; RUN: opt < %s -passes=simplifycfg -hoist-common-insts=true -simplifycfg-require-and-preserve-domtree=1 -verify-each -S | FileCheck %s

declare void @left(i64)
declare void @right(i64)

; CHECK-LABEL: define void @both_marked(
; CHECK: load i64, ptr %p, align 64, !ejit.may_const
; CHECK-NOT: load i64
; CHECK-NOT: !ejit.mayconst.audit.site
; CHECK-NOT: !unknown.test
; CHECK: ret void
define void @both_marked(i1 %cond, ptr %p) {
  br i1 %cond, label %a, label %b
a:
  %x = load i64, ptr %p, align 64, !ejit.may_const !0, !ejit.mayconst.audit.site !1, !unknown.test !0
  call void @left(i64 %x)
  ret void
b:
  %y = load i64, ptr %p, align 64, !ejit.may_const !0, !ejit.mayconst.audit.site !2, !unknown.test !0
  call void @right(i64 %y)
  ret void
}

; CHECK-LABEL: define void @left_only(
; CHECK: load i64, ptr %p, align 64{{$}}
; CHECK-NOT: !ejit.may_const
; CHECK: ret void
define void @left_only(i1 %cond, ptr %p) {
  br i1 %cond, label %a, label %b
a:
  %x = load i64, ptr %p, align 64, !ejit.may_const !0
  call void @left(i64 %x)
  ret void
b:
  %y = load i64, ptr %p, align 64
  call void @right(i64 %y)
  ret void
}

; CHECK-LABEL: define void @right_only(
; CHECK: load i64, ptr %p, align 64{{$}}
; CHECK-NOT: !ejit.may_const
; CHECK: ret void
define void @right_only(i1 %cond, ptr %p) {
  br i1 %cond, label %a, label %b
a:
  %x = load i64, ptr %p, align 64
  call void @left(i64 %x)
  ret void
b:
  %y = load i64, ptr %p, align 64, !ejit.may_const !0
  call void @right(i64 %y)
  ret void
}

!0 = !{}
!1 = !{i64 1}
!2 = !{i64 2}
