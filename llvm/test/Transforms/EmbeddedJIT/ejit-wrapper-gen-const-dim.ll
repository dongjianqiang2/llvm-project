; ejit_const_dim: a specialization dimension with no lifecycle behind it.
;
; The wrapper treats it exactly like an ejit_dim on the identity axis (it is one
; of the four dim slots, and its value travels as the instanceId that keys the
; clone), and differently everywhere lifecycle bookkeeping is involved:
;
;   * dimType is the reserved constant 7 (kEJitConstDimType), baked directly --
;     NOT a load of a per-lifecycle @__ejit_dimtype_<name> global, because there
;     is no lifecycle to register.
;   * no ejit_register_lifecycle call and no .ejit.registry.lifecycle payload is
;     emitted on its account.
;   * the inline-cache probe gets a DYNAMIC bound guard. An ejit_dim value is
;     contractually a dense period-array index in [0, D), so its axis is indexed
;     unguarded; nothing bounds a const dim's value, so without the guard an
;     out-of-range argument would index past the [D]^numDims table and
;     indirect-call whatever it read.

; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s --check-prefix=NOICACHE
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s | FileCheck %s --check-prefix=ICACHE
; --- Idempotency: the guard moves the icache GEP out of the entry block, so the
;     already-wrapped probe must still recognize the function. Running the pass
;     twice must not re-wrap it. ---
; RUN: opt -passes=ejit-wrapper-gen,ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s | FileCheck %s --check-prefix=IDEM

@g_trp = global [16 x i32] zeroinitializer, !ejit.metadata !10

; A period dim and a const dim side by side (the driving case).
define i32 @mixed_entry(i32 %trpIdx, i32 %slotNo) !ejit.metadata !0 {
entry:
  %p = getelementptr [16 x i32], ptr @g_trp, i32 0, i32 %trpIdx
  %v = load i32, ptr %p
  ret i32 %v
}

; A const dim on its own: no period array anywhere in the module.
define i32 @const_only_entry(i32 %numerology) !ejit.metadata !1 {
entry:
  %v = shl i32 10, %numerology
  ret i32 %v
}

; Two const dims in ONE function. They share the reserved dimType, which the
; validation must NOT reject as a duplicated dimension (the cache tells them
; apart positionally).
define i32 @two_const_entry(i32 %a, i32 %b) !ejit.metadata !2 {
entry:
  %v = add i32 %a, %b
  ret i32 %v
}

; --- Only the "trp" lifecycle gets a dimType global; the const dims get none. -
; NOICACHE-DAG: @__ejit_dimtype_trp = internal global i32 -1
; NOICACHE-NOT: @__ejit_dimtype_ =
; NOICACHE-NOT: @__ejit_dimtype_numerology

; --- mixed_entry: dim0 loads the lifecycle slot, dim1 passes the constant 7. --
; NOICACHE-LABEL: define i32 @mixed_entry(
; NOICACHE: %ejit_dimtype = load i32, ptr @__ejit_dimtype_trp
; NOICACHE: call i32 @ejit_taskpool_compile_or_get_2d(i32 %ejit_funcidx, i32 %ejit_dimtype, i32 %trpIdx, i32 7, i32 %slotNo,

; --- const_only_entry: 1 dim, dimType 7, no dimtype load at all. ---
; NOICACHE-LABEL: define i32 @const_only_entry(
; NOICACHE-NOT: @__ejit_dimtype_
; NOICACHE: call i32 @ejit_taskpool_compile_or_get_1d(i32 %ejit_funcidx, i32 7, i32 %numerology,

; --- two_const_entry: both dims carry dimType 7; no error, no dimtype load. ---
; NOICACHE-LABEL: define i32 @two_const_entry(
; NOICACHE-NOT: @__ejit_dimtype_
; NOICACHE: call i32 @ejit_taskpool_compile_or_get_2d(i32 %ejit_funcidx, i32 7, i32 %a, i32 7, i32 %b,

; --- Exactly one lifecycle is registered ("trp"); the const dims add none. ---
; NOICACHE: define internal void @ejit_auto_register()
; NOICACHE: call void @ejit_register_lifecycle
; NOICACHE-NOT: call void @ejit_register_lifecycle

; --- icache ON: the const-dim axis is guarded, the period-dim axis is not. ---
; A const dim also opts the function out of the branchless sentinel form its
; dim count would otherwise select: the range guard needs a miss block to branch
; to, so the table stays zero-init and registration passes the null drain value.
; ICACHE-DAG: @__ejit_icache_fn_mixed_entry = internal global [16 x [16 x ptr]] zeroinitializer, align 8
; ICACHE-DAG: @__ejit_icache_fn_const_only_entry = internal global [16 x ptr] zeroinitializer, align 8
; ICACHE-DAG: ptr null, ptr @__ejit_icache_fn_mixed_entry, i64 2 }
; ICACHE-DAG: ptr null, ptr @__ejit_icache_fn_const_only_entry, i64 1 }

; mixed_entry: ONE guard (for %slotNo only -- %trpIdx is unguarded), then the
; two-dim GEP in the guarded successor.
; ICACHE-LABEL: define i32 @mixed_entry(
; ICACHE: %ejit_ic_inrange = icmp ult i32 %slotNo, 16
; ICACHE: br i1 {{.*}}, label %jit_icache_probe, label %jit_miss
; ICACHE-LABEL: jit_icache_probe:
; ICACHE: %ejit_ic_slot = getelementptr inbounds [16 x [16 x ptr]], ptr @__ejit_icache_fn_mixed_entry, i32 0, i32 %trpIdx, i32 %slotNo
; ICACHE: load atomic ptr, ptr %ejit_ic_slot monotonic, align 8

; two_const_entry: TWO guards, and-ed together.
; ICACHE-LABEL: define i32 @two_const_entry(
; ICACHE: %ejit_ic_inrange = icmp ult i32 %a, 16
; ICACHE: %ejit_ic_inrange{{.*}} = icmp ult i32 %b, 16
; ICACHE: and i1 %ejit_ic_inrange, %ejit_ic_inrange
; ICACHE: br i1 {{.*}}, label %jit_icache_probe, label %jit_miss

; --- Idempotency: one probe, one guard, one wrap. ---
; IDEM-LABEL: define i32 @mixed_entry(
; IDEM: %ejit_ic_inrange = icmp ult i32 %slotNo, 16
; IDEM-NOT: icmp ult
; IDEM-LABEL: jit_icache_probe:
; IDEM: %ejit_ic_slot = getelementptr inbounds [16 x [16 x ptr]], ptr @__ejit_icache_fn_mixed_entry, i32 0, i32 %trpIdx, i32 %slotNo
; IDEM-NOT: @__ejit_icache_fn_mixed_entry, i32 0

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"trp", i32 0}, !{!"ejit_const_dim", !"", i32 1}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_const_dim", !"", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_const_dim", !"", i32 0}, !{!"ejit_const_dim", !"", i32 1}}
!10 = distinct !{!{!"ejit_period_arr", !"trp", i32 16}}
