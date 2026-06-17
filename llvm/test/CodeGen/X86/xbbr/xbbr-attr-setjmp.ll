; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s

; M1-T05: a basic block containing a returns_twice call (e.g. setjmp) must
; be flagged HasSetjmp (bit 3 = 0x08), in addition to IsEntry on the entry.

declare i32 @setjmp(ptr) returns_twice

define i32 @hassetjmp(ptr %buf) {
entry:
  %r = call i32 @setjmp(ptr %buf)
  ret i32 %r
}

; ver=1, num=1, [Entry|HasSetjmp = 0x09]
; CHECK: 010109
