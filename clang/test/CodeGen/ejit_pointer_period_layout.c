// RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64_be-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s

struct Slot {
  unsigned char live;
  __attribute__((ejit_may_const)) unsigned char enabled;
  unsigned short payload;
};
struct Cell {
  unsigned long long header;
  struct Slot slots[5];
};
__attribute__((ejit_period_arr("cell"))) struct Cell *nested;
__attribute__((ejit_period_arr("trp"))) struct Slot *flat;

// Bounds are registered separately at runtime. The pointee layout authorizes
// only marked fields and includes nested array elements and exact field widths.
// CHECK-DAG: !{!"ejit_pointer_layout", i64 32, ![[F0:[0-9]+]], ![[F1:[0-9]+]], ![[F2:[0-9]+]], ![[F3:[0-9]+]], ![[F4:[0-9]+]]}
// CHECK-DAG: ![[F0]] = !{i64 9, i64 1}
// CHECK-DAG: ![[F1]] = !{i64 13, i64 1}
// CHECK-DAG: ![[F2]] = !{i64 17, i64 1}
// CHECK-DAG: ![[F3]] = !{i64 21, i64 1}
// CHECK-DAG: ![[F4]] = !{i64 25, i64 1}
// CHECK-DAG: !{!"ejit_pointer_layout", i64 4, ![[FLAT:[0-9]+]]}
// CHECK-DAG: ![[FLAT]] = !{i64 1, i64 1}

struct Exclusions {
  __attribute__((ejit_may_const)) volatile int volatileField;
  __attribute__((ejit_may_const)) _Atomic(int) atomicField;
  __attribute__((ejit_may_const)) unsigned bits : 3;
  int live;
  __attribute__((ejit_may_const)) int fixed;
};
__attribute__((ejit_period_arr("cell"))) struct Exclusions *exclusions;
// CHECK-DAG: !{!"ejit_pointer_layout", i64 20, ![[FIXED:[0-9]+]]}
// CHECK-DAG: ![[FIXED]] = !{i64 16, i64 4}
