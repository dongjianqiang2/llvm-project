; RUN: split-file %s %t
; RUN: rm -rf %t/basic-dump && mkdir -p %t/basic-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t/basic-dump -S %t/basic.ll -o %t/basic.aot.ll
; RUN: opt -S %t/basic-dump/*.bc | FileCheck %s --check-prefix=BASIC
; RUN: rm -rf %t/ext-dump && mkdir -p %t/ext-dump
; RUN: opt -passes=ejit-register-bitcode -enable-ejit-global-ctors=1 -ejit-dump-bitcode-dir=%t/ext-dump -S %t/externalize.ll -o %t/ext.aot.ll
; RUN: opt -S %t/ext-dump/*.bc | FileCheck %s --check-prefix=EXT
; RUN: FileCheck %s --check-prefix=EXT-MOD --input-file=%t/ext.aot.ll
; RUN: rm -rf %t/zero-dump && mkdir -p %t/zero-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-externalize-min-insts=0 -ejit-dump-bitcode-dir=%t/zero-dump -S %t/externalize.ll -o /dev/null
; RUN: opt -S %t/zero-dump/*.bc | FileCheck %s --check-prefix=ZERO
; RUN: rm -rf %t/alias-dump && mkdir -p %t/alias-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t/alias-dump -S %t/alias-root.ll -o /dev/null
; RUN: opt -S %t/alias-dump/*.bc | FileCheck %s --check-prefix=ALIAS
; RUN: rm -rf %t/alias-zero-dump && mkdir -p %t/alias-zero-dump
; RUN: opt -passes=ejit-register-bitcode -enable-ejit-global-ctors=1 -ejit-externalize-min-insts=0 -ejit-dump-bitcode-dir=%t/alias-zero-dump -S %t/alias-root.ll -o %t/alias-zero.aot.ll
; RUN: opt -S %t/alias-zero-dump/*.bc | FileCheck %s --check-prefix=ALIAS-ZERO
; RUN: FileCheck %s --check-prefix=ALIAS-ZERO-MOD --input-file=%t/alias-zero.aot.ll
; RUN: rm -rf %t/mutable-dump && mkdir -p %t/mutable-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t/mutable-dump -S %t/mutable.ll -o %t/mutable.aot.ll
; RUN: opt -S %t/mutable-dump/*.bc | FileCheck %s --check-prefix=MUTABLE
; RUN: FileCheck %s --check-prefix=MUTABLE-MOD --input-file=%t/mutable.aot.ll
; RUN: rm -rf %t/mutable-alias-dump && mkdir -p %t/mutable-alias-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t/mutable-alias-dump -S %t/mutable-alias.ll -o %t/mutable-alias.aot.ll
; RUN: opt -passes=verify -disable-output %t/mutable-alias-dump/*.bc
; RUN: opt -S %t/mutable-alias-dump/*.bc | FileCheck %s --check-prefix=MUTABLE-ALIAS
; RUN: FileCheck %s --check-prefix=MUTABLE-ALIAS-MOD --input-file=%t/mutable-alias.aot.ll
; RUN: rm -rf %t/function-alias-ext-dump && mkdir -p %t/function-alias-ext-dump
; RUN: opt -passes=ejit-register-bitcode -enable-ejit-global-ctors=1 -ejit-externalize-min-insts=0 -ejit-dump-bitcode-dir=%t/function-alias-ext-dump -S %t/function-alias.ll -o %t/function-alias-ext.aot.ll
; RUN: opt -passes=verify -disable-output %t/function-alias-ext-dump/*.bc
; RUN: opt -S %t/function-alias-ext-dump/*.bc | FileCheck %s --check-prefix=FUNCTION-ALIAS-EXT
; RUN: FileCheck %s --check-prefix=FUNCTION-ALIAS-EXT-MOD --input-file=%t/function-alias-ext.aot.ll
; RUN: rm -rf %t/function-alias-keep-dump && mkdir -p %t/function-alias-keep-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-externalize-min-insts=100 -ejit-dump-bitcode-dir=%t/function-alias-keep-dump -S %t/function-alias.ll -o /dev/null
; RUN: opt -passes=verify -disable-output %t/function-alias-keep-dump/*.bc
; RUN: opt -S %t/function-alias-keep-dump/*.bc | FileCheck %s --check-prefix=FUNCTION-ALIAS-KEEP
;
; A reachable global initializer is part of the specialization closure. Follow
; constant-expression casts, nested aggregate/global chains, and aliases to
; defined function targets. Unreachable tables must not retain their targets.

; BASIC: @handlers = internal constant %table_t { [2 x ptr] [ptr @func1, ptr @func2_alias] }
; BASIC: @handler_root = internal constant ptr @handlers
; BASIC: @cycle_a = internal constant ptr @cycle_b
; BASIC: @cycle_b = internal constant ptr @cycle_a
; BASIC: @func2_alias = internal alias i32 (i32, i32), ptr @func2
; BASIC-DAG: define internal {{.*}}i32 @func1(
; BASIC-DAG: define internal {{.*}}i32 @func2(
; BASIC-NOT: @unused_table
; BASIC-NOT: @unused_func
; BASIC-NOT: undef

; The default threshold keeps a small target in the bitcode and externalizes
; the large target under the same deterministic key used by both AOT-side
; registration paths.
; EXT: @handlers = internal constant [2 x ptr] [ptr @small_target, ptr @[[LARGE:ejit_static\.[^ ]+\.large_target]]]
; EXT-DAG: define internal {{.*}}i32 @small_target(
; EXT-DAG: declare {{.*}}i32 @[[LARGE]](
; EXT-NOT: undef
; EXT-MOD: @llvm.global_ctors = appending global
; EXT-MOD: @.ejit.registry.bitcode = private constant
; EXT-MOD: call void @ejit_register_symbol({{.*}}ptr @large_target)

; ZERO: @handlers = internal constant [2 x ptr] [ptr @[[SMALL:ejit_static\.[^ ]+\.small_target]], ptr @[[LARGE0:ejit_static\.[^ ]+\.large_target]]]
; ZERO-DAG: declare {{.*}}i32 @[[SMALL]](
; ZERO-DAG: declare {{.*}}i32 @[[LARGE0]](
; ZERO-NOT: undef

; A whole-table alias used directly by the entry must root the underlying
; constant global and its initializer-only target.
; ALIAS: @handlers = internal constant [1 x ptr] [ptr @target]
; ALIAS: @handlers_alias = internal alias [1 x ptr], ptr @handlers
; ALIAS: define internal {{.*}}i32 @target(
; ALIAS-NOT: undef
; ALIAS-ZERO: @handlers = internal constant [1 x ptr] [ptr @[[TARGET:ejit_static\.[^ ]+\.target]]]
; ALIAS-ZERO: @handlers_alias = internal alias [1 x ptr], ptr @handlers
; ALIAS-ZERO: declare {{.*}}i32 @[[TARGET]](
; ALIAS-ZERO-NOT: undef
; ALIAS-ZERO-MOD: @llvm.global_ctors = appending global
; ALIAS-ZERO-MOD: c"{{ejit_static\..*\.target}}\00"
; ALIAS-ZERO-MOD: @.ejit.registry.bitcode = private constant
; ALIAS-ZERO-MOD: call void @ejit_register_symbol({{.*}}ptr @target)

; A mutable table becomes an external declaration in the serialized module.
; Its host initializer must not pull host_target into the JIT closure, while
; the AOT module and global-symbol registration stay intact.
; MUTABLE: @mutable_handlers = external {{.*}}global [1 x ptr]
; MUTABLE-NOT: @host_target
; MUTABLE-NOT: undef
; MUTABLE-MOD: @mutable_handlers = internal global [1 x ptr] [ptr @host_target]
; MUTABLE-MOD: c"mutable_handlers\00"
; MUTABLE-MOD: @.ejit.registry.bitcode = private constant
; MUTABLE-MOD: define internal i32 @host_target(
; MUTABLE-MOD: call void @ejit_register_symbol({{.*}}ptr @mutable_handlers)

; An alias rooted at a mutable table must be dissolved in the extracted clone
; before the table becomes a declaration. The entry keeps the exact non-zero
; GEP offset, while the AOT alias and mutable initializer remain untouched.
; MUTABLE-ALIAS: @mutable_handlers = external {{.*}}global [2 x ptr]
; MUTABLE-ALIAS-NOT: @mutable_handlers_alias
; MUTABLE-ALIAS-NOT: @mutable_handlers_alias_chain
; MUTABLE-ALIAS: getelementptr inbounds ([2 x ptr], ptr @mutable_handlers, i64 0, i64 1)
; MUTABLE-ALIAS-NOT: @host_target
; MUTABLE-ALIAS-NOT: undef
; MUTABLE-ALIAS-MOD: @mutable_handlers = internal global [2 x ptr] [ptr @host_target, ptr @host_target]
; MUTABLE-ALIAS-MOD: c"mutable_handlers\00"
; MUTABLE-ALIAS-MOD: @mutable_handlers_alias = internal alias [1 x ptr], getelementptr inbounds ([2 x ptr], ptr @mutable_handlers, i64 0, i64 1)
; MUTABLE-ALIAS-MOD: @mutable_handlers_alias_chain = internal alias [1 x ptr], ptr @mutable_handlers_alias
; MUTABLE-ALIAS-MOD: call void @ejit_register_symbol({{.*}}ptr @mutable_handlers)

; Function aliases rooted at a helper externalized from the clone must be
; dissolved there, otherwise the alias points at a declaration. Alias chains
; remain intact when the threshold keeps the helper body. AOT aliases and the
; original table are unchanged in either case.
; FUNCTION-ALIAS-EXT: @handlers = internal constant [1 x ptr] [ptr @[[EXT_TARGET:ejit_static\.[^ ]+\.target]]]
; FUNCTION-ALIAS-EXT-NOT: @target_alias
; FUNCTION-ALIAS-EXT-NOT: @target_alias_chain
; FUNCTION-ALIAS-EXT: declare {{.*}}i32 @[[EXT_TARGET]](
; FUNCTION-ALIAS-EXT-NOT: undef
; FUNCTION-ALIAS-EXT-MOD-DAG: @handlers = internal constant [1 x ptr] [ptr @target_alias_chain]
; FUNCTION-ALIAS-EXT-MOD-DAG: @target_alias = internal alias i32 (i32), ptr @target
; FUNCTION-ALIAS-EXT-MOD-DAG: @target_alias_chain = internal alias i32 (i32), ptr @target_alias
; FUNCTION-ALIAS-EXT-MOD-DAG: c"{{ejit_static\..*\.target}}\00"
; FUNCTION-ALIAS-EXT-MOD-DAG: @.ejit.registry.bitcode = private constant {{.*}}ptr @target
; FUNCTION-ALIAS-EXT-MOD: call void @ejit_register_symbol({{.*}}ptr @target)
; FUNCTION-ALIAS-KEEP: @handlers = internal constant [1 x ptr] [ptr @target_alias_chain]
; FUNCTION-ALIAS-KEEP: @target_alias = internal alias i32 (i32), ptr @target
; FUNCTION-ALIAS-KEEP: @target_alias_chain = internal alias i32 (i32), ptr @target_alias
; FUNCTION-ALIAS-KEEP: define internal {{.*}}i32 @target(
; FUNCTION-ALIAS-KEEP-NOT: undef

;--- basic.ll
source_filename = "basic.c"
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"

%table_t = type { [2 x ptr] }

@handlers = internal constant %table_t { [2 x ptr] [ptr @func1, ptr @func2_alias] }
@handler_root = internal constant ptr getelementptr inbounds (%table_t, ptr @handlers, i64 0, i32 0)
@cycle_a = internal constant ptr @cycle_b
@cycle_b = internal constant ptr @cycle_a
@unused_table = internal constant [1 x ptr] [ptr @unused_func]

@func2_alias = internal alias i32 (i32, i32), ptr @func2

define internal i32 @func1(i32 %cell, i32 %value) noinline {
  %r = add i32 %value, %cell
  ret i32 %r
}

define internal i32 @func2(i32 %cell, i32 %value) noinline {
  %base = add i32 %value, %cell
  %r = add i32 %base, 100
  ret i32 %r
}

define internal i32 @unused_func(i32 %value) noinline {
  %r = add i32 %value, 999
  ret i32 %r
}

define i32 @entry(i32 %cell, i32 %which, i32 %value) !ejit.metadata !0 {
  %cycle = load ptr, ptr @cycle_a
  %cycle_is_null = icmp eq ptr %cycle, null
  %bias = select i1 %cycle_is_null, i32 1, i32 0
  %table = load ptr, ptr @handler_root
  %slot = and i32 %which, 1
  %slot64 = zext i32 %slot to i64
  %addr = getelementptr [2 x ptr], ptr %table, i64 0, i64 %slot64
  %target = load ptr, ptr %addr
  %result = call i32 %target(i32 %cell, i32 %value)
  %with_bias = add i32 %result, %bias
  ret i32 %with_bias
}

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}

;--- externalize.ll
source_filename = "externalize.c"
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"

@handlers = internal constant [2 x ptr] [ptr @small_target, ptr @large_target]

define internal i32 @small_target(i32 %value) noinline {
  %r = add i32 %value, 1
  ret i32 %r
}

define internal i32 @large_target(i32 %value) noinline {
  %v01 = add i32 %value, 1
  %v02 = add i32 %v01, 2
  %v03 = add i32 %v02, 3
  %v04 = add i32 %v03, 4
  %v05 = add i32 %v04, 5
  %v06 = add i32 %v05, 6
  %v07 = add i32 %v06, 7
  %v08 = add i32 %v07, 8
  %v09 = add i32 %v08, 9
  %v10 = add i32 %v09, 10
  %v11 = add i32 %v10, 11
  %v12 = add i32 %v11, 12
  %v13 = add i32 %v12, 13
  %v14 = add i32 %v13, 14
  %v15 = add i32 %v14, 15
  %r = add i32 %v15, 16
  ret i32 %r
}

define i32 @entry(i32 %which, i32 %value) !ejit.metadata !1 {
  %slot = and i32 %which, 1
  %slot64 = zext i32 %slot to i64
  %addr = getelementptr [2 x ptr], ptr @handlers, i64 0, i64 %slot64
  %target = load ptr, ptr %addr
  %result = call i32 %target(i32 %value)
  ret i32 %result
}

!1 = distinct !{!{!"ejit_entry"}}

;--- alias-root.ll
source_filename = "alias-root.c"
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"

@handlers = internal constant [1 x ptr] [ptr @target]
@handlers_alias = internal alias [1 x ptr], ptr @handlers

define internal i32 @target(i32 %value) noinline {
  %r = add i32 %value, 7
  ret i32 %r
}

define i32 @entry(i32 %value) !ejit.metadata !2 {
  %addr = getelementptr [1 x ptr], ptr @handlers_alias, i64 0, i64 0
  %target = load ptr, ptr %addr
  %result = call i32 %target(i32 %value)
  ret i32 %result
}

!2 = distinct !{!{!"ejit_entry"}}

;--- mutable.ll
source_filename = "mutable.c"
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"

@mutable_handlers = internal global [1 x ptr] [ptr @host_target]

define internal i32 @host_target(i32 %value) noinline {
  %r = add i32 %value, 9
  ret i32 %r
}

define i32 @entry(i32 %value) !ejit.metadata !3 {
  %addr = getelementptr [1 x ptr], ptr @mutable_handlers, i64 0, i64 0
  %target = load ptr, ptr %addr
  %result = call i32 %target(i32 %value)
  ret i32 %result
}

!3 = distinct !{!{!"ejit_entry"}}

;--- mutable-alias.ll
source_filename = "mutable-alias.c"
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"

@mutable_handlers = internal global [2 x ptr] [ptr @host_target, ptr @host_target]
@mutable_handlers_alias = internal alias [1 x ptr], getelementptr inbounds ([2 x ptr], ptr @mutable_handlers, i64 0, i64 1)
@mutable_handlers_alias_chain = internal alias [1 x ptr], ptr @mutable_handlers_alias

define internal i32 @host_target(i32 %value) noinline {
  %r = add i32 %value, 9
  ret i32 %r
}

define i32 @entry(i32 %value) !ejit.metadata !4 {
  %slot = getelementptr [1 x ptr], ptr @mutable_handlers_alias_chain, i64 0, i64 0
  %target = load ptr, ptr %slot
  %result = call i32 %target(i32 %value)
  ret i32 %result
}

!4 = distinct !{!{!"ejit_entry"}}

;--- function-alias.ll
source_filename = "function-alias.c"
target datalayout = "e-m:e-p:64:64-i64:64-n8:16:32:64-S128"

@target_alias = internal alias i32 (i32), ptr @target
@target_alias_chain = internal alias i32 (i32), ptr @target_alias
@handlers = internal constant [1 x ptr] [ptr @target_alias_chain]

define internal i32 @target(i32 %value) noinline {
  %r = add i32 %value, 11
  ret i32 %r
}

define i32 @entry(i32 %value) !ejit.metadata !5 {
  %slot = getelementptr [1 x ptr], ptr @handlers, i64 0, i64 0
  %target = load ptr, ptr %slot
  %result = call i32 %target(i32 %value)
  ret i32 %result
}

!5 = distinct !{!{!"ejit_entry"}}
