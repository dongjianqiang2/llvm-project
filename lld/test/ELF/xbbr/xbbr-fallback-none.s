# REQUIRES: aarch64

# P0-1 / TASK M3-T04-C5: --bb-cross-reorder-fallback=none is the CI strict
# mode (SPEC §7). When Stage 4 cannot satisfy the thunk-byte budget without
# crossing the 30% degrade threshold, it must NOT silently degrade — it must
# emit a fatal error and abort the link (non-zero exit). The hidden
# branch-range testing knob forces the unsatisfiable budget on a tiny binary.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=full -x ir a.ll \
# RUN:     -c -o a.o 2>/dev/null
# RUN: not ld.lld -e a a.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-branch-range-for-testing=8 \
# RUN:     --bb-cross-reorder-max-thunk-bytes=1 --bb-cross-reorder-fallback=none \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe 2>&1 | FileCheck %s
# CHECK: error: XBBR: thunk-budget constraints cannot be satisfied
# CHECK-SAME: --bb-cross-reorder-fallback=none is set; aborting.

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
  call void @c(i32 %n)
  br label %done
done:
  call void @ext(i32 2)
  ret void
}
define void @c(i32 %n) noinline !prof !3 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %hot, label %done, !prof !1
hot:
  call void @ext(i32 3)
  br label %done
done:
  call void @ext(i32 4)
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
!2 = !{!"function_entry_count", i64 900}
!3 = !{!"function_entry_count", i64 800}
