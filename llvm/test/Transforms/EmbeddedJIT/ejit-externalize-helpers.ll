; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S < %s > %t.out
; RUN: FileCheck %s --check-prefix=MOD < %t.out
; RUN: opt -S %t-dump/*.bc | FileCheck %s --check-prefix=EXT
; RUN: rm -rf %t-dump-hi && mkdir -p %t-dump-hi
; RUN: opt -passes=ejit-register-bitcode -ejit-externalize-min-insts=1000 -ejit-dump-bitcode-dir=%t-dump-hi -S < %s > %t.out-hi
; RUN: opt -S %t-dump-hi/*.bc | FileCheck %s --check-prefix=HI
;
; Closure helpers at or above -ejit-externalize-min-insts (default 16)
; instructions are externalized: the extracted bitcode keeps only a
; declaration, and the
; AOT side registers the original body so the isolated spec JITDylib (no
; dlsym fallback) can resolve it. Internal helpers are renamed to
; ejit_static.<basename>.<full-64bit-hash>.<name> — internal names are
; unique per module only, two TUs can both define `static helper`, and the
; runtime's flat symbol table would otherwise bind a JIT'd call to the
; wrong TU; the hash of the raw module path keeps sanitized names
; collision-free.
; Externally-linked helpers keep their process-unique name. Helpers below
; the threshold stay as definitions: the registration record (~160 bytes)
; costs more than the body. See jit_design_doc/EJIT_BITCODE_SLIMMING.md.
;
; All helpers except hint_big are noinline so preOptimizeBitcode's inliner
; does not consume them (mirrors the -finline-hint-functions world, where
; non-inline functions carry noinline). hint_big is an inlinehint helper
; above the threshold: preopt inlines it away, the extraction skips it, and
; the AOT side still registers it (harmless dead entry, by design).
;
; NOTE: the pass mutates only its extraction clone; the -S output (MOD) is
; the ORIGINAL module plus the embedded bitcode, the ejit_auto_register
; calls, and the .ejit_bitcode registry section.

@cell_data = global [16 x i32] zeroinitializer, !ejit.metadata !10

; big static helper (>= 16 inst) -> externalized + renamed
define internal i32 @st_helper_big(i32 %x) #0 {
entry:
  %a0 = add i32 %x, 1
  %a1 = mul i32 %a0, 3
  %a2 = xor i32 %a1, %x
  %a3 = sub i32 %a2, 5
  %a4 = or i32 %a3, %a0
  %a5 = shl i32 %a4, 1
  %a6 = lshr i32 %a5, 2
  %a7 = and i32 %a6, %a1
  %a8 = add i32 %a7, %a2
  %a9 = mul i32 %a8, %a3
  %a10 = xor i32 %a9, %a4
  %a11 = sub i32 %a10, %a5
  %a12 = or i32 %a11, %a6
  %a13 = shl i32 %a12, 3
  %a14 = lshr i32 %a13, 1
  %a15 = and i32 %a14, %a7
  %a16 = add i32 %a15, %a8
  %a17 = mul i32 %a16, %a9
  %a18 = xor i32 %a17, %a10
  ret i32 %a18
}

; inlinehint helper above the threshold: consumed by preopt's inliner, but
; still registered on the AOT side (dead entry, by design)
define internal i32 @hint_big(i32 %x) #1 {
entry:
  %h0 = add i32 %x, 2
  %h1 = mul i32 %h0, 5
  %h2 = xor i32 %h1, %x
  %h3 = sub i32 %h2, 7
  %h4 = or i32 %h3, %h0
  %h5 = shl i32 %h4, 2
  %h6 = lshr i32 %h5, 3
  %h7 = and i32 %h6, %h1
  %h8 = add i32 %h7, %h2
  %h9 = mul i32 %h8, %h3
  %h10 = xor i32 %h9, %h4
  %h11 = sub i32 %h10, %h5
  %h12 = or i32 %h11, %h6
  %h13 = shl i32 %h12, 1
  %h14 = lshr i32 %h13, 2
  %h15 = and i32 %h14, %h7
  %h16 = add i32 %h15, %h8
  %h17 = mul i32 %h16, %h9
  ret i32 %h17
}

; small static helper (< 16 inst) -> kept as a definition
define internal i32 @st_helper_small(i32 %x) #0 {
entry:
  %s0 = xor i32 %x, 85
  %s1 = add i32 %s0, 1
  ret i32 %s1
}

; big external helper -> externalized, name kept
define i32 @ext_helper_big(i32 %x) #0 {
entry:
  %b0 = add i32 %x, 2
  %b1 = mul i32 %b0, 5
  %b2 = xor i32 %b1, %x
  %b3 = sub i32 %b2, 7
  %b4 = or i32 %b3, %b0
  %b5 = shl i32 %b4, 2
  %b6 = lshr i32 %b5, 3
  %b7 = and i32 %b6, %b1
  %b8 = add i32 %b7, %b2
  %b9 = mul i32 %b8, %b3
  %b10 = xor i32 %b9, %b4
  %b11 = sub i32 %b10, %b5
  %b12 = or i32 %b11, %b6
  %b13 = shl i32 %b12, 1
  %b14 = lshr i32 %b13, 2
  %b15 = and i32 %b14, %b7
  %b16 = add i32 %b15, %b8
  %b17 = mul i32 %b16, %b9
  ret i32 %b17
}

; small external helper -> kept as a definition
define i32 @ext_helper_small(i32 %x) #0 {
entry:
  %e0 = mul i32 %x, 3
  %e1 = add i32 %e0, 1
  ret i32 %e1
}

; exactly at the threshold (15 non-terminator + ret = 16) -> externalized
define i32 @boundary_helper(i32 %x) #0 {
entry:
  %c1 = add i32 %x, 1
  %c2 = add i32 %c1, 1
  %c3 = add i32 %c2, 1
  %c4 = add i32 %c3, 1
  %c5 = add i32 %c4, 1
  %c6 = add i32 %c5, 1
  %c7 = add i32 %c6, 1
  %c8 = add i32 %c7, 1
  %c9 = add i32 %c8, 1
  %c10 = add i32 %c9, 1
  %c11 = add i32 %c10, 1
  %c12 = add i32 %c11, 1
  %c13 = add i32 %c12, 1
  %c14 = add i32 %c13, 1
  %c15 = add i32 %c14, 1
  ret i32 %c15
}

; just below the threshold (14 non-terminator + ret = 15) -> kept
define i32 @kept_15inst(i32 %x) #0 {
entry:
  %k1 = add i32 %x, 1
  %k2 = add i32 %k1, 1
  %k3 = add i32 %k2, 1
  %k4 = add i32 %k3, 1
  %k5 = add i32 %k4, 1
  %k6 = add i32 %k5, 1
  %k7 = add i32 %k6, 1
  %k8 = add i32 %k7, 1
  %k9 = add i32 %k8, 1
  %k10 = add i32 %k9, 1
  %k11 = add i32 %k10, 1
  %k12 = add i32 %k11, 1
  %k13 = add i32 %k12, 1
  %k14 = add i32 %k13, 1
  ret i32 %k14
}

define i32 @entry(i32 %idx) !ejit.metadata !20 {
entry:
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %idx
  %v = load i32, ptr %p, !ejit.may_const !{}
  %r1 = call i32 @st_helper_big(i32 %v)
  %r2 = call i32 @st_helper_small(i32 %r1)
  %r3 = call i32 @ext_helper_big(i32 %r2)
  %r4 = call i32 @ext_helper_small(i32 %r3)
  %r5 = call i32 @hint_big(i32 %r4)
  %r6 = call i32 @boundary_helper(i32 %r5)
  %r7 = call i32 @kept_15inst(i32 %r6)
  ret i32 %r7
}

; an entry above the threshold must never be externalized
define internal i32 @entry_big(i32 %idx) !ejit.metadata !21 {
entry:
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %idx
  %v = load i32, ptr %p, !ejit.may_const !{}
  %j1 = add i32 %v, 1
  %j2 = add i32 %j1, 1
  %j3 = add i32 %j2, 1
  %j4 = add i32 %j3, 1
  %j5 = add i32 %j4, 1
  %j6 = add i32 %j5, 1
  %j7 = add i32 %j6, 1
  %j8 = add i32 %j7, 1
  %j9 = add i32 %j8, 1
  %j10 = add i32 %j9, 1
  %j11 = add i32 %j10, 1
  %j12 = add i32 %j11, 1
  %j13 = add i32 %j12, 1
  %j14 = add i32 %j13, 1
  %j15 = add i32 %j14, 1
  %j16 = add i32 %j15, 1
  %j17 = add i32 %j16, 1
  ret i32 %j17
}

attributes #0 = { noinline }
attributes #1 = { inlinehint }

!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}, !{!"ejit_may_const_field", i32 0}}
!20 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!21 = distinct !{!{!"ejit_entry"}}

; MOD side: registration records for the externalized helpers. The module
; name comes from stdin ("<stdin>", basename sanitized to "_stdin_"), so
; internal-helper keys are ejit_static._stdin_.<full-64bit-hash>.<name> —
; the hash segment is checked loosely.
; ToExternalize discovery order is stack-reversed (boundary, hint_big,
; ext_helper_big, st_helper_big), hence the ordered section checks.
; MOD: c"boundary_helper\00"
; MOD: [48 x i8] c"ejit_static._stdin_.{{0x[0-9a-f]+}}.hint_big\00"
; MOD: c"ext_helper_big\00"
; MOD: [53 x i8] c"ejit_static._stdin_.{{0x[0-9a-f]+}}.st_helper_big\00"
; MOD: c"ejit_static._stdin_.{{0x[0-9a-f]+}}.entry_big\00"
; MOD: { i32 3, ptr @{{.*}}, ptr null, ptr @boundary_helper, i64 0 }
; MOD: { i32 3, ptr @{{.*}}, ptr null, ptr @hint_big, i64 0 }
; MOD: { i32 3, ptr @{{.*}}, ptr null, ptr @ext_helper_big, i64 0 }
; MOD: { i32 3, ptr @{{.*}}, ptr null, ptr @st_helper_big, i64 0 }
; MOD-NOT: ptr @st_helper_small
; MOD-NOT: ptr @ext_helper_small
; MOD-NOT: ptr @kept_15inst
; MOD: define internal void @ejit_auto_register() {
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @entry)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @entry_big)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @st_helper_big)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @ext_helper_big)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @hint_big)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @boundary_helper)
; MOD-NOT: @st_helper_small
; MOD-NOT: @ext_helper_small
; MOD-NOT: @kept_15inst
; MOD: }

; EXT side: the extracted bitcode. Big helpers are declarations (the static
; one renamed to its registration key); small helpers stay as internal
; definitions; entries keep their bodies; hint_big is gone (inlined).
; EXT: declare i32 @ejit_static._stdin_.{{0x[0-9a-f]+}}.st_helper_big(i32)
; EXT: define internal i32 @st_helper_small(i32 %x)
; EXT: declare i32 @ext_helper_big(i32)
; EXT: define internal i32 @ext_helper_small(i32 %x)
; EXT: declare i32 @boundary_helper(i32)
; EXT: define internal i32 @kept_15inst(i32 %x)
; EXT: define i32 @entry(i32 %idx) {{.*}} {
; EXT: tail call i32 @ejit_static._stdin_.{{0x[0-9a-f]+}}.st_helper_big(i32 %v)
; EXT: tail call i32 @st_helper_small(i32 %r1)
; EXT: tail call i32 @ext_helper_big(i32 %r2)
; EXT: tail call i32 @ext_helper_small(i32 %r3)
; EXT: tail call i32 @boundary_helper(i32 {{.*}})
; EXT: tail call i32 @kept_15inst(i32 {{.*}})
; EXT: define internal i32 @entry_big(i32 %idx) #[[ENTRY_ATTR:[0-9]+]] {{.*}} {
; EXT: attributes #[[ENTRY_ATTR]] = { {{.*}}"ejit.wrapper_symbol"="ejit_static._stdin_.{{0x[0-9a-f]+}}.entry_big"{{.*}} }
; EXT-NOT: @st_helper_big
; EXT-NOT: @hint_big

; HI side: -ejit-externalize-min-insts=1000 removes the size floor — even
; the big helpers stay as definitions in the bitcode (the internalize step
; has run, so both are internal).
; HI: define internal i32 @st_helper_big(i32 %x)
; HI: define internal i32 @ext_helper_big(i32 %x)
