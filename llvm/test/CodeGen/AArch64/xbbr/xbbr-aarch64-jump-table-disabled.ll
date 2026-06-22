; REQUIRES: aarch64-registered-target
; RUN: clang -target aarch64-linux-gnu -O2 -fbb-cross-reorder=partial -x ir %s \
; RUN:     -S -o %t.s 2>/dev/null
; RUN: FileCheck %s < %t.s
;
; Regression test for the AArch64 jump-table-vs-BB-sections build failure
; (consumer-lame/parse.c "invalid symbol kind for ADR relocation").
;
; Under -fbb-cross-reorder=partial|full, AArch64 implies
; -fbasic-block-sections=all. AArch64 jump tables lower to
;   `adr x, <base-BB>` + `.byte (target-BB - base-BB)>>2` entries, but under
; BB sections each BB is its own section, so the `adr` targets a section symbol
; and the `(sym-sym)>>2` data is a cross-section difference — both rejected by
; MC ("invalid symbol kind for ADR relocation" / "expected relocatable
; expression"). Upstream rejects -fbasic-block-sections=all on AArch64 partly
; for this reason.
;
; XBBR disables AArch64 jump tables (clang BackendUtil sets
; -aarch64-min-jump-table-entries=UINT_MAX) so switches lower to compare chains
; and BB-section codegen is valid. This test asserts NO jump table is emitted
; (no `adr`-to-BB, no `(sym-sym)>>2` data) under -fbb-cross-reorder=partial.
; (Uses clang, not llc, because the disable lives in BackendUtil.)

define i32 @sparse_switch(i32 %x) noinline {
entry:
  switch i32 %x, label %default [
    i32 3,   label %c0
    i32 10,  label %c1
    i32 17,  label %c2
    i32 24,  label %c3
    i32 31,  label %c4
    i32 38,  label %c5
    i32 45,  label %c6
    i32 52,  label %c7
    i32 59,  label %c8
    i32 66,  label %c9
    i32 73,  label %c10
    i32 80,  label %c11
    i32 87,  label %c12
    i32 94,  label %c13
    i32 101, label %c14
    i32 108, label %c15
    i32 115, label %c16
    i32 122, label %c17
    i32 129, label %c18
    i32 136, label %c19
  ], !prof !0
c0:  ret i32 1
c1:  ret i32 2
c2:  ret i32 4
c3:  ret i32 8
c4:  ret i32 16
c5:  ret i32 32
c6:  ret i32 64
c7:  ret i32 128
c8:  ret i32 256
c9:  ret i32 512
c10: ret i32 1024
c11: ret i32 2048
c12: ret i32 4096
c13: ret i32 8192
c14: ret i32 16384
c15: ret i32 32768
c16: ret i32 65536
c17: ret i32 131072
c18: ret i32 262144
c19: ret i32 524288
default:
  ret i32 -1
}

!0 = !{!"branch_weights", i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1, i32 1}

; No jump-table dispatch: no `adr` to a per-BB base, and no `.byte (sym-sym)>>2`
; data. The switch lowers to a compare chain (cmp/b.eq sequence).
; CHECK-NOT: .byte  {{.*}}>>2
; CHECK-NOT: adr	{{x[0-9]+}}, {{.*}}__part
; CHECK-NOT: LJTI
