# REQUIRES: aarch64

# P0-1: a thunkable B/BL (AArch64 JUMP26/CALL26) whose real target is out of
# the ±128 MiB branch range must NOT be pinned by Stage 4 — only the
# unthunkable conditional/test branches (CONDBR19/TSTBR14) are pinned. lld's
# existing range-extension thunk loop then injects __AArch64AbsLongThunk_<sym>.
#
# This mirrors lld's own aarch64-jump26-thunk.s, but compiles the caller under
# -fbb-cross-reorder=full so it flows through the real XBBR pipeline (per-BB
# sections + BBAddrMap → Stage 0 graph → Stage 4 constraint solve → physical
# emission). The far target `big` is an absolute symbol at 0x1000000000
# (>128 MiB from .text) supplied via --defsym, so no 128 MiB filler is needed.
# The link succeeds and the thunk symbol is present, proving XBBR's Stage 4
# (non-pin of B/BL) and physical emission cooperate with lld thunking.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null

# lld inserts a range-extension thunk for the over-range BL/B to `big`.
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --defsym big=0x1000000000 --unresolved-symbols=ignore-all \
# RUN:     -o %t/exe 2>/dev/null
# RUN: llvm-nm %t/exe | FileCheck %s
# CHECK: __AArch64AbsLongThunk_big

# Reproducibility (SPEC §9.3): two XBBR links are bitwise identical.
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --defsym big=0x1000000000 --unresolved-symbols=ignore-all \
# RUN:     -o %t/exe2 2>/dev/null
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
