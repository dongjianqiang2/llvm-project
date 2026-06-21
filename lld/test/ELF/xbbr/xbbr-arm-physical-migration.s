# REQUIRES: arm

# P2-1: XBBR physical reordering on ARM (A32). Three functions a (hot,
# entry_count 1000), c (cold, 1), b (hot, 900), with a hot a→b call edge.
# Input/module order is a, c, b. XBBR clusters the hot pair {a,b} (CGProfile
# a→b edge from !prof) and pushes cold c last, so the linked .text order
# becomes a, b, c — b physically moves before c. This proves the Stage 0
# (ELF32LE/REL BBAddrMap decode) → Stage 1-4 → physical emission pipeline works
# end-to-end on ARM, deterministically.
#
# ARM functions carry a mandatory .ARM.exidx unwind entry, so every function is
# EH-gated (PLAN §5.3 / §5.4): non-entry BBs move wholesale with their function
# rather than drifting individually — a correct, mechanism-first safety
# property. Function-level hot/cold reordering still applies (b before c), and
# the .text.hot split is produced by the P1-1 section rename.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target armv7a-linux-gnueabi -O2 -fbb-cross-reorder=full -x ir \
# RUN:     cross.ll -c -o cross.o 2>/dev/null

# Input-order baseline (--call-graph-profile-sort=none): module order a, c, b.
# RUN: ld.lld -e a cross.o --call-graph-profile-sort=none \
# RUN:     --unresolved-symbols=ignore-all -o %t/input 2>/dev/null
# RUN: llvm-nm --numeric-sort --defined-only %t/input | sed -n 's/.* \(a\|b\|c\)$/\1/p' > %t/input.order

# XBBR full: hot {a,b} clustered, cold c last → a, b, c.
# RUN: ld.lld -e a cross.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/xbbr 2>/dev/null
# RUN: llvm-nm --numeric-sort --defined-only %t/xbbr | sed -n 's/.* \(a\|b\|c\)$/\1/p' > %t/xbbr.order

# The two orders differ (b moved before c) AND XBBR is a, b, c.
# RUN: not cmp %t/input.order %t/xbbr.order
# RUN: FileCheck %s --check-prefix=ORDER < %t/xbbr.order

# ORDER: a
# ORDER-NEXT: b
# ORDER-NEXT: c

# The .text.hot split is produced (P1-1).
# RUN: llvm-readelf -SW %t/xbbr | FileCheck %s --check-prefix=SEC
# SEC: .text
# SEC: .text.hot

# Reproducibility (SPEC §9.3): two XBBR links are bitwise identical.
# RUN: ld.lld -e a cross.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/xbbr1 2>/dev/null
# RUN: ld.lld -e a cross.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/xbbr2 2>/dev/null
# RUN: cmp %t/xbbr1 %t/xbbr2

#--- cross.ll
target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64"
target triple = "armv7a-linux-gnueabi"
declare void @ext(i32)

define void @a(i32 %n) noinline !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !3
hot:
  call void @b(i32 %n)
  br label %done
done:
  call void @ext(i32 1)
  ret void
}
define void @c(i32 %n) noinline !prof !2 {
entry:
  call void @ext(i32 2)
  ret void
}
define void @b(i32 %n) noinline !prof !1 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit, !prof !4
body:
  call void @ext(i32 3)
  br label %exit
exit:
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"function_entry_count", i64 900}
!2 = !{!"function_entry_count", i64 1}
!3 = !{!"branch_weights", i32 900, i32 100}
!4 = !{!"branch_weights", i32 900, i32 100}
