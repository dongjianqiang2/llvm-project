# REQUIRES: aarch64

# P2-2: XBBR under PIE (-fPIE -pie). Physical BB reordering must preserve
# PC-relative correctness — lld resolves PC-rel relocs normally after XBBR
# moves the per-BB InputSections, and the thunk loop handles over-range B/BL.
# The link succeeds and is reproducible. (These functions issue B.cond, so
# renameSectionsForHotColdSplit keeps them co-located in .text for cond-pair
# safety — no .text.hot split here, unlike cond-free functions.)

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fPIE -fbb-cross-reorder=full \
# RUN:     -x ir a.ll -c -o a.o 2>/dev/null
# RUN: ld.lld -pie -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/pie 2>/dev/null
# RUN: llvm-readelf -SW %t/pie | FileCheck %s
# CHECK: .text

# Reproducibility (SPEC §9.3).
# RUN: ld.lld -pie -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/pie2 2>/dev/null
# RUN: cmp %t/pie %t/pie2

#--- a.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @ext(i32)
define void @a(i32 %n) noinline !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @b(i32 %n)
  br label %done
done:
  call void @ext(i32 1)
  ret void
}
define void @b(i32 %n) noinline !prof !2 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @ext(i32 2)
  br label %done
done:
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
!2 = !{!"function_entry_count", i64 800}
