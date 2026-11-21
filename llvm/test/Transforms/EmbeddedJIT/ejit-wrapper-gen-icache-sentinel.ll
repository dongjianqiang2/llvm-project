; Sentinel-form selection: NumDims <= 2 (and no -ejit-wrapper-timing) gets the
; branchless wrapper whose cell table is DEFINED pre-filled with &MissFn;
; 3D/4D keep the historical guarded shape - a [D]^N splat initializer costs
; 4096/65536 relocations per table at D=16, so their tables stay zero-init and
; the null guard stays load-bearing. The registration's 4th argument tells the
; runtime which empty value to write back on drain / fill-retract: &MissFn for
; sentinel slots, null (0) for guarded ones.

; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s | FileCheck %s

; --- Cell tables + static .ejit_group registry entries (globals section):
;     the 0D table is defined pre-filled with its MissFn sentinel and its
;     registry entry carries the same pointer in name2 (3rd field); the 3D
;     table stays zero-init and its entry passes null. ---
; CHECK-DAG: @__ejit_icache_fn_zero_dim_entry = internal global ptr @zero_dim_entry_miss, align 8
; CHECK-DAG: @__ejit_icache_fn_three_dim_entry = internal global [16 x [16 x [16 x ptr]]] zeroinitializer, align 8
; CHECK-DAG: { i32 {{[0-9]+}}, ptr {{[^{]*}}, ptr @zero_dim_entry_miss, ptr @__ejit_icache_fn_zero_dim_entry, i64 0 }
; CHECK-DAG: { i32 {{[0-9]+}}, ptr {{[^{]*}}, ptr null, ptr @__ejit_icache_fn_three_dim_entry, i64 3 }

; --- 0D wrapper: sentinel form - the whole wrapper is load + musttail, no
;     guard branch anywhere. ---
; CHECK-LABEL: define i32 @zero_dim_entry(
; CHECK-NOT: br
; CHECK: load atomic ptr, ptr @__ejit_icache_fn_zero_dim_entry monotonic, align 8
; CHECK-NOT: br
; CHECK: musttail call {{.*}} %ejit_ic_fn
; CHECK: ret

; --- 3D wrapper: guarded form retained - expect + condBr, dispatch and miss
;     blocks. ---
; CHECK-LABEL: define i32 @three_dim_entry(
; CHECK: load atomic ptr, ptr {{.*}} monotonic, align 8
; CHECK: call {{.*}} @llvm.expect
; CHECK: br i1 {{.*}}, label %jit_icache_dispatch, label %jit_miss
; CHECK-LABEL: jit_miss:
; CHECK: musttail call {{.*}} @three_dim_entry_miss

; --- Registration calls (auto_register body): the 0D slot carries its MissFn
;     sentinel as the 4th argument, the 3D slot passes null. ---
; CHECK-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_zero_dim_entry, i32 0, ptr @zero_dim_entry_miss)
; CHECK-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_three_dim_entry, i32 3, ptr null)

define i32 @zero_dim_entry(i32 %x) !ejit.metadata !0 {
entry:
  ret i32 %x
}

define i32 @three_dim_entry(i32 %a, i32 %b, i32 %c) !ejit.metadata !1 {
entry:
  %v = load i32, ptr @data
  ret i32 %v
}

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11
@data3 = global i32 0, !ejit.metadata !12

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_dim", !"p1", i32 0}, !{!"ejit_dim", !"p2", i32 1}, !{!"ejit_dim", !"p3", i32 2}}
!10 = distinct !{!{!"ejit_group_indexed", !"p1", i32 16}}
!11 = distinct !{!{!"ejit_group_indexed", !"p2", i32 16}}
!12 = distinct !{!{!"ejit_group_indexed", !"p3", i32 16}}
