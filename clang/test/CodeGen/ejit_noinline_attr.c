// RUN: %clang_cc1 -O1 -mllvm -enable-ejit-aot=false -emit-llvm -o - %s | FileCheck %s
//
// CodeGen must mark ejit_entry and ejit_period_lc noinline so they survive
// the LTO inliner for PASS1 (bitcode), PASS3 (wrapper) and PASS4 (lifecycle).
// AOT passes are disabled (-enable-ejit-aot=false) so the attribute is
// attributable to CodeGen alone - PASS3 would otherwise also add noinline to
// ejit_entry. A plain function is the negative control: it must NOT carry
// noinline at -O1.

struct Cfg { int t; };
__attribute__((section(".mc_shared"))) __attribute__((ejit_period_arr("cell"))) struct Cfg g_cell[16];
extern void sink(int);

__attribute__((ejit_entry))
void entry(__attribute__((ejit_period_arr_ind("cell"))) int i) {
  sink(g_cell[i].t);
}

__attribute__((ejit_period_lc("cell")))
void lc(__attribute__((ejit_period_arr_ind("cell"))) int i) {
  g_cell[i].t = 1;
}

void plain(int i) {
  sink(i);
}

// CHECK-DAG: define {{.*}} @entry{{.*}}#[[E:[0-9]+]]
// CHECK-DAG: define {{.*}} @lc{{.*}}#[[L:[0-9]+]]
// CHECK-DAG: define {{.*}} @plain{{.*}}#[[P:[0-9]+]]
// CHECK-DAG: attributes #[[E]] = { {{.*}}noinline{{.*}} }
// CHECK-DAG: attributes #[[L]] = { {{.*}}noinline{{.*}} }
// Plain's attribute set must not contain noinline.
// CHECK: attributes #[[P]] = {
// CHECK-NOT: noinline
// CHECK: }
