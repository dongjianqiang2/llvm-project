; RUN: opt -ejit-inline-cache=false -passes=ejit-wrapper-gen -S %s | FileCheck %s
;
; ejit_free_dim is metadata for PASS6 only. The wrapper must key one logical
; specialization by the retained cell dimension, while forwarding the live slot
; and other call arguments on every AOT and JIT path.
;
; CHECK-LABEL: define i32 @free_dim_entry(i8 %cell, i32 %slot, i32 %live)
; CHECK: zext i8 %cell to i32
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 1, ptr {{.*}}, ptr {{.*}})
; CHECK: jit_fallback:
; CHECK: urem i32 %slot, 5
; CHECK: add i32 {{.*}}, %live
; CHECK: jit_dispatch:
; CHECK: call i32 %{{.*}}(i8 %cell, i32 %slot, i32 %live)
;
define i32 @free_dim_entry(i8 %cell, i32 %slot, i32 %live) !ejit.metadata !0 {
entry:
  %slotmod = urem i32 %slot, 5
  %sum = add i32 %slotmod, %live
  ret i32 %sum
}

!0 = distinct !{!1, !2, !3}
!1 = !{!"ejit_entry"}
!2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
!3 = !{!"ejit_free_dim", !"", i32 1}
