// RUN: %clang_cc1 -emit-llvm -o - %s 2>/dev/null | FileCheck %s %if ejit-switch-case %{--check-prefix=ON%} %else %{--check-prefix=OFF%}
// EmbeddedJIT: ejit_runtime_dim metadata emission.

typedef struct {
  __attribute__((ejit_may_const)) unsigned shift;
} SlotCfg;
typedef struct {
  SlotCfg slot[3];
} CellCfg;

__attribute__((ejit_period_arr("cell"))) CellCfg g_cellCfg[8];

// The runtime dim rides in the entry's !ejit.metadata list with an empty name,
// its IR argument index, and the arm limit (0 = build default).
// ON: define {{.*}}i32 @process({{.*}} !ejit.metadata ![[META:[0-9]+]]
__attribute__((ejit_entry))
int process(__attribute__((ejit_period_arr_ind("cell"))) unsigned char cell,
            __attribute__((ejit_runtime_dim)) unsigned slotNo) {
  return g_cellCfg[cell].slot[slotNo % 3].shift + slotNo;
}

// ON: define {{.*}}i32 @process_n({{.*}} !ejit.metadata ![[META_N:[0-9]+]]
__attribute__((ejit_entry))
int process_n(__attribute__((ejit_period_arr_ind("cell"))) unsigned char cell,
              __attribute__((ejit_runtime_dim(4))) unsigned slotNo) {
  return g_cellCfg[cell].slot[slotNo % 3].shift;
}

// ON-DAG: ![[META]] = distinct !{![[ENTRY:[0-9]+]], ![[IND:[0-9]+]], ![[RT:[0-9]+]]}
// ON-DAG: ![[RT]] = !{!"ejit_runtime_dim", !"", i32 1, i32 0}
// ON-DAG: ![[META_N]] = distinct !{![[ENTRY]], ![[IND]], ![[RT_N:[0-9]+]]}
// ON-DAG: ![[RT_N]] = !{!"ejit_runtime_dim", !"", i32 1, i32 4}

// Without EJIT_SWITCH_CASE Sema dropped the attribute, so nothing is emitted.
// OFF-NOT: ejit_runtime_dim
