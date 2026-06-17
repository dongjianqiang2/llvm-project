; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s

; M1-T05 / PLAN §3.4 review fix: musttail must be detected via IR
; getTerminatingMustTailCall(), NOT MachineInstr::isReturn() (which would
; over-match plain RET / TCRETURN tail calls).
;
; Negative half: @plain_ret has a normal `ret`, NOT a musttail call —
; its byte must be 0x01 (IsEntry only), with bit 5 (0x20) cleared.

declare i32 @callee(i32)

define i32 @withmusttail(i32 %x) {
entry:
  %a = musttail call i32 @callee(i32 %x)
  ret i32 %a
}

define i32 @plain_ret(i32 %x) {
entry:
  ret i32 %x
}

; @withmusttail (3 bytes) immediately followed by @plain_ret (3 bytes):
;   01 01 21    01 01 01
; CHECK: 01012101 0101
