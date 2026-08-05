// RUN: %clang_cc1 -emit-llvm -o - %s 2>&1 | FileCheck %s
// New attribute spellings: verify metadata is emitted correctly.

struct CellConfig {
  __attribute__((ejit_period_const)) int cellType;
  int xx;
};

// CHECK-DAG: @g_boardCfg = {{.*}} !ejit.metadata
__attribute__((section(".mc_shared"))) __attribute__((ejit_in_period("static"))) struct CellConfig g_boardCfg;

// CHECK-DAG: @g_cellCfg = {{.*}} !ejit.metadata
__attribute__((section(".mc_shared"))) __attribute__((ejit_in_period_array("cell"))) struct CellConfig g_cellCfg[16];

// CHECK: define {{.*}} @jit_entry({{.*}} !ejit.metadata
__attribute__((ejit_entry))
void jit_entry(__attribute__((ejit_dim("cell"))) int cellIdx) {
  // CHECK: !ejit.may_const
  if (g_cellCfg[cellIdx].cellType == 2) { }
}

// CHECK: define {{.*}} @lc_func({{.*}} !ejit.metadata
__attribute__((ejit_period_guard("cell")))
void lc_func(__attribute__((ejit_dim("cell"))) int cellIdx) {
  g_cellCfg[cellIdx].xx = 42;
}

// Metadata tags use canonical names.
// CHECK-DAG: !{!"ejit_period_arr", !"cell", i32 16}
// CHECK-DAG: !{!"ejit_entry"}
// CHECK-DAG: !{!"ejit_period_arr_ind", !"cell", i32 0}
// CHECK-DAG: !{!"ejit_period_lc", !"cell"}
