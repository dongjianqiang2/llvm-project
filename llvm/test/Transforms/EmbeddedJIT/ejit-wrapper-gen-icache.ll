; -ejit-inline-cache (ON): emits an inline probe DIRECTLY in the ejit_entry
; wrapper - a GEP into the per-function @__ejit_icache_fn_<name> [D]^numDims
; cell table (D = EJIT_ICACHE_DIM_SIZE, power-of-2) by the ejit_dim argument
; values, then ONE monotonic load + musttail indirect call. For NumDims <= 2
; (no timing) the table is DEFINED pre-filled with &<name>_miss: the loaded
; value is always a callable pointer (the specialization on a hit, MissFn
; itself before the first fill / after a drain), so the wrapper has NO guard
; branch at all - a miss simply executes MissFn, whose funcidx guard falls to
; the AOT body while registration has not run. The load is atomic because the
; table is shared across cores (a peer's period toggle rewrites cells in
; place); monotonic is the weakest order that makes that a defined race, and
; lowers to the same LDR a plain load would. There is NO freshness check on
; the hit path - draining the shared cells IS the invalidation. numDims=0 is
; a scalar cell (no GEP). Default (OFF): the original 4-block wrapper, no
; icache. Idempotent: running the pass twice does not duplicate the probe.

; RUN: opt -passes=ejit-wrapper-gen -ejit-icache-section= -S %s | FileCheck %s --check-prefix=ICACHE
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache=false -S %s | FileCheck %s --check-prefix=NOICACHE
; RUN: opt -passes=ejit-wrapper-gen,ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s | FileCheck %s --check-prefix=IDEM
; --- -ejit-inline-cache + -ejit-dispatcher-cluster + -ejit-missfn-cold ---
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-dispatcher-cluster -ejit-missfn-cold -S %s | FileCheck %s --check-prefix=OPT
; --- -ejit-icache-section: the cell table goes into the inter-core SHARED
;     section, which is what lets a deactivate on one core zero the cells every
;     other core probes. Nothing else about the wrapper changes. ---
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section=.mc_shared -S %s | FileCheck %s --check-prefix=SECTION

; --- icache globals: [D]^numDims, D=16 (EJIT_ICACHE_DIM_SIZE). 0D is scalar.
;     Every cell is DEFINED as &<name>_miss - the sentinel that makes the
;     branchless probe safe. ---
; ICACHE-DAG: @__ejit_icache_fn_zero_dim_entry = internal global ptr @zero_dim_entry_miss, align 8
; ICACHE-DAG: @__ejit_icache_fn_one_dim_entry = internal global [16 x ptr] [ptr @one_dim_entry_miss, {{.*}}], align 8
; ICACHE-DAG: @__ejit_icache_fn_two_dim_entry = internal global [16 x [16 x ptr]] {{.*}}@two_dim_entry_miss{{.*}}, align 8

; --- 0D entry: scalar slot, direct plain load (NO GEP), NO guard branch -
;     the whole wrapper is load + musttail. Without extra flags the dispatcher
;     stays in default .text (no section attribute). ---
; ICACHE-LABEL: define i32 @zero_dim_entry(
; ICACHE-NOT: section
; ICACHE-NOT: ejit_icache_try
; ICACHE-NOT: getelementptr
; ICACHE-NOT: br
; ICACHE: load atomic ptr, ptr @__ejit_icache_fn_zero_dim_entry monotonic, align 8
; ICACHE-NOT: br
; ICACHE: musttail call {{.*}} %ejit_ic_fn
; ICACHE: ret

; --- 1D entry: [16 x ptr] slot, GEP by the single dim arg + plain load,
;     still branchless. ---
; ICACHE-LABEL: define i32 @one_dim_entry(
; ICACHE-NOT: section
; ICACHE-NOT: ejit_icache_try
; ICACHE-NOT: br
; ICACHE: icmp ult i32 %idx1, 16
; ICACHE: getelementptr {{.*}}ptr @__ejit_icache_fn_one_dim_entry, i32 0, i32 {{.*}}
; ICACHE: select i1 {{.*}}, ptr %ejit_ic_slot, ptr @one_dim_entry.icache_miss
; ICACHE: load atomic ptr, ptr {{.*}} monotonic, align 8
; ICACHE-NOT: call void @ejit_taskpool_release_read
; ICACHE-NOT: br
; ICACHE: musttail call {{.*}} %ejit_ic_fn
; ICACHE: ret

; --- 2D entry: [16 x [16 x ptr]] slot, 2-subscript GEP, branchless. ---
; ICACHE-LABEL: define i32 @two_dim_entry(
; ICACHE-NOT: section
; ICACHE: getelementptr {{.*}}ptr @__ejit_icache_fn_two_dim_entry, i32 0, i32 {{.*}}, i32 {{.*}}
; ICACHE: load atomic ptr, ptr {{.*}} monotonic, align 8
; ICACHE: musttail call {{.*}} %ejit_ic_fn

; --- OPT (dispatcher-cluster + missfn-cold ON): section and cold present ---
; OPT-LABEL: define i32 @zero_dim_entry(
; OPT-SAME: section ".text.ejit_dispatch"
; OPT-LABEL: define i32 @one_dim_entry(
; OPT-SAME: section ".text.ejit_dispatch"
; OPT-LABEL: define i32 @two_dim_entry(
; OPT-SAME: section ".text.ejit_dispatch"
; OPT-LABEL: define internal i32 @zero_dim_entry_miss(
; OPT-SAME: #[[MISS_ATTRS:[0-9]+]]
; OPT-LABEL: define internal i32 @one_dim_entry_miss(
; OPT-SAME: #[[MISS_ATTRS]]
; OPT-LABEL: define internal i32 @two_dim_entry_miss(
; OPT-SAME: #[[MISS_ATTRS]]
; OPT-DAG: attributes #[[MISS_ATTRS]] = { cold noinline }

; --- registration carries numDims (3rd arg) and the sentinel MissFn (4th arg,
;     which the runtime writes back on drain / fill-retract). DAG: order-
;     independent. ---
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_zero_dim_entry, i32 0, ptr @zero_dim_entry_miss)
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_one_dim_entry, i32 1, ptr @one_dim_entry_miss)
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_two_dim_entry, i32 2, ptr @two_dim_entry_miss)

; --- -ejit-icache-section (default .mc_shared, pinned here so the test does not
;     depend on the CMake value): every cell table lands in the inter-core
;     shared section - now carrying its &MissFn sentinel initializer, which is
;     what the image loader must place before any core runs - and the probe is
;     unchanged (one monotonic load + musttail, no freshness compare). The
;     other RUN lines pin it EMPTY to check the plain-.bss shape. ---
; SECTION-DAG: @__ejit_icache_fn_zero_dim_entry = internal global ptr @zero_dim_entry_miss, section ".mc_shared", align 8
; SECTION-DAG: @__ejit_icache_fn_one_dim_entry = internal global [16 x ptr] [ptr @one_dim_entry_miss, {{.*}}], section ".mc_shared", align 8
; SECTION-DAG: @__ejit_icache_fn_two_dim_entry = internal global [16 x [16 x ptr]] {{.*}}@two_dim_entry_miss{{.*}}, section ".mc_shared", align 8
; SECTION-LABEL: define i32 @one_dim_entry(
; SECTION: load atomic ptr, ptr {{.*}} monotonic, align 8
; SECTION: musttail call {{.*}} %ejit_ic_fn

; --- Explicit opt-out: no icache anywhere; original compile_or_get path. ---
; NOICACHE-LABEL: define i32 @one_dim_entry(
; NOICACHE-NOT: __ejit_icache_fn
; NOICACHE-NOT: ejit_register_icache_slot
; NOICACHE: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 1, ptr {{.*}}, ptr {{.*}})

; --- Idempotent: two passes emit each probe exactly once, and the sentinel
;     registration (with its MissFn argument) is not duplicated either. ---
; IDEM-LABEL: define i32 @one_dim_entry(
; IDEM-COUNT-1: getelementptr {{.*}} @__ejit_icache_fn_one_dim_entry, i32 0, i32 {{.*}}
; IDEM-COUNT-1: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_one_dim_entry, i32 1, ptr @one_dim_entry_miss)

define i32 @zero_dim_entry(i32 %x) !ejit.metadata !0 {
entry:
  %v1 = load i32, ptr @data
  ret i32 0
}

define i32 @one_dim_entry(i32 %idx1) !ejit.metadata !1 {
entry:
  %v1 = load i32, ptr @data
  ret i32 0
}

define i32 @two_dim_entry(i32 %a, i32 %b) !ejit.metadata !2 {
entry:
  %v1 = load i32, ptr @data
  %v2 = load i32, ptr @data2
  ret i32 0
}

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
; `trp` declares MORE entries than EJIT_ICACHE_DIM_SIZE (16) on purpose: the
; declared element count does not gate probe emission, so two_dim_entry still
; gets its [16 x [16 x ptr]] table and its probe. The ICACHE checks above pin
; that. Ids at or above the per-dim bound have no cell, and the runtime declines
; to fill one for them (icacheDimsInRange in icacheFill / icacheTry).
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 32}}
