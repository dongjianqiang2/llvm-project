; REQUIRES: x86-registered-target
;
; XBBR IsCold attribute bit (SPEC §6.1): with IRPGO profile present and
; -xbbr-cold-threshold=0.5, the cold branch's BB must get bit 7 (IsCold)
; in `.llvm_xbbr_attr`; with default threshold 0.01 (1%) the same branch
; (10% of entry frequency) is still warm — bit 7 stays 0.
;
; Function entry count = 1000; branch weights 100:900 → BB1 (neg) is
; 10% of entry → cold under threshold=0.5 (10% < 50%); warm under
; threshold=0.01 (10% > 1%).
;
; The hex-dump expectation is byte-level. Format (PLAN §9.3 v0x02):
;   u8 version (0x02), uleb128 num_bbs (0x03), then 3 × u16 LE attr.

; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -xbbr-cold-threshold=0.5 %s -filetype=obj -o %t.cold.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.cold.o \
; RUN:     | FileCheck %s --check-prefix=COLD
;
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -xbbr-cold-threshold=0.01 %s -filetype=obj -o %t.warm.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.warm.o \
; RUN:     | FileCheck %s --check-prefix=WARM

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

; With threshold=0.5, the 10%-frequency BB is cold (bit 7 = 0x80) and the
; 90%-frequency BB is not. Entry never cold. The on-disk byte stream is:
; 02=version, 03=num_bbs, then 3 × u16-LE: 0100=entry, 0000=warm, 8000=cold.
; readobj groups every 4 bytes ⇒ "02030100 00008000".
; COLD: 02030100 00008000
;
; With threshold=0.01, 10% > 1%, neither non-entry BB cold — all u16 words
; for non-entry BBs are 0x0000.
; WARM: 02030100 00000000
