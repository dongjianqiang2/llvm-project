; REQUIRES: x86-registered-target
; RUN: opt -passes=cg-profile -S %s -o %t.ll
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %t.ll \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --cg-profile %t.o | FileCheck %s

; M1-T04 integration: indirect-call edges (IRPGO VP) must survive the
; full IR → CG Profile module flag → MC → SHT_LLVM_CALL_GRAPH_PROFILE
; pipeline when XBBR is enabled. Without this test, an MC-side change
; that filters ELF call-graph-profile entries would silently corrupt
; the linker input.

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
!1 = !{!"VP", i32 0, i64 1600,
       i64 7651369219802541373, i64 1030,
       i64 -4377547752858689819, i64 410,
       i64 -6929281286627296573, i64 150,
       i64 -2545542355363006406, i64 10}

; CHECK: CGProfile [
; CHECK-DAG: From: bar
; CHECK-DAG: From: bar
; CHECK-DAG: From: bar
; CHECK-DAG: From: bar
; CHECK-DAG: To: func4
; CHECK-DAG: To: func2
; CHECK-DAG: To: func3
; CHECK-DAG: To: func1
; CHECK-DAG: Weight: 1030
; CHECK-DAG: Weight: 410
; CHECK-DAG: Weight: 150
; CHECK-DAG: Weight: 10
; CHECK: ]
