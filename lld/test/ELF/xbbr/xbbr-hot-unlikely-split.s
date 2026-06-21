# REQUIRES: aarch64

# P1-1 (SPEC §2.2 cold-code isolation): XBBR renames per-BB InputSections by
# hot/cold classification so that, with -z keep-text-section-prefix (implied for
# partial/full), lld's orphan grouping routes them into separate output
# sections: hot migratable BBs → .text.hot, (full mode) cold BBs →
# .text.unlikely, entry/anchor BBs → .text. In partial mode cold BBs stay in
# .text (SPEC §4: cold stays at its original function position).
#
# optnone keeps -O2 from collapsing the small functions to a single BB so the
# per-BB split is observable.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null

# Full mode: hot BBs → .text.hot, entry/anchor → .text.
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/full 2>/dev/null
# RUN: llvm-readelf -SW %t/full | FileCheck %s --check-prefix=FULL
# FULL: .text
# FULL: .text.hot

# Partial mode also splits hot BBs out (cold stays in .text in partial).
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=partial \
# RUN:     --unresolved-symbols=ignore-all -o %t/part 2>/dev/null
# RUN: llvm-readelf -SW %t/part | FileCheck %s --check-prefix=PART
# PART: .text
# PART: .text.hot

# A plain link (no XBBR) keeps everything in .text — no .text.hot.
# RUN: ld.lld -e f a.o --unresolved-symbols=ignore-all -o %t/plain 2>/dev/null
# RUN: llvm-readelf -SW %t/plain | FileCheck %s --check-prefix=PLAIN \
# RUN:     --allow-empty
# PLAIN: .text
# PLAIN-NOT: .text.hot

# Reproducibility (SPEC §9.3).
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/full2 2>/dev/null
# RUN: cmp %t/full %t/full2

#--- a.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @ext(i32)
define void @f(i32 %n) noinline optnone !prof !0 {
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
define void @g(i32 %n) noinline optnone !prof !2 {
entry:
  call void @f(i32 %n)
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
!2 = !{!"function_entry_count", i64 800}
