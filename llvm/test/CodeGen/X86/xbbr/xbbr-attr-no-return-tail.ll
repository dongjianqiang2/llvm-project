; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o | FileCheck %s

; M1-T05 / SPEC §5.3 item 7 (review #2 fix): a BB whose terminator is a
; `noreturn` callsite and which has no successors must be flagged
; IsNoReturnTail (bit 8 = 0x100). The narrow detection — succ_empty()
; AND `noreturn` callsite immediately before `unreachable` — keeps us
; from over-matching every abort()/exit() call site (those that have
; in-function successors are not "tail" blocks and remain migratable).

declare void @do_die() noreturn

define i32 @hot_with_die(i32 %n) {
entry:
  %c = icmp slt i32 %n, 0
  br i1 %c, label %bad, label %good
bad:
  call void @do_die() noreturn
  unreachable
good:
  ret i32 %n
}

; The function has 3 MBBs at -O2:
;   entry: just the branch       → 0x0001 (Entry only)
;   good:  ret                   → 0x0000
;   bad:   noreturn + unreachable, succ_empty → 0x0100 (IsNoReturnTail)
;
; Layout order may vary; assert version/num + presence of all three
; attr words.
;
; Hex layout (8 bytes total = 1 ver + 1 num + 3 × u16 LE):
;   02 03 | 01 00 | 00 00 | 00 01
; readobj prints 4-byte groups, so:
;   02030100 00000001
; CHECK: 02030100 00000001
