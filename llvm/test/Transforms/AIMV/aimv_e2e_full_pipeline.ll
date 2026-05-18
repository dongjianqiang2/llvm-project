; [AIMV] E2E: all 3 diagnostic passes verified in a single test file.
; Each RUN invokes a different pass on proven-trigger IR, appending to the
; same JSON file. The final CHECK verifies all 3 pass types are present.

; RUN: rm -f %t.json

; --- LoopVectorize: aliasing pointers ---
; RUN: opt -passes="loop-vectorize,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-vectorize \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1

; --- SLPVectorizer: non-consecutive memory accesses ---
; RUN: opt -passes="slp-vectorizer,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=slp-vectorizer \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1

; --- LoopUnroll: variable trip count ---
; RUN: opt -passes="loop-unroll,aimv-feedback" -S < %s \
; RUN:   -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-unroll \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1

; --- Verify all 3 passes appear in combined JSON ---
; RUN: FileCheck %s < %t.json
; CHECK-DAG: "pass_name":"LoopVectorize"
; CHECK-DAG: "pass_name":"SLPVectorize"
; CHECK-DAG: "pass_name":"LoopUnroll"

; LoopVectorize target: aliasing pointer writes + reads
define void @lv_func(ptr %a, ptr %b, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %gep_a = getelementptr i32, ptr %a, i32 %i
  %gep_b = getelementptr i32, ptr %b, i32 %i
  store i32 0, ptr %gep_a
  %v = load i32, ptr %gep_b
  %i.next = add i32 %i, 1
  %cmp = icmp slt i32 %i.next, %n
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}

; SLP target: non-consecutive loads from different base pointers
define void @slp_func(ptr %a, ptr %b, ptr %c) {
  %v1 = load i32, ptr %a
  %v2 = load i32, ptr %b
  %v3 = load i32, ptr %c
  %add1 = add i32 %v1, %v2
  %add2 = add i32 %v1, %v3
  store i32 %add1, ptr %a
  store i32 %add2, ptr %b
  ret void
}

; Unroll target: variable trip count prevents compile-time unroll decision
define void @unroll_func(ptr %a, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %gep = getelementptr i32, ptr %a, i32 %i
  store i32 0, ptr %gep
  %i.next = add i32 %i, 1
  %cmp = icmp slt i32 %i.next, %n
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}
