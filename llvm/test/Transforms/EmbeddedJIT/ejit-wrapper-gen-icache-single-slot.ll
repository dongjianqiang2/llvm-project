; Single-slot mode (-ejit-icache-single-slot): every entry's
; @__ejit_icache_fn_<name> collapses to ONE scalar cell (the 0-dim shape)
; regardless of how many ejit_dim params it marks -- the global is a plain
; ptr initialized to &MissFn, the probe is a single load + musttail with NO
; GEP and NO guard branch, and the registration carries numDims=0 with the
; sentinel, while MissFn still resolves through the taskpool with the REAL
; dim count (compile_or_get_1d / generic compile_or_get + dimtype loads
; unchanged). A 3-dim table that used to be [16 x [16 x [16 x ptr]]] (32 KB
; in the shared section, 4096 drain stores) is now 8 bytes / 1 store. 0-dim
; entries are byte-identical with or without the flag (TableDims=0 either way).
;
; Emission order factored into the checks: the cell globals follow source
; order, while the registration payload iterates the name-keyed map
; (alphabetical: one_dim, three_dim, zero_dim), and every MissFn is emitted
; after the last wrapper.

; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-single-slot -ejit-icache-section= -S %s | FileCheck %s --check-prefix=SINGLE
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-single-slot -ejit-icache-section= -ejit-wrapper-timing -S %s | FileCheck %s --check-prefix=TIMING
; RUN: opt -passes=ejit-wrapper-gen,ejit-wrapper-gen -ejit-inline-cache -ejit-icache-single-slot -ejit-icache-section= -S %s | FileCheck %s --check-prefix=IDEM
; --- single-slot x cluster: every table is an 8B scalar, so the derived
;     cluster region packs EIGHT cells per 64B line -- the best-case shape. ---
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-single-slot -ejit-icache-section=.mc_shared -ejit-icache-cluster -S %s | FileCheck %s --check-prefix=CLUSTER

; CLUSTER-DAG: @__ejit_icache_fn_zero_dim_entry = internal global ptr @zero_dim_entry_miss, section ".mc_shared.ejit_icache", align 8
; CLUSTER-DAG: @__ejit_icache_fn_one_dim_entry = internal global ptr @one_dim_entry_miss, section ".mc_shared.ejit_icache", align 8
; CLUSTER-DAG: @__ejit_icache_fn_three_dim_entry = internal global ptr @three_dim_entry_miss, section ".mc_shared.ejit_icache", align 8
; CLUSTER-LABEL: define i32 @three_dim_entry(
; CLUSTER-NOT: getelementptr
; CLUSTER: load atomic ptr, ptr @__ejit_icache_fn_three_dim_entry monotonic, align 8
; CLUSTER: musttail call {{.*}} %ejit_ic_fn

; --- Cell tables: scalar for EVERY entry, each defined pre-filled with its
;     own &MissFn (one cell, so the sentinel splat is always affordable --
;     the NumDims <= 2 clamp of the dimensioned form does not bind here). ---
; SINGLE-DAG: @__ejit_icache_fn_zero_dim_entry = internal global ptr @zero_dim_entry_miss, align 8
; SINGLE-DAG: @__ejit_icache_fn_one_dim_entry = internal global ptr @one_dim_entry_miss, align 8
; SINGLE-DAG: @__ejit_icache_fn_three_dim_entry = internal global ptr @three_dim_entry_miss, align 8

; --- Static .ejit_period registry entries (map order): the `size` (i64)
;     numDims field is 0 for every slot, name2 carries the sentinel. ---
; SINGLE-DAG: { i32 {{[0-9]+}}, ptr {{[^{]*}}, ptr @one_dim_entry_miss, ptr @__ejit_icache_fn_one_dim_entry, i64 0 }
; SINGLE-DAG: { i32 {{[0-9]+}}, ptr {{[^{]*}}, ptr @three_dim_entry_miss, ptr @__ejit_icache_fn_three_dim_entry, i64 0 }
; SINGLE-DAG: { i32 {{[0-9]+}}, ptr {{[^{]*}}, ptr @zero_dim_entry_miss, ptr @__ejit_icache_fn_zero_dim_entry, i64 0 }

; --- 0-dim entry: byte-identical to the no-flag shape (TableDims=0 both
;     ways) -- the sentinel-test 0D checks carry over verbatim. ---
; SINGLE-LABEL: define i32 @zero_dim_entry(
; SINGLE-NOT: getelementptr
; SINGLE-NOT: br
; SINGLE: load atomic ptr, ptr @__ejit_icache_fn_zero_dim_entry monotonic, align 8
; SINGLE-NOT: br
; SINGLE: musttail call {{.*}} %ejit_ic_fn
; SINGLE: ret

; --- 1-dim wrapper: scalar load straight out of the global, no GEP. ---
; SINGLE-LABEL: define i32 @one_dim_entry(
; SINGLE-NOT: getelementptr
; SINGLE-NOT: br
; SINGLE: load atomic ptr, ptr @__ejit_icache_fn_one_dim_entry monotonic, align 8
; SINGLE-NOT: br
; SINGLE: musttail call {{.*}} %ejit_ic_fn
; SINGLE: ret

; --- 3-dim wrapper: sentinel form for ANY real dim count -- one load, no
;     GEP, no branch, where the dimensioned form had a 3-deep GEP chain
;     (and, at > 2 dims, a guarded shape). ---
; SINGLE-LABEL: define i32 @three_dim_entry(
; SINGLE-NOT: getelementptr
; SINGLE-NOT: br
; SINGLE: load atomic ptr, ptr @__ejit_icache_fn_three_dim_entry monotonic, align 8
; SINGLE-NOT: br
; SINGLE: musttail call {{.*}} %ejit_ic_fn
; SINGLE: ret

; --- Registration calls (auto_register body, emitted after the wrappers,
;     map order): numDims 0 for every slot, sentinel non-null for every
;     slot. ---
; SINGLE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_one_dim_entry, i32 0, ptr @one_dim_entry_miss)
; SINGLE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_three_dim_entry, i32 0, ptr @three_dim_entry_miss)
; SINGLE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_zero_dim_entry, i32 0, ptr @zero_dim_entry_miss)

; --- 1-dim MissFn: UNCHANGED slow path -- the REAL dim count (1) still
;     selects the fixed-dim _1d variant. ---
; SINGLE-LABEL: define internal i32 @one_dim_entry_miss(
; SINGLE: load i32, ptr @__ejit_dimtype_p1
; SINGLE: call i32 @ejit_taskpool_compile_or_get_1d(

; --- 3-dim MissFn: UNCHANGED slow path -- real dims (3 > 2) keep the generic
;     compile_or_get with all three dimtype loads; the taskpool key still
;     carries the full tuple, so lifecycle retirement per instance is intact. ---
; SINGLE-LABEL: define internal i32 @three_dim_entry_miss(
; SINGLE: load i32, ptr @__ejit_dimtype_p1
; SINGLE: load i32, ptr @__ejit_dimtype_p2
; SINGLE: load i32, ptr @__ejit_dimtype_p3
; SINGLE: call i32 @ejit_taskpool_compile_or_get({{.*}}, i32 3,{{.*}})

; --- Timing combo: guarded form, still NO GEP. Table stays null-init and the
;     registration passes a null sentinel; the hit path is a plain (framed)
;     call because trace calls follow it. ---
; TIMING-DAG: @__ejit_icache_fn_three_dim_entry = internal global ptr null, align 8
; TIMING-LABEL: define i32 @three_dim_entry(
; TIMING-NOT: getelementptr
; TIMING: load atomic ptr, ptr @__ejit_icache_fn_three_dim_entry monotonic, align 8
; TIMING: call {{.*}} @llvm.expect
; TIMING: br i1 {{.*}}, label %jit_icache_dispatch, label %jit_miss
; TIMING-LABEL: jit_miss:
; TIMING: musttail call {{.*}} @three_dim_entry_miss
; TIMING-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_three_dim_entry, i32 0, ptr null)

; --- Idempotency: the second pass run neither duplicates the probe nor the
;     registration. ---
; IDEM-LABEL: define i32 @three_dim_entry(
; IDEM-COUNT-1: load atomic ptr, ptr @__ejit_icache_fn_three_dim_entry monotonic
; IDEM-COUNT-1: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_three_dim_entry, i32 0, ptr @three_dim_entry_miss)

define i32 @zero_dim_entry(i32 %x) !ejit.metadata !0 {
entry:
  ret i32 %x
}

define i32 @one_dim_entry(i32 %a) !ejit.metadata !1 {
entry:
  %v = load i32, ptr @data
  ret i32 %v
}

define i32 @three_dim_entry(i32 %a, i32 %b, i32 %c) !ejit.metadata !2 {
entry:
  %v = load i32, ptr @data
  ret i32 %v
}

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11
@data3 = global i32 0, !ejit.metadata !12

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"p1", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"p1", i32 0}, !{!"ejit_period_arr_ind", !"p2", i32 1}, !{!"ejit_period_arr_ind", !"p3", i32 2}}
!10 = distinct !{!{!"ejit_period_arr", !"p1", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"p2", i32 16}}
!12 = distinct !{!{!"ejit_period_arr", !"p3", i32 16}}
