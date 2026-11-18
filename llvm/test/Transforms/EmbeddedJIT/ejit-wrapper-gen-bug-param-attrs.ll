; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s
; RUN: opt -passes=ejit-wrapper-gen -verify-each -disable-output %s
;
; Dispatch must preserve the entry calling convention and parameter/return ABI.
%S = type { i32, i32 }
%R = type { i64, i64 }

; --- byval struct argument must stay byval at the dispatch call ---
; CHECK-LABEL: define void @byval_entry(ptr byval(%S) %p, i8 %cell)
; CHECK: jit_dispatch:
; CHECK: call void {{.*}}(ptr byval(%S) %p, i8 %cell)
define void @byval_entry(ptr byval(%S) %p, i8 %cell) !ejit.metadata !1 {
entry:
  ret void
}

; --- sret pointer must stay sret at the dispatch call ---
; CHECK-LABEL: define void @sret_entry(ptr sret(%R) %out, i8 %cell)
; CHECK: jit_dispatch:
; CHECK: call void {{.*}}(ptr sret(%R) %out, i8 %cell)
define void @sret_entry(ptr sret(%R) %out, i8 %cell) !ejit.metadata !1 {
entry:
  ret void
}

; --- signext on the return value and on a sub-word argument must be preserved ---
; CHECK-LABEL: define signext i16 @signext_entry(i16 signext %a)
; CHECK: jit_dispatch:
; CHECK: call signext i16 {{.*}}(i16 signext %a)
define signext i16 @signext_entry(i16 signext %a) !ejit.metadata !0 {
entry:
  ret i16 %a
}

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 1}}
