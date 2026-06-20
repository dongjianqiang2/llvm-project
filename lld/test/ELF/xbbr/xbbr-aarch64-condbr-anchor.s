# REQUIRES: aarch64

# Phase 1a range safety: on AArch64, B.cond (R_AARCH64_CONDBR19, ±1 MiB) and
# TBZ/TBNZ (R_AARCH64_TSTBR14, ±32 KiB) cannot be thunked by lld — only B/BL
# can. So a BB that is the source or target of such a branch must not migrate,
# or the branch could overflow and hard-error at link time. XBBR marks those
# BBs CondInvolved and pins them. Under =all, `br i1` lowers to B.cond <taken>
# (CONDBR19) + B <fallthrough> (JUMP26): the entry (source) and the B.cond
# target are pinned; the B target is thunkable and may migrate. The link
# succeeds and at least the conditional pair is anchored.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=partial -x ir \
# RUN:     f.ll -c -o f.o 2>/dev/null
# RUN: ld.lld -e f f.o --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-emit-decision-map --unresolved-symbols=ignore-all \
# RUN:     -o %t/exe 2>/dev/null
# RUN: llvm-bbreorder-dump --summary %t/exe | FileCheck %s

# 3 BBs (entry, then, else); entry + the B.cond target are pinned (≥2 anchored).
# CHECK: xbbr-dump: entries=3
# CHECK: anchored={{[2-9]}}

#--- f.ll
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"
declare void @then(i32)
declare void @els(i32)
define void @f(i32 %n) !prof !0 {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %t, label %e, !prof !1
t:
  call void @then(i32 %n)
  ret void
e:
  call void @els(i32 %n)
  ret void
}
!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 900, i32 100}
