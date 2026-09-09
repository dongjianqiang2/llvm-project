// RUN: %clang_cc1 -emit-llvm -o - %s 2>&1 | FileCheck %s
// EmbeddedJIT: ejit_const_dim metadata emission.

struct TrpCfg {
  __attribute__((ejit_may_const)) int cpriMode;
  int xx;
};

__attribute__((ejit_period_arr("trp"))) struct TrpCfg g_trp[16];

// CHECK: define {{.*}} @init_trp({{.*}} !ejit.metadata
__attribute__((ejit_entry))
int init_trp(__attribute__((ejit_dim("trp"))) unsigned trpIdx,
             __attribute__((ejit_const_dim)) unsigned slotNo) {
  return g_trp[trpIdx * 5 + slotNo % 5].cpriMode;
}

// Dim order in the metadata is SOURCE PARAMETER order, not "period dims first".
// The wrapper, module loader and JIT optimizer all identify a dim by its
// position in this list, so an interleaved declaration must round-trip with the
// const dim at index 0 and the period dim at index 2.
// CHECK: define {{.*}} @interleaved({{.*}} !ejit.metadata
__attribute__((ejit_entry))
int interleaved(__attribute__((ejit_const_dim)) unsigned slotNo,
                int plain,
                __attribute__((ejit_dim("trp"))) unsigned trpIdx) {
  return g_trp[trpIdx].cpriMode + plain + slotNo;
}

// A const dim on its own needs no period array at all.
// CHECK: define {{.*}} @mode_select({{.*}} !ejit.metadata
__attribute__((ejit_entry))
int mode_select(__attribute__((ejit_const_dim)) unsigned char numerology) {
  return 10 << numerology;
}

// A const dim carries the same 3-operand shape as a period dim, with an EMPTY
// period name -- a const dim names no lifecycle.
// CHECK-DAG: !{!"ejit_period_arr_ind", !"trp", i32 0}
// CHECK-DAG: !{!"ejit_const_dim", !"", i32 1}
// CHECK-DAG: !{!"ejit_const_dim", !"", i32 0}
// CHECK-DAG: !{!"ejit_period_arr_ind", !"trp", i32 2}
