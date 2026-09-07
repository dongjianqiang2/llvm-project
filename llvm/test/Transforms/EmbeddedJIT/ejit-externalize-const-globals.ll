; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S < %s > %t.out
; RUN: FileCheck %s --check-prefix=MOD < %t.out
; RUN: opt -S %t-dump/*.bc | FileCheck %s --check-prefix=EXT
;
; Constant globals are externalized out of the extracted bitcode.
;
; A const definition kept in the extracted bitcode materializes as JIT-side
; .rodata: every adrp+ldr that survives pre-optimization reads a private
; copy instead of the AOT original. extractAndSerialize therefore turns every
; surviving const definition into an external declaration, and both
; registration emitters record it so the JIT linker resolves the name to the
; AOT image's own rodata. Internal constants (static const, private string
; literals) are renamed to their deterministic ejit_static.* key — module
; -local names are not process-unique and the runtime's flat symbol table
; would collide across TUs. Externally linked constants keep their name.
;
; NOTE: the EXT assertions assume preOptimizeBitcode actually RUNS: it is
; #ifdef NDEBUG-guarded (cyclic link dependency in debug/shared builds), so
; a debug opt is a no-op there - the entry body then still folds nothing,
; but the externalization below does not depend on preopt. Run the EJIT lit
; suite with a release/NDEBUG opt (see CLAUDE.md's note on debug builds).
;
; NOTE: the pass mutates only its extraction clone; the -S output (MOD) is
; the ORIGINAL module plus the embedded bitcode, the ejit_auto_register
; calls, and the .ejit_bitcode registry section.

; Externally linked const — registered under its own (unique) name.
@msg = constant [11 x i8] c"helloworld\00", align 1

; Internal const array — renamed to the deterministic key.
@tbl = internal constant [4 x i32] [i32 10, i32 20, i32 30, i32 40]

; Extern const (declaration in this TU) — was already registered before
; const externalization; must stay registered.
@g_ext_const = external constant i32

; Mutable global — unchanged behavior: externalized as before, registered.
@counter = global i32 0

define i32 @entry(i32 %i) !ejit.metadata !1 {
entry:
  %pi = getelementptr [11 x i8], ptr @msg, i32 0, i32 %i
  %c = load i8, ptr %pi
  %ci = zext i8 %c to i32
  %pt = getelementptr [4 x i32], ptr @tbl, i32 0, i32 %i
  %v = load i32, ptr %pt
  %e = load i32, ptr @g_ext_const
  %n = load i32, ptr @counter
  %s1 = add i32 %n, %ci
  %s2 = add i32 %s1, %v
  %s3 = add i32 %s2, %e
  store i32 %s3, ptr @counter
  ret i32 %s3
}

; MOD side: every closure global gets a registration record so the JIT
; linker resolves each declaration to the AOT original.
; MOD: define internal void @ejit_auto_register()
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @msg)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @g_ext_const)
; MOD-DAG: call void @ejit_register_symbol(ptr @{{.*}}, ptr @counter)

; EXT side: the extracted bitcode declares (does not define) every const;
; the internal array is renamed to its deterministic key.
; EXT: @msg = external constant
; EXT: @ejit_static._stdin_.{{0x[0-9a-f]+}}.tbl = external constant
; EXT: @g_ext_const = external constant
; EXT: @counter = external global

; ...and no const initializer survives anywhere in the extracted module:
; EXT-NOT: c"helloworld\00"
; EXT-NOT: [i32 10, i32 20, i32 30, i32 40]

!0 = !{!"ejit_entry"}
!1 = distinct !{!0}
