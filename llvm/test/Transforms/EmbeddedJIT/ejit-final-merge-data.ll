; RUN: rm -rf %t.on %t.off %t.ctor && mkdir -p %t.on %t.off %t.ctor
; RUN: opt -passes='ejit-register-bitcode,verify' -ejit-final-version-merge -enable-ejit-global-ctors=0 -ejit-dump-bitcode-dir=%t.on -S %s | FileCheck %s --check-prefix=REG
; RUN: opt -passes=verify -S %t.on/*.bc | FileCheck %s --check-prefix=BC
; RUN: opt -passes='ejit-register-bitcode,verify' -ejit-final-version-merge -enable-ejit-global-ctors=1 -ejit-dump-bitcode-dir=%t.ctor -S %s | FileCheck %s --check-prefix=REG
; RUN: opt -passes=verify -S %t.ctor/*.bc | FileCheck %s --check-prefix=BC
; RUN: opt -passes='ejit-register-bitcode,verify' -ejit-dump-bitcode-dir=%t.off -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes=verify -S %t.off/*.bc | FileCheck %s --check-prefix=OLD
;
; REG: @[[KEY:__ejit_final_data\.[-a-zA-Z0-9._]+\.Foo\.identity]].regkey = hidden constant
; REG: @.ejit.registry.bitcode = private constant {{.*}}ptr @[[KEY]].regkey, ptr null, ptr @Foo.identity{{.*}}section ".ejit_bitcode"
; REG: call void @ejit_register_symbol(ptr @[[KEY]].regkey, ptr @Foo.identity)
; REG: call void @ejit_register_symbol({{.*}}, ptr @publicword)
; BC-DAG: @Foo.lut = internal {{.*}}constant [3 x i8]
; BC-DAG: @__ejit_final_data.{{.*}}.Foo.identity = external constant i8
; BC-DAG: @__ejit_final_data.{{.*}}.Foo.state = external global i8
; BC-DAG: @publicword = external constant i8
; BC: store ptr @__ejit_final_data.{{.*}}.Foo.identity
; OFF-NOT: __ejit_final_data.
; OFF: ret void
; OFF-NOT: __ejit_final_data.
; OLD-DAG: @Foo.identity = internal constant i8 11
; OLD-DAG: @Foo.lut = internal {{.*}}constant [3 x i8]

@Foo.identity = internal constant i8 11
@Foo.state = internal global i8 17
@publicword = constant i8 33
@Foo.lut = internal constant [3 x i8] c"z81"

define i8 @Foo(i32 %cell, i32 %live, ptr %out) !ejit.metadata !0 {
  store ptr @Foo.identity, ptr %out
  %idx = urem i32 %live, 3
  %ptr = getelementptr inbounds [3 x i8], ptr @Foo.lut, i32 0, i32 %idx
  %v = load i8, ptr %ptr
  %old = load i8, ptr @Foo.state
  %next = add i8 %old, %v
  store i8 %next, ptr @Foo.state
  %word = load volatile i8, ptr @publicword
  %result = add i8 %next, %word
  ret i8 %result
}

!0 = !{!1, !2}
!1 = !{!"ejit_entry"}
!2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
