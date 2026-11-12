// RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64_be-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s

struct Noise {
  unsigned metrics[16];
};
struct SparseSlot {
  unsigned char padding[8192];
  struct Noise history[128];
  __attribute__((ejit_may_const)) unsigned char enabled;
};
struct SparseCell {
  struct SparseSlot slots[10];
};
__attribute__((ejit_period_arr("cell"))) struct SparseCell *sparse;

// CHECK: @sparse = {{.*}}!ejit.metadata ![[SPARSE:[0-9]+]]
// CHECK: @excessive = {{.*}}!ejit.metadata ![[EXCESS:[0-9]+]]
// CHECK-DAG: ![[SPARSE]] = distinct !{![[PERIOD:[0-9]+]], ![[LAYOUT:[0-9]+]]}
// CHECK-DAG: ![[PERIOD]] = !{!"ejit_period_arr", !"cell", i32 0}
// CHECK-DAG: ![[LAYOUT]] = !{!"ejit_pointer_layout", i64 163880, ![[FIRST:[0-9]+]], {{.*}}![[LAST:[0-9]+]]}
// CHECK-DAG: ![[FIRST]] = !{i64 16384, i64 1}
// CHECK-DAG: ![[LAST]] = !{i64 163876, i64 1}

struct MarkedSlot {
  __attribute__((ejit_may_const)) unsigned char enabled;
};
struct Excessive {
  struct MarkedSlot slots[5000];
};
__attribute__((ejit_period_arr("cell"))) struct Excessive *excessive;

// The output budget still applies: do not emit a partial layout for this root.
// CHECK-DAG: ![[EXCESS]] = distinct !{![[PERIOD]]}
