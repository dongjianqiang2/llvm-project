; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --bb-addr-map --pretty-pgo-analysis-map %t.o \
; RUN:   | FileCheck %s

; : a function with no profile metadata must not crash and must emit
; FuncEntryCount=0 (treated as cold by lld per SPEC §3.1).

define i32 @noprof(i32 %n) {
entry:
  %c = icmp slt i32 %n, 0
  br i1 %c, label %neg, label %pos
neg:
  ret i32 -1
pos:
  ret i32 1
}

; CHECK: Name: noprof
; CHECK: FuncEntryCount: 0
