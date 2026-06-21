# REQUIRES: aarch64

# P1-2: AArch64 --optimize-bb-jumps. Under -fbasic-block-sections=all (implied
# by -fbb-cross-reorder=full), each BB that falls through to the next emits a
# redundant trailing B (R_AARCH64_JUMP26). AArch64::deleteFallThruJmpInsn now
# deletes a trailing B whose target is the immediately following section (a
# genuine fall-through — exact VA match, so the deletion is semantically safe).
# This shrinks .text. (The B.cond+B flip case is a follow-up; case 1 captures
# the common fall-through B.)
#
# Without qemu-aarch64 the binary cannot be run in this environment; correctness
# rests on the exact-VA-match invariant (the B is deleted only when its target
# equals the next section's start, i.e. execution would fall through there
# anyway). Reproducibility is the determinism gate.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null

# Baseline: full mode, no --optimize-bb-jumps (redundant fall-through B's kept).
# RUN: ld.lld -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --unresolved-symbols=ignore-all -o %t/noopt 2>/dev/null

# With --optimize-bb-jumps: fall-through B's deleted → smaller .text.
# RUN: ld.lld -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --optimize-bb-jumps --unresolved-symbols=ignore-all -o %t/opt 2>/dev/null

# The optimized binary is strictly smaller (redundant B's removed).
# RUN: test $(stat -c %%s %t/opt) -lt $(stat -c %%s %t/noopt)

# Both still lay out a .text.hot (P1-1 split survives the jump optimization).
# RUN: llvm-readelf -SW %t/opt | FileCheck %s
# CHECK: .text
# CHECK: .text.hot

# Reproducibility (SPEC §9.3): two --optimize-bb-jumps links are identical.
# RUN: ld.lld -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --optimize-bb-jumps --unresolved-symbols=ignore-all -o %t/opt2 2>/dev/null
# RUN: cmp %t/opt %t/opt2

#--- a.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
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
