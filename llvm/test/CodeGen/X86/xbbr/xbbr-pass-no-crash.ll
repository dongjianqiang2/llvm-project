; REQUIRES: asserts
; RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu %s -o /dev/null

; Verify the XBBRMetadataEmitter pass does not crash on functions containing
; recursion, indirect branches, and exception handling (all blacklist cases).

@ptr = global ptr null

define i32 @recfib(i32 %n) {
; recursion
entry:
  %cmp = icmp slt i32 %n, 2
  br i1 %cmp, label %base, label %rec
base:
  ret i32 1
rec:
  %s1 = sub i32 %n, 1
  %a = call i32 @recfib(i32 %s1)
  %s2 = sub i32 %n, 2
  %b = call i32 @recfib(i32 %s2)
  %add = add i32 %a, %b
  ret i32 %add
}

define void @indirectbr() {
; address-taken blocks via indirectbr
entry:
  %p = load ptr, ptr @ptr
  indirectbr ptr %p, [label %L1, label %L2]
L1:
  store ptr blockaddress(@indirectbr, %L2), ptr @ptr
  ret void
L2:
  ret void
}

define i32 @withpersonality() personality ptr @__gxx_personality_v0 {
; EH landing pad
entry:
  invoke void @maythrow() to label %cont unwind label %lpad
cont:
  ret i32 0
lpad:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
}

declare void @maythrow()
declare i32 @__gxx_personality_v0(...)
