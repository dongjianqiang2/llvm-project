; RUN: opt -passes=ejit-wrapper-gen -ejit-fixup-section= -enable-ejit-global-ctors -S %s | FileCheck %s --check-prefix=LOCAL
; RUN: opt -passes=ejit-wrapper-gen -ejit-fixup-section=.mc_shared -enable-ejit-global-ctors=false -S %s | FileCheck %s --check-prefix=SHARED --implicit-check-not=@llvm.global_ctors
; RUN: opt -passes=ejit-wrapper-gen -ejit-fixup-section=.mc_shared -ejit-icache-section=.mc_shared -ejit-inline-cache -enable-ejit-global-ctors=false -S %s | FileCheck %s --check-prefix=CACHE --implicit-check-not=@llvm.global_ctors

; LOCAL-DAG: @__ejit_dimtype_cell = internal global i32 -1{{$}}
; LOCAL-DAG: @__ejit_funcidx_probe = internal global i32 -1{{$}}
; LOCAL-DAG: @llvm.global_ctors =
; LOCAL: load i32, ptr @__ejit_funcidx_probe
; LOCAL: load i32, ptr @__ejit_dimtype_cell

; SHARED-DAG: @__ejit_dimtype_cell = internal global i32 -1, section ".mc_shared", align 4
; SHARED-DAG: @__ejit_funcidx_probe = internal global i32 -1, section ".mc_shared", align 4
; SHARED: load atomic i32, ptr @__ejit_funcidx_probe acquire, align 4
; SHARED: load atomic i32, ptr @__ejit_dimtype_cell acquire, align 4

; CACHE: @__ejit_icache_fn_probe = internal global {{.*}} zeroinitializer, section ".mc_shared"
; CACHE: load atomic ptr
; CACHE: icmp {{eq|ne}} ptr {{.*}}, null

define i32 @probe(i32 %cell) !ejit.metadata !0 {
  ret i32 %cell
}

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"cell", i32 0}}
