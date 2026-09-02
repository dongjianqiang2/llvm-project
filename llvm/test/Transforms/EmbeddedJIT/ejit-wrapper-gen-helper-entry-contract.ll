; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s | FileCheck %s --check-prefix=WRAP
; RUN: opt -passes=ejit-register-bitcode -S %s | FileCheck %s --check-prefix=REG

; A direct helper that participates in bound-pointer propagation must own both
; a separately registered EJIT entry identity and its bound-pointer contract.
; The ordinary helper below is reachable from the closure, but it must not
; acquire an independent wrapper/cache identity merely because an entry calls
; it.

; WRAP-DAG: @__ejit_funcidx_root = internal global i32 -1
; WRAP-DAG: @__ejit_funcidx_helper = internal global i32 -1
; WRAP-DAG: @__ejit_icache_fn_root = internal global [16 x ptr] zeroinitializer
; WRAP-DAG: @__ejit_icache_fn_helper = internal global [16 x ptr] zeroinitializer
; WRAP-LABEL: define i32 @root(i8 %cell, ptr %cfg)
; WRAP: jit_entry:
; WRAP: call i32 @ejit_taskpool_compile_or_get_bound
; WRAP-LABEL: define i32 @helper(i8 %cell, ptr %cfg)
; WRAP: jit_entry:
; WRAP: call i32 @ejit_taskpool_compile_or_get_bound
; WRAP-LABEL: define internal i32 @plain_helper(ptr %cfg)
; WRAP-NEXT: entry:
; WRAP-NOT: jit_entry

; REG-COUNT-2: call void @ejit_register_bitcode
; REG-DAG: c"root\\00"
; REG-DAG: c"helper\\00"
; REG-NOT: c"plain_helper\\00"

%Cfg = type { i32 }
@cfg = global %Cfg zeroinitializer, !ejit.metadata !10

define i32 @root(i8 %cell, ptr %cfg) !ejit.metadata !20 {
entry:
  %result = call i32 @helper(i8 %cell, ptr %cfg)
  ret i32 %result
}

define i32 @helper(i8 %cell, ptr %cfg) !ejit.metadata !21 {
entry:
  %result = call i32 @plain_helper(ptr %cfg)
  ret i32 %result
}

define internal i32 @plain_helper(ptr %cfg) {
entry:
  %value = load i32, ptr %cfg
  ret i32 %value
}

!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!20 = distinct !{!{!"ejit_entry"},
                  !{!"ejit_period_arr_ind", !"cell", i32 0},
                  !{!"ejit_bound_ptr", !"cell", i32 1, i64 4}}
!21 = distinct !{!{!"ejit_entry"},
                  !{!"ejit_period_arr_ind", !"cell", i32 0},
                  !{!"ejit_bound_ptr", !"cell", i32 1, i64 4}}
