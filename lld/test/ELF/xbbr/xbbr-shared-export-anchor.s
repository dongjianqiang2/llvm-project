# REQUIRES: aarch64

# P2-2 (SPEC §8.2): in a -shared library, exported symbols strictly anchor
# their function entry block (ABI §5.1: function symbol = entry BB). XBBR
# anchors every entry block (isEntry), so an exported function's symbol
# resolves to its entry in .text while its non-entry BBs may migrate to
# .text.hot. PLT/GOT for external symbols are unaffected (they reference
# symbols, not BBs). The link succeeds and is reproducible.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fPIC -fbb-cross-reorder=full \
# RUN:     -x ir lib.ll -c -o lib.o 2>/dev/null
# RUN: ld.lld -shared lib.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --soname=libx.so --unresolved-symbols=ignore-all -o %t/libx.so 2>/dev/null

# exported is a GLOBAL FUNC (visible to dynamic linker); the lib has a .text.hot
# split (non-entry BBs migrated). PLT/GOT for @ext resolved (link succeeded).
# RUN: llvm-readelf -SW %t/libx.so | FileCheck %s --check-prefix=SEC
# RUN: llvm-readelf -sW %t/libx.so | FileCheck %s --check-prefix=SYM
# SEC: .text
# SEC: .text.hot
# SYM: FUNC{{.*}}GLOBAL{{.*}}exported

# Reproducibility (SPEC §9.3).
# RUN: ld.lld -shared lib.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --soname=libx.so --unresolved-symbols=ignore-all -o %t/libx2.so 2>/dev/null
# RUN: cmp %t/libx.so %t/libx2.so

#--- lib.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @ext(i32)
define void @exported(i32 %n) noinline !prof !0 {
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
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
