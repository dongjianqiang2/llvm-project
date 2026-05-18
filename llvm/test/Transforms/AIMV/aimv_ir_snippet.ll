; RUN: opt -passes="aimv-feedback" -S < %s \
; RUN:   -aimv-output=%t.json -aimv-enable 2>&1
; RUN: FileCheck %s < %t.json
; CHECK: "ir_snippet"
; CHECK: "source_context":"{{.*}}test.c:5{{.*}}"
;
; Function with an actual loop body and matching diagnostic source location.
; ir_snippet and source_context should be populated (source_context content
; depends on whether test.c exists on disk).

define void @test_loop(ptr %a, i32 %n) !dbg !20 {
entry:
  br label %loop, !dbg !21
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

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!10}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang", isOptimized: true, emissionKind: FullDebug)
!1 = !DIFile(filename: "test.c", directory: "/tmp")
!10 = !{i32 2, !"Debug Info Version", i32 3}
!20 = distinct !DISubprogram(name: "test_loop", scope: !1, file: !1, line: 4, type: !22, unit: !0)
!21 = !DILocation(line: 5, column: 5, scope: !20)
!22 = !DISubroutineType(types: !{})

; Diagnostic source location matches the DILocation above.
!aimv.diag = !{!30}
!30 = !{!"LoopVectorize", !"CantReorderMemOps", !"test_loop", !"test.c:5:5", !"msg", !31, !32, !33, !34}
!31 = !{i32 5, i32 8, i32 4, i32 1}
!32 = !{i32 0}
!33 = !{i32 1, i32 0, i32 0, i32 4, !"stride=1", i32 0, i32 0}
!34 = !{!"loop", i32 1, i32 5, i32 100, i32 0, i32 0}
