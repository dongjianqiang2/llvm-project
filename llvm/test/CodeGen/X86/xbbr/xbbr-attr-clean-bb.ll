; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s

; M1-T05-C9 (negative): an unremarkable BB — no EH, no indirect branch,
; no setjmp/longjmp, no musttail, no inline asm with section directives,
; no noreturn tail, not user-blacklisted, not (yet) cold — must produce
; an attr word with ALL bits clear except IsEntry on the entry block.
; This is the regression guard against accidentally over-flagging.

define i32 @plain_arith(i32 %a, i32 %b) {
entry:
  %s = add i32 %a, %b
  %p = mul i32 %s, %s
  ret i32 %p
}

; 1 MBB: 0x0001 (IsEntry only — bits 1..15 must be zero).
; CHECK: 02010100
