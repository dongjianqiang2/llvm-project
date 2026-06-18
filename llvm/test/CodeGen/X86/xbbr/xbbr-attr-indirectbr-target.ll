; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s

; : blockaddress-taken blocks must be flagged IsIndirectBrTarget
; (bit 2 = 0x04). Both L1 and L2 are address-taken targets of indirectbr.

@addrs = global [2 x ptr] [ptr blockaddress(@indir, %L1), ptr blockaddress(@indir, %L2)]

define void @indir(i64 %i) {
entry:
  %p = getelementptr [2 x ptr], ptr @addrs, i64 0, i64 %i
  %t = load ptr, ptr %p
  indirectbr ptr %t, [label %L1, label %L2]
L1:
  ret void
L2:
  ret void
}

; ver=2, num=3, [Entry=0x0001, L1=IsIndirectBrTarget=0x0004, L2=IsIndirectBrTarget=0x0004]
; CHECK: 02030100 04000400
