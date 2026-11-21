// RUN: %clang_cc1 -emit-llvm -o - %s | FileCheck %s
// EmbeddedJIT CodeGen metadata tests

struct CellConfig {
  __attribute__((ejit_period_const)) int cellType;
  int xx;
};

// CHECK-DAG: @g_boardCfg = {{.*}} !ejit.metadata ![[PERIOD_META:[0-9]+]]
__attribute__((ejit_group("static"))) struct CellConfig g_boardCfg;

// CHECK-DAG: @g_cellCfg = {{.*}} !ejit.metadata ![[ARR_META:[0-9]+]]
__attribute__((ejit_group("cell"))) struct CellConfig g_cellCfg[16];

// CHECK: define {{.*}}void @jit_entry({{.*}} !ejit.metadata ![[ENTRY_META:[0-9]+]]
__attribute__((ejit_entry))
void jit_entry(__attribute__((ejit_dim("cell"))) int cellIdx) {
  // CHECK: load {{.*}} !ejit.period_const ![[PERIODCONST:[0-9]+]]
  if (g_cellCfg[cellIdx].cellType == 2) {
    // Access to period_const field should have metadata
  }
}

// CHECK: define {{.*}}void @lc_func({{.*}} !ejit.metadata ![[LC_META:[0-9]+]]
__attribute__((ejit_guard("cell")))
void lc_func(__attribute__((ejit_dim("cell"))) int cellIdx) {
  g_cellCfg[cellIdx].xx = 42; // modify non-period_const field
}

// CHECK: define {{.*}}i32 @bound_entry({{.*}} !ejit.metadata ![[BOUND_META:[0-9]+]]
__attribute__((ejit_entry))
int bound_entry(__attribute__((ejit_dim("cell"))) int cellIdx,
                __attribute__((ejit_bound_ptr("cell")))
                const struct CellConfig *cfg) {
  return cfg->cellType + cfg->xx;
}

// CHECK: define {{.*}}i32 @multi_bound_entry({{.*}} !ejit.metadata ![[MULTI_BOUND_META:[0-9]+]]
__attribute__((ejit_entry))
int multi_bound_entry(__attribute__((ejit_dim("cell"))) int cellIdx,
                      __attribute__((ejit_bound_ptr("cell")))
                      const struct CellConfig *cfgA,
                      __attribute__((ejit_bound_ptr("cell")))
                      const struct CellConfig *cfgB) {
  return cfgA->cellType + cfgB->cellType + cellIdx;
}

// CHECK-DAG: ![[PERIODCONST_FIELD:[0-9]+]] = !{!"ejit_period_const_field", i32 0}
// CHECK-DAG: ![[PERIOD_META]] = distinct !{![[PERIOD:[0-9]+]], ![[PERIODCONST_FIELD]]}
// CHECK-DAG: ![[PERIOD]] = !{!"ejit_group", !"static"}
// CHECK-DAG: ![[ARR_META]] = distinct !{![[ARR:[0-9]+]], ![[PERIODCONST_FIELD]]}
// CHECK-DAG: ![[ARR]] = !{!"ejit_group_indexed", !"cell", i32 16}
// CHECK-DAG: ![[ENTRY_META]] = distinct !{![[ENTRY:[0-9]+]], ![[IND:[0-9]+]]}
// CHECK-DAG: ![[ENTRY]] = !{!"ejit_entry"}
// CHECK-DAG: ![[LC_META]] = distinct !{![[LC:[0-9]+]], ![[IND]]}
// CHECK-DAG: ![[LC]] = !{!"ejit_guard", !"cell"}
// CHECK-DAG: ![[IND]] = !{!"ejit_dim", !"cell", i32 0}
// CHECK-DAG: ![[PERIODCONST]] = !{}
// CHECK-DAG: ![[BOUND_META]] = distinct !{![[ENTRY]], ![[IND]], ![[BOUND:[0-9]+]]}
// The node carries the pointee's size AND alignment; opaque pointers erase the
// pointee type, so the alignment cannot be recovered from the IR without it.
// CHECK-DAG: ![[BOUND]] = !{!"ejit_bound_ptr", !"cell", i32 1, i64 8, i64 4, ![[BOUND_FIELD:[0-9]+]]}
// CHECK-DAG: ![[BOUND_FIELD]] = !{i64 0, i64 4}
// The first multi-bound node is structurally identical to BOUND and is uniqued.
// CHECK-DAG: ![[MULTI_BOUND_META]] = distinct !{![[ENTRY]], ![[IND]], ![[BOUND]], ![[BOUND_B:[0-9]+]]}
// CHECK-DAG: ![[BOUND_B]] = !{!"ejit_bound_ptr", !"cell", i32 2, i64 8, i64 4, ![[BOUND_FIELD]]}
