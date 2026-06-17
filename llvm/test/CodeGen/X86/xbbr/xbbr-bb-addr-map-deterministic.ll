; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t1.o
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t2.o
; RUN: cmp %t1.o %t2.o

; M1-T03 + SPEC §9.3: same source + flags must produce bitwise-identical
; objects. This is a CI gate — a deterministic-build regression must fail
; here before it can poison the linker pipeline.

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
