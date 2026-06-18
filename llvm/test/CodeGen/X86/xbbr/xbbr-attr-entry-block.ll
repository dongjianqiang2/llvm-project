; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readelf -SW %t.o | FileCheck %s --check-prefix=SECTION
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s --check-prefix=BYTES

; : a single-block function should produce exactly one xbbr_attr byte
; with IsEntry (0x01) set and nothing else.

define i32 @entry_only() {
  ret i32 0
}

; Section flags must include LE = SHF_LINK_ORDER + SHF_EXCLUDE so the
; attrs are dropped at link time (PLAN §9.3).
; SECTION: .llvm_xbbr_attr{{.*}}LLVM_XBBR_ATTR{{.*}}LE

; Format: u8 version=2, uleb128 num_bbs=1, u16 attrs[num_bbs] = {0x0001}
; (version 0x02 = u16 attrs widened to fit IsNoReturnTail = bit 8).
; BYTES: 02010100
