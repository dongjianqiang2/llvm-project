# REQUIRES: aarch64

# Cond-pair safety: a function that issues an unthunkable conditional branch
# (br i1 -> B.cond, R_AARCH64_CONDBR19) must keep ALL its BBs co-located in the
# same output section. renameSectionsForHotColdSplit detects cond-branch
# functions from raw reloc TYPES at graph-build time (HasCondBranch, in
# collectFromFile — section relocs are not scanned until the Writer, after the
# rename) and skips .text.hot routing for the whole function, so the cond pair
# is never split across .text / .text.hot. The projection (Stage 4) ignores
# output-section routing, so a split pair would slip past collectCondPins and
# fatal at verifyCondRangesFinal; this guard prevents that.
#
# f issues B.cond. Pre-fix its hot BB routed to .text.hot (splitting the
# entry->hot B.cond pair across sections); post-fix the whole function stays in
# .text, so no .text.hot section is produced at all. The link succeeds and is
# reproducible.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe 2>/dev/null
# RUN: llvm-readelf -SW %t/exe | FileCheck %s

# f stays co-located in .text — no .text.hot split for a cond-branch function.
# CHECK: .text
# CHECK-NOT: .text.hot

# Reproducibility (SPEC §9.3).
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe2 2>/dev/null
# RUN: cmp %t/exe %t/exe2

#--- a.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @ext(i32)
define void @f(i32 %n) noinline !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @ext(i32 1)
  br label %done
done:
  call void @ext(i32 2)
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
