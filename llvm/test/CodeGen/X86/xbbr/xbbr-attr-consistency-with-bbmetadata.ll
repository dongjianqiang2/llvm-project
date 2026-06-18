; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --bb-addr-map %t.o | FileCheck %s --check-prefix=BBADDR
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s --check-prefix=ATTR

; : when both .llvm_xbbr_attr and BB_ADDR_MAP describe the
; same BB, the overlapping bits must agree. This is the lld Stage 0
; consistency assertion's first line of defense — caught at compile
; time, before the linker ever sees the .o.
;
; Specifically:
;   BBEntry::Metadata::IsEHPad  ⇔  XBBRAttr.IsLandingPad (bit 1)
;
; Function:
;   entry                  → IsEntry             = 0x0001
;   cont                   → (clean)             = 0x0000
;   lpad (the landing pad) → IsLandingPad        = 0x0002

declare void @maythrow()
declare i32 @__gxx_personality_v0(...)

define i32 @withlpad() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @maythrow() to label %cont unwind label %lpad
cont:
  ret i32 0
lpad:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
}

; BB_ADDR_MAP must mark the lpad BB as IsEHPad.
; BBADDR: Name: withlpad
; BBADDR: IsEHPad: Yes

; Corresponding XBBR attr: 3 BBs, lpad must have IsLandingPad (0x0002).
; (Block emission order may differ at -O2 — accept any of the three
;  attr words at the lpad slot; `02 03` is the version+num_bbs prefix.)
; ATTR: 0203
; ATTR-DAG: 0200
