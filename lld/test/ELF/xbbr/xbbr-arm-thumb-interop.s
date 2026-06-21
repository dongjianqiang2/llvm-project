# REQUIRES: arm

# P2-1: ARM↔Thumb interop under XBBR. An A32 (ARM-mode) caller `a` and a
# Thumb-2 callee `b` are compiled as separate objects and linked together with
# XBBR. The cross-mode BL from `a` (ARM) to `b` (Thumb) requires an ARM→Thumb
# interworking thunk, which lld inserts during finalizeAddressDependentContent.
# XBBR's per-BB section reordering must not break mixed-mode linking: the link
# succeeds, the .text.hot split is produced, and the result is reproducible.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target armv7a-linux-gnueabi -O2 -fbb-cross-reorder=full -x ir \
# RUN:     arm_fn.ll -c -o arm_fn.o 2>/dev/null
# RUN: clang -target thumbv7a-linux-gnueabi -O2 -fbb-cross-reorder=full -x ir \
# RUN:     thumb_fn.ll -c -o thumb_fn.o 2>/dev/null

# RUN: ld.lld -e a arm_fn.o thumb_fn.o --bb-cross-reorder=foo \
# RUN:     --bb-cross-reorder-mode=full --unresolved-symbols=ignore-all \
# RUN:     -o %t/lib 2>/dev/null
# RUN: llvm-readelf -SW %t/lib | FileCheck %s --check-prefix=SEC
# SEC: .text
# SEC: .text.hot

# Both functions are present (a in ARM state, b in Thumb state — bit 0 set).
# RUN: llvm-nm --numeric-sort --defined-only %t/lib | FileCheck %s --check-prefix=SYM
# SYM: a
# SYM: b

# Reproducibility (SPEC §9.3): two XBBR links are bitwise identical.
# RUN: ld.lld -e a arm_fn.o thumb_fn.o --bb-cross-reorder=foo \
# RUN:     --bb-cross-reorder-mode=full --unresolved-symbols=ignore-all \
# RUN:     -o %t/lib2 2>/dev/null
# RUN: cmp %t/lib %t/lib2

#--- arm_fn.ll
target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64"
target triple = "armv7a-linux-gnueabi"
declare void @b(i32)
define void @a(i32 %n) noinline !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @b(i32 %n)
  br label %done
done:
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}

#--- thumb_fn.ll
target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64"
target triple = "thumbv7a-linux-gnueabi"
define void @b(i32 %n) noinline !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit, !prof !1
body:
  br label %exit
exit:
  ret void
}
!0 = !{!"function_entry_count", i64 900}
!1 = !{!"branch_weights", i32 900, i32 100}
