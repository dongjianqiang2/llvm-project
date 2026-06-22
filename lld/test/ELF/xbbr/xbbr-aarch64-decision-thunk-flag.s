# REQUIRES: aarch64

# P0 task #2 (PLAN §9.4 "decision_flags Thunk 位"): a Moved BB whose branch
# is over-range and therefore linker-thunked must carry the Thunk flag in
# .debug_xbbr_decision. lld rewrites the over-range branch's reloc.sym to the
# thunk's own Defined symbol (Relocations.cpp: `rel.sym =
# t->getThunkTargetSym()`), whose section is a ThunkSection named ".text.thunk";
# SectionEmitter::backfillDecisionMapVAs detects that and ORs in
# EntryFlags::Thunk (on top of Moved, so the entry reads moved+thunk).
#
# The over-range target `big` is an absolute symbol at 0x1000000000 (>128 MiB
# from .text) supplied via --defsym, so no 128 MiB filler is needed. The hot
# BB of `f` calls `big`; under -fbb-cross-reorder=full it migrates to .text.hot
# (Moved), and its BL big is over-range -> lld emits __AArch64AbsLongThunk_big
# -> the BB's decision-map entry gets the Thunk flag.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null

# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --defsym big=0x1000000000 --unresolved-symbols=ignore-all \
# RUN:     --bb-cross-reorder-emit-decision-map -o %t/exe

# lld emitted the range-extension thunk.
# RUN: llvm-nm %t/exe | FileCheck %s --check-prefix=THUNKSYM
# THUNKSYM: __AArch64AbsLongThunk_big

# The decision map reports at least one Thunk-flagged entry.
# RUN: llvm-bbreorder-dump --summary %t/exe | FileCheck %s --check-prefix=SUM
# SUM: xbbr-dump: entries={{[1-9]}}
# SUM: thunk={{[1-9]}}

# Human dump surfaces the Thunk flag on the moved hot BB.
# RUN: llvm-bbreorder-dump %t/exe | FileCheck %s --check-prefix=HUMAN
# HUMAN: thunk

# Reproducibility (SPEC §9.3): the Thunk flag is set deterministically.
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --defsym big=0x1000000000 --unresolved-symbols=ignore-all \
# RUN:     --bb-cross-reorder-emit-decision-map -o %t/exe2
# RUN: cmp %t/exe %t/exe2

#--- a.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @big()
declare void @ext()
define void @f(i32 %n) noinline !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @big()
  br label %done
done:
  call void @ext()
  ret void
}
define void @g(i32 %n) noinline !prof !2 {
entry:
  call void @f(i32 %n)
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
!2 = !{!"function_entry_count", i64 800}
