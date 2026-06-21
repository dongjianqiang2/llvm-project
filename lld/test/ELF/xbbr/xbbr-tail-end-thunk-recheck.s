# REQUIRES: aarch64

# P0-2: Stage 5 tail-end recheck (PLAN §4.3 Stage 5 "末梢复核"). After
# finalizeAddressDependentContent converges, the SectionEmitter measures the
# REAL thunk overhead from emitted ThunkSections and compares it against
# Stage 4's projected estimate. When the projected estimate under-counted
# (here it is blind to the far absolute target `big`, which is not an XBBR
# node), the real thunk bytes can exceed --bb-cross-reorder-max-thunk-bytes
# even though Stage 4 saw no over-range edges. The recheck then emits a
# warning (the layout is already emitted; pre-emit revert is Stage 4's job).
#
# `big` at 0x1000000000 (>128 MiB) via --defsym forces one real
# __AArch64AbsLongThunk (16 B). Stage 4 projects 0 (BL target is not a node),
# so with budget=1 the tail-end recheck flags the overrun.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --defsym big=0x1000000000 --bb-cross-reorder-max-thunk-bytes=1 \
# RUN:     --bb-cross-reorder-stats --unresolved-symbols=ignore-all \
# RUN:     -o %t/exe 2>&1 | FileCheck %s

# Stage 4's projected estimate is 0 (the BL to the far absolute `big` is not
# an XBBR node, so it is not range-checked); the real thunk is 16 B.
# CHECK: xbbr-stage4: overRange=0 estThunkBytes=0 pinned=0 degraded=0
# CHECK: warning: XBBR tail-end recheck: real thunk bytes (16) exceed
# CHECK-SAME: --bb-cross-reorder-max-thunk-bytes (1); Stage 4 projected 0
# CHECK: xbbr-stage5: realThunkBytes=16 estThunkBytes=0 overrun=1

# Reproducibility (SPEC §9.3).
# RUN: ld.lld -e f a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --defsym big=0x1000000000 --bb-cross-reorder-max-thunk-bytes=1 \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe2 2>/dev/null
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
