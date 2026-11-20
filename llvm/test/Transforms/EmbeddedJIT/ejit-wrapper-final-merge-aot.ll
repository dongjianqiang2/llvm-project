; RUN: opt -passes='ejit-wrapper-gen,verify' -ejit-final-version-merge -S %s | FileCheck %s --check-prefix=ON
; RUN: opt -passes='ejit-wrapper-gen,ejit-wrapper-gen,verify' -ejit-final-version-merge -ejit-inline-cache -S %s | FileCheck %s --check-prefix=ON
; RUN: opt -passes='ejit-wrapper-gen,verify' -S %s | FileCheck %s --check-prefix=OFF
; OFF-NOT: __ejit_final_merge_aot_
; OFF: define i32 @merge_source
; OFF-NOT: __ejit_final_merge_aot_
; ON: @.ejit.final.aot.merge_source = private constant {{.*}}i32 3,{{.*}}ptr @__ejit_final_merge_aot_merge_source{{.*}}section ".ejit_period"
; ON-LABEL: define internal i32 @__ejit_final_merge_aot_merge_source(
; ON-NOT: ejit.metadata
; ON-NOT: ejit_taskpool_compile_or_get
; ON: store i32
; ON: ret i32

@live_store = external global i32
define i32 @merge_source(i8 %cell, i8 %trp, i32 %live) !ejit.metadata !0 {
  %c = zext i8 %cell to i32
  %t = zext i8 %trp to i32
  %s = add i32 %c, %t
  %r = add i32 %s, %live
  store i32 %r, ptr @live_store
  ret i32 %r
}
!0 = !{!1, !2, !3}
!1 = !{!"ejit_entry"}
!2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
!3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
