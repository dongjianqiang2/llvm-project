; REQUIRES: aarch64-registered-target
;
; XBBR AArch64: mirror of xbbr-bb-addr-map-pgo.ll. Asserts that
; -enable-xbbr emits SHT_LLVM_BB_ADDR_MAP with PGO analyses on AArch64
; just like on x86_64 — both share the ELF64LE backend, so the lld
; Stage 0 / dump-tool / per-BB attr emission path should work uniformly.

; RUN: llc -enable-xbbr -O2 -mtriple=aarch64-linux-gnu %s \
; RUN:     -filetype=obj -o %t.o
; RUN: llvm-readobj --bb-addr-map --pretty-pgo-analysis-map %t.o \
; RUN:     | FileCheck %s
; RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o \
; RUN:     | FileCheck %s --check-prefix=ATTR

declare i32 @sink_a(i32)
declare i32 @sink_b(i32)

define i32 @hot(i32 %n) !prof !0 {
entry:
  %c = icmp slt i32 %n, 0
  br i1 %c, label %neg, label %pos, !prof !1
neg:
  %a = call i32 @sink_a(i32 %n)
  ret i32 %a
pos:
  %b = call i32 @sink_b(i32 %n)
  ret i32 %b
}

!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 100, i32 900}

; CHECK: BBAddrMap [
; CHECK:   Function {
; CHECK:     Name: hot
; CHECK:     PGO analyses {
; CHECK:       FuncEntryCount: 1000
;
; The MBB count after AArch64 lowering with calls in both branches is 3
; (entry + neg + pos). Attr block is: 02 (version) 03 (uleb128 num_bbs)
; 0100 (entry) 0000 0000 (two warm-no-cold-bit BBs).
; ATTR: 02030100 00000000

