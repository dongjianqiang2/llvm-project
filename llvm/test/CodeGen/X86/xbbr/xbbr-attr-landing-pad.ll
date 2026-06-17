; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s

; M1-T05 + M1-T03: a function with a normal entry/cont path and an EH
; landing pad must produce attrs:
;   entry:  IsEntry        = 0x01
;   cont:   (none)         = 0x00
;   lpad:   IsLandingPad   = 0x02   (mirrors BBEntry::Metadata::IsEHPad)
;
; Stage 0 in lld will sanity-check that this bit matches BBEntry::IsEHPad
; in the same .o; M1-T05 ensures both originate from MBB.isEHPad().

declare void @maythrow()
declare i32 @__gxx_personality_v0(...)

define i32 @haseh() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @maythrow() to label %cont unwind label %lpad
cont:
  ret i32 0
lpad:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
}

; ver=1, num=3, [Entry=0x01, cont=0x00, lpad=0x02]
; CHECK: 01030100 02
