; REQUIRES: x86-registered-target
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t1.o
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s \
; RUN:     -filetype=obj -o %t2.o
; RUN: cmp %t1.o %t2.o

; M1-T05 + SPEC §9.3: same source + flags ⇒ bitwise-identical .o, including
; the new .llvm_xbbr_attr section. Catches DenseMap-iteration-order leaks
; in the per-MF attr table. Without `pinned` ordering, this would fail.

@addrs = global [2 x ptr] [ptr blockaddress(@indir, %L1), ptr blockaddress(@indir, %L2)]

declare void @maythrow()
declare i32 @__gxx_personality_v0(...)
declare i32 @callee(i32)

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

define i32 @withmusttail(i32 %x) {
entry:
  %a = musttail call i32 @callee(i32 %x)
  ret i32 %a
}

define i32 @haseh() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @maythrow() to label %cont unwind label %lpad
cont:
  ret i32 0
lpad:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
}
