; REQUIRES: x86-registered-target
; RUN: echo "blacklisted_fn" > %t.bl
; RUN: llc -enable-xbbr -xbbr-blacklist=%t.bl -O2 \
; RUN:     -mtriple=x86_64-unknown-linux-gnu %s -filetype=obj -o %t.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s

;  / SPEC §6.1 -fbb-cross-reorder-blacklist=<file>: each function
; named in the blacklist file gets the UserBlacklisted bit on every
; non-entry BB. The entry BB is anchored regardless (function symbol =
; entry address) so the redundant bit is suppressed there.
;
; not_blacklisted has the same shape as blacklisted_fn, but should NOT
; be flagged.

define i32 @blacklisted_fn(i32 %n) {
entry:
  %c = icmp slt i32 %n, 0
  br i1 %c, label %neg, label %pos
neg:
  ret i32 -1
pos:
  ret i32 1
}

define i32 @not_blacklisted(i32 %n) {
entry:
  %c = icmp slt i32 %n, 0
  br i1 %c, label %neg, label %pos
neg:
  ret i32 -1
pos:
  ret i32 1
}

; Two functions × 3 MBB each = 12 bytes (6 u16 attr words).
;
; @blacklisted_fn:
;   entry: IsEntry only (entry exempt) = 0x0001
;   neg:   UserBlacklisted             = 0x0040
;   pos:   UserBlacklisted             = 0x0040
;
; @not_blacklisted:
;   entry: IsEntry only                = 0x0001
;   neg/pos: clean                     = 0x0000  (both BBs)
;
; (Branch-weight or layout reordering at -O2 may swap neg/pos —
;  match in any order with -DAG inside each function group.)
; CHECK: 0203
; CHECK-DAG: 0100
; CHECK-DAG: 4000
; CHECK-DAG: 4000
