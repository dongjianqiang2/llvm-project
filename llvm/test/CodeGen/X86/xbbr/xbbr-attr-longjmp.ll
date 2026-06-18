; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s

; longjmp recognition: glibc longjmp has only `noreturn`, not
; `returns_twice`. This test pins down that the longjmp side is caught
; by callee-name match (separately from the setjmp side, which is
; recognized via the returns_twice attribute). PLAN §3.4 documents why
; name-match is the only sound signal — `noreturn` would over-match
; abort/exit.
;
; The `tail` attribute is added explicitly so the IR has the canonical
; shape; XBBR detection is unaffected by it.

declare void @longjmp(ptr, i32) noreturn

define void @uses_longjmp(ptr %buf) {
entry:
  call void @longjmp(ptr %buf, i32 1)
  unreachable
}

; 1 MBB:
;   IsEntry            = 0x0001
;   HasSetjmp (longjmp matched by name) = 0x0008
;   IsNoReturnTail (succ_empty + noreturn callsite + unreachable) = 0x0100
;   total = 0x0109
; CHECK: 02010901
