; Inline-cache probe: shared-epoch freshness check.
;
; Cells are core-private, so a core that toggles a period cannot reach a peer's
; cells -- it can only bump the shared icacheEpoch. The probe therefore reads
; that epoch on EVERY call and treats a mismatch as a miss. Without this, a core
; whose cells are all warm never enters the runtime, never observes the toggle,
; and keeps running a specialization built for the previous period values.
;
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -S %s | FileCheck %s
; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes=ejit-wrapper-gen,ejit-wrapper-gen -ejit-inline-cache -S %s \
; RUN:   | FileCheck %s --check-prefix=IDEM

; The OBJECT owns the window -- a definition, not a declaration. If it were
; extern, the probe's reference and the runtime's definition would have to be
; matched by the linker, and a mismatch would leave them on different storage:
; a zeroed window reads seen == *shared == 0, i.e. "always fresh", so every core
; would keep hitting a stale cell with no diagnostic at all.
; linkonce_odr merges the per-TU copies; hidden keeps the address at adrp+add.
; CHECK: @__ejit_icache_epoch = linkonce_odr hidden global { i64, ptr } zeroinitializer

; Its address reaches the runtime through the icache registry entry's spare
; field (name2), so the runtime writes the bytes the probe reads. The i64 packs
; the probe contract version above numDims.
; CHECK: @.ejit.registry.icache {{.*}}ptr @__ejit_icache_epoch, ptr @__ejit_icache_fn_dim_entry, i64 8589934593

; CHECK-LABEL: define i32 @dim_entry(
; The cell load and its null test come first ...
; CHECK: %ejit_ic_slot = getelementptr {{.*}} @__ejit_icache_fn_dim_entry
; CHECK: %ejit_ic_fn = load ptr, ptr %ejit_ic_slot
; CHECK: %ejit_icache_hit = icmp ne ptr %ejit_ic_fn, null
; CHECK: br i1 {{.*}}, label %jit_icache_epoch, label %jit_miss

; ... then the epoch check, which is only reached with a non-null cell -- so a
; fill has happened, so the runtime has bound `shared` and no null check on it
; is required.
; seen/shared are core-private, so they stay plain loads. *shared is not: peers
; update it with an RMW, so it is atomic. monotonic lowers to the same LDR on
; AArch64; acquire would cost an LDAR per hit.
; CHECK: jit_icache_epoch:
; CHECK: %ejit_ic_seen = load i64, ptr @__ejit_icache_epoch, align 8
; CHECK: %ejit_ic_shared_p = load ptr, ptr getelementptr {{.*}} @__ejit_icache_epoch, i32 0, i32 1
; CHECK: %ejit_ic_epoch = load atomic i32, ptr %ejit_ic_shared_p monotonic, align 4
; CHECK: %ejit_icache_fresh = icmp eq
; CHECK: br i1 {{.*}}, label %jit_icache_dispatch, label %jit_miss

; Stale -> the miss path, which drains this core's cells and re-resolves.
; CHECK: jit_icache_dispatch:
; CHECK: musttail call i32 %ejit_ic_fn(
; CHECK: jit_miss:
; CHECK: musttail call i32 @dim_entry_miss(

; The cell itself is core-private, so its load stays plain.
; CHECK-NOT: %ejit_ic_fn = load atomic

; The CONSTRUCTOR path carries the same two facts per entry, in one call (the
; static registry above is only walked when forceStaticRegistry is set or the
; constructor produced nothing). Per-entry, not a global handshake: that would
; let a pre-epoch TU register against a newer TU's window in a mixed link.
; CHECK-LABEL: define internal void @ejit_auto_register()
; CHECK: call void @ejit_register_icache_slot(ptr {{.*}}, ptr @__ejit_icache_fn_dim_entry, i32 1, ptr @__ejit_icache_epoch, i32 2)

; Flag off: no probe, so nothing reads the epoch and the symbol never appears.
; OFF-NOT: __ejit_icache_epoch
; OFF-NOT: jit_icache_epoch

; Re-running the pass must not emit a second check.
; IDEM-COUNT-1: %ejit_icache_fresh


define i32 @dim_entry(i8 %idx) !ejit.metadata !0 {
entry:
  %c = zext i8 %idx to i32
  ret i32 %c
}

!0 = !{!1, !2}
!1 = !{!"ejit_entry", i32 1}
!2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
