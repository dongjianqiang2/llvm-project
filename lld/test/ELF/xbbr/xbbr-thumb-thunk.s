# REQUIRES: arm

# P2-1: a thunkable Thumb BL (R_ARM_THM_CALL) whose real target is out of the
# ±16 MiB branch range must NOT be pinned by Stage 4 — only the unthunkable
# Thumb-1 B<cond> narrow (R_ARM_THM_JUMP11, ±2 KiB) is pinned by
# markRangeAnchors. lld's existing range-extension thunk loop then injects an
# ARM/Thumb long-branch thunk.
#
# Compiles the caller under -fbb-cross-reorder=full so it flows through the
# real XBBR pipeline (per-BB sections + BBAddrMap → Stage 0 ELF32LE/REL graph
# → Stage 4 constraint solve → physical emission). The far target `big` is an
# absolute symbol at 0x1000000000 (>16 MiB from .text) supplied via --defsym,
# so no large filler is needed. The link succeeds and the thunk symbol is
# present, proving XBBR's Stage 4 (non-pin of thunkable B/BL) and physical
# emission cooperate with lld ARM/Thumb thunking.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target thumbv7a-linux-gnueabi -O2 -fbb-cross-reorder=full -x ir \
# RUN:     a.ll -c -o a.o 2>/dev/null

# lld inserts a range-extension thunk for the over-range BL to `big`.
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --defsym big=0x1000000000 --unresolved-symbols=ignore-all \
# RUN:     -o %t/exe 2>/dev/null
# RUN: llvm-nm %t/exe | FileCheck %s
# CHECK: Thunk_big

# Reproducibility (SPEC §9.3): two XBBR links are bitwise identical.
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --defsym big=0x1000000000 --unresolved-symbols=ignore-all \
# RUN:     -o %t/exe2 2>/dev/null
# RUN: cmp %t/exe %t/exe2

#--- a.ll
target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64"
target triple = "thumbv7a-linux-gnueabi"
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
