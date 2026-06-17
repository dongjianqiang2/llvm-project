; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --bb-addr-map --pretty-pgo-analysis-map %t.o \
; RUN:   | FileCheck %s

; M1-T03: -enable-xbbr must implicitly turn on SHT_LLVM_BB_ADDR_MAP with
; FuncEntryCount + BBFreq + BrProb features (PLAN §3.2). The frontend's
; profile metadata (entry_count + branch_weights) is propagated to lld
; via this section, where global_freq(BB) = BBFreq × FuncEntryCount.

; Function entry count = 1000; branch weights 100:900 (BB1 cold, BB2 hot).

define i32 @hot(i32 %n) !prof !0 {
entry:
  %c = icmp slt i32 %n, 0
  br i1 %c, label %neg, label %pos, !prof !1
neg:
  ret i32 -1
pos:
  ret i32 1
}

!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 100, i32 900}

; CHECK: BBAddrMap [
; CHECK:   Function {
; CHECK:     Name: hot
; CHECK:     PGO analyses {
; CHECK:       FuncEntryCount: 1000
; CHECK:       PGO BB entries [
; CHECK:         {
; CHECK:           Frequency: 1.0
; CHECK:           Successors [
; CHECK:             {
; CHECK:               Probability: {{.*}} = 10.00%
; CHECK:             }
; CHECK:             {
; CHECK:               Probability: {{.*}} = 90.00%
; CHECK:             }
; CHECK:           ]
; CHECK:         }
