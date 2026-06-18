; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --bb-addr-map --pretty-pgo-analysis-map %t.o \
; RUN:   | FileCheck %s

; : recursive functions must report entry_count as the measured total
; call count (NOT geometric amplification — the PLAN §3.2 fix to the
; "recursion blows up frequency" review concern). lld will use entry_count
; (not Σ global_freq over BBs) for cluster density (PLAN §4.3 Stage 1).

define i32 @recfib(i32 %n) !prof !0 {
entry:
  %c = icmp slt i32 %n, 2
  br i1 %c, label %base, label %rec, !prof !1
base:
  ret i32 1
rec:
  %s1 = sub i32 %n, 1
  %a = call i32 @recfib(i32 %s1)
  %s2 = sub i32 %n, 2
  %b = call i32 @recfib(i32 %s2)
  %add = add i32 %a, %b
  ret i32 %add
}

!0 = !{!"function_entry_count", i64 100}
!1 = !{!"branch_weights", i32 50, i32 50}

; The function is invoked 100 times by the harness; recursion does not
; amplify FuncEntryCount further. Per-BB BBFreq is the conditional
; probability under one entry, summing to ~entry weight.
; CHECK: Name: recfib
; CHECK: FuncEntryCount: 100
