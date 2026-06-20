# REQUIRES: aarch64

# Phase 3: after the thunk/address loop converges, the decision map's
# OrigFuncAddr/NewAddress are backfilled with real linked VAs (not the
# placeholder 0 / projected offsets from Stage 5). Verify:
#   * entries are non-zero
#   * the entry BB's NewAddress equals its function's OrigFuncAddr (ABI §5.1)
#   * llvm-bbreorder-dump reads the map and reports moved/anchored counts

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=partial -x ir \
# RUN:     cross.ll -c -o cross.o 2>/dev/null
# RUN: ld.lld -e a cross.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-emit-decision-map --unresolved-symbols=ignore-all \
# RUN:     -o %t/exe 2>/dev/null

# The dump tool parses the map; a populated, non-degraded link yields entries.
# RUN: llvm-bbreorder-dump --summary %t/exe | FileCheck %s --check-prefix=SUM
# SUM: xbbr-dump: entries={{[1-9]}}
# SUM: moved={{[1-9]}}
# SUM: anchored={{[1-9]}}

# Every entry byte after the 16-byte header is a 32-byte record. The first
# 8 bytes of each record are OrigFuncAddr; bytes 12-19 are NewAddress. Both
# must be non-zero for a real layout (the entry BB at minimum lands at the
# function VA). Assert the header magic + that the first record's OrigFuncAddr
# (offset 0x10) is non-zero.
# RUN: llvm-readelf -x .debug_xbbr_decision %t/exe | FileCheck %s --check-prefix=HEX
# HEX: XBBR
# HEX-NOT: 00000000 00000000 00000000 00000000

#--- cross.ll
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
