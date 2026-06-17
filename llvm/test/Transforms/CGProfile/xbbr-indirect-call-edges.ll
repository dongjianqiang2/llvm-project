; RUN: opt -passes=cg-profile -S %s | FileCheck %s

; M1-T04 / TASK §M1-T04 / PLAN §3.3: indirect-call edges harvested from
; IRPGO IPVK_IndirectCallTarget VP data must reach the CG Profile module
; flag — XBBR Stage 1 hfsort+ then consumes them for cluster density.
;
; This test pins down the existing LLVM CGProfile pass behaviour so a
; future refactor that drops VP-derived edges (e.g. a partial rewrite of
; CGProfile.cpp:81-87) breaks here, before it can silently strip C++
; vtable / function-pointer hot edges from the linker input.
;
; The VP `i32 0` kind constant is `IPVK_IndirectCallTarget`; the i64
; values are PGO-name MD5 hashes for func1..func4 (taken from
; PGOProfile/indirect_call_promotion.ll, which uses the same scheme).

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@foo = common global ptr null, align 8

define i32 @func1() { ret i32 0 }
define i32 @func2() { ret i32 1 }
define i32 @func3() { ret i32 2 }
define i32 @func4() { ret i32 3 }

define i32 @bar() !prof !0 {
entry:
  %tmp = load ptr, ptr @foo, align 8
  %call = call i32 %tmp(), !prof !1
  ret i32 %call
}

!0 = !{!"function_entry_count", i64 1600}
; VP kind 0 = IPVK_IndirectCallTarget. Total=1600, callee/count pairs:
;   func4 hash, 1030
;   func2 hash, 410
;   func3 hash, 150
;   func1 hash, 10
!1 = !{!"VP", i32 0, i64 1600,
       i64 7651369219802541373, i64 1030,
       i64 -4377547752858689819, i64 410,
       i64 -6929281286627296573, i64 150,
       i64 -2545542355363006406, i64 10}

; All four indirect callees must appear as CG Profile edges from @bar.
; CHECK: !"CG Profile"
; CHECK-DAG: !{ptr @bar, ptr @func4, i64 1030}
; CHECK-DAG: !{ptr @bar, ptr @func2, i64 410}
; CHECK-DAG: !{ptr @bar, ptr @func3, i64 150}
; CHECK-DAG: !{ptr @bar, ptr @func1, i64 10}
