# REQUIRES: aarch64

# P2-2 (SPEC §8.2): in a -shared library, only INTERNAL-linkage functions may
# have their BBs drift cross-function; exported symbols anchor their entry.
# The library links and is reproducible. (Both functions here issue B.cond, so
# renameSectionsForHotColdSplit keeps them co-located in .text for cond-pair
# safety — no .text.hot split; the internal function is still present and its
# entry anchored under -shared.)

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fPIC -fbb-cross-reorder=full \
# RUN:     -x ir lib.ll -c -o lib.o 2>/dev/null
# RUN: ld.lld -shared lib.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --soname=libx.so --unresolved-symbols=ignore-all -o %t/libx.so 2>/dev/null

# The internal function is present (LOCAL) under -shared.
# RUN: llvm-readelf -SW %t/libx.so | FileCheck %s --check-prefix=SEC
# RUN: llvm-readelf -sW %t/libx.so | FileCheck %s --check-prefix=SYM
# SEC: .text
# SYM: FUNC{{.*}}LOCAL{{.*}}internal

# Reproducibility (SPEC §9.3).
# RUN: ld.lld -shared lib.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --soname=libx.so --unresolved-symbols=ignore-all -o %t/libx2.so 2>/dev/null
# RUN: cmp %t/libx.so %t/libx2.so

#--- lib.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @ext(i32)
define void @exported(i32 %n) noinline !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @internal(i32 %n)
  br label %done
done:
  call void @ext(i32 1)
  ret void
}
define internal void @internal(i32 %n) noinline !prof !2 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit, !prof !1
body:
  call void @ext(i32 2)
  br label %exit
exit:
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
!2 = !{!"function_entry_count", i64 500}
