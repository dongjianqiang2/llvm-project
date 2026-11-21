// RUN: %clang_cc1 -emit-llvm -o - %s 2>&1 | FileCheck %s
// Attribute spellings and their IR metadata: every annotation lowers to the
// canonical metadata tags. The legacy spellings (ejit_period, ejit_period_arr,
// ejit_period_arr_ind, ejit_period_lc, ejit_may_const) have been removed and
// are now unknown attributes (see ext_attr_ejit.cpp for the negative test).

struct CellConfig {
  __attribute__((ejit_period_const)) int cellType;
  int xx;
};

// ejit_group: form inferred from the variable type.
// CHECK-DAG: @g_boardCfg = {{.*}} !ejit.metadata
__attribute__((ejit_group("static"))) struct CellConfig g_boardCfg;
// CHECK-DAG: @g_groupCfg = {{.*}} !ejit.metadata
__attribute__((ejit_group("grp"))) struct CellConfig g_groupCfg[8];
// CHECK-DAG: @g_ptrCfg = {{.*}} !ejit.metadata
__attribute__((ejit_group("grp"))) struct CellConfig *g_ptrCfg;

// CHECK: define {{.*}} @jit_entry({{.*}} !ejit.metadata
__attribute__((ejit_entry))
void jit_entry(__attribute__((ejit_dim("grp"))) int cellIdx) {
  // CHECK: !ejit.period_const
  if (g_groupCfg[cellIdx].cellType == 2) { }
}

// CHECK: define {{.*}} @lc_func({{.*}} !ejit.metadata
__attribute__((ejit_guard("grp")))
void lc_func(__attribute__((ejit_dim("grp"))) int cellIdx) {
  g_groupCfg[cellIdx].xx = 42;
}

// Metadata tags: scalar form -> "ejit_group", indexed form (array with size,
// pointer with size 0) -> "ejit_group_indexed".
// CHECK-DAG: !{!"ejit_group", !"static"}
// CHECK-DAG: !{!"ejit_group_indexed", !"grp", i32 8}
// CHECK-DAG: !{!"ejit_group_indexed", !"grp", i32 0}
// CHECK-DAG: !{!"ejit_entry"}
// CHECK-DAG: !{!"ejit_dim", !"grp", i32 0}
// CHECK-DAG: !{!"ejit_guard", !"grp"}
