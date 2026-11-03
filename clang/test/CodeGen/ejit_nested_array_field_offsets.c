// RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64_be-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s

#define MC __attribute__((ejit_may_const))
#define CELL __attribute__((ejit_period_arr("cell")))

struct Slot {
  unsigned char prefix[31];
  MC _Bool flag;
  unsigned char payload[96];
};
struct Cell { struct Slot slots[5]; };
CELL struct Cell nested[2];
CELL struct Slot flat[10];
CELL struct Slot grid[2][3];
__attribute__((ejit_period("static"))) struct Cell static_cell;

// CHECK-DAG: @nested = {{.*}} !ejit.metadata ![[NESTED:[0-9]+]]
// CHECK-DAG: @flat = {{.*}} !ejit.metadata ![[FLAT:[0-9]+]]
// CHECK-DAG: @grid = {{.*}} !ejit.metadata ![[GRID:[0-9]+]]
// CHECK-DAG: @static_cell = {{.*}} !ejit.metadata ![[STATIC:[0-9]+]]
// CHECK-DAG: ![[NESTED]] = distinct !{![[CELL2:[0-9]+]], ![[F0:[0-9]+]], ![[F1:[0-9]+]], ![[F2:[0-9]+]], ![[F3:[0-9]+]], ![[F4:[0-9]+]]}
// CHECK-DAG: ![[CELL2]] = !{!"ejit_period_arr", !"cell", i32 2}
// CHECK-DAG: ![[F0]] = !{!"ejit_may_const_field", i32 31}
// CHECK-DAG: ![[F1]] = !{!"ejit_may_const_field", i32 159}
// CHECK-DAG: ![[F2]] = !{!"ejit_may_const_field", i32 287}
// CHECK-DAG: ![[F3]] = !{!"ejit_may_const_field", i32 415}
// CHECK-DAG: ![[F4]] = !{!"ejit_may_const_field", i32 543}
// CHECK-DAG: ![[FLAT]] = distinct !{![[CELL10:[0-9]+]], ![[F0]]}
// CHECK-DAG: ![[CELL10]] = !{!"ejit_period_arr", !"cell", i32 10}
// CHECK-DAG: ![[GRID]] = distinct !{![[CELL2]], ![[F0]], ![[F1]], ![[F2]]}
// CHECK-DAG: ![[STATIC]] = distinct !{![[STATICPERIOD:[0-9]+]], ![[F0]], ![[F1]], ![[F2]], ![[F3]], ![[F4]]}
// CHECK-DAG: ![[STATICPERIOD]] = !{!"ejit_period", !"static"}

// A global pointer array keeps its legacy pointee-offset representation;
// nested pointee elements are not inline storage in the pointer-slot array.
struct PointerLegacy { MC unsigned fixed; struct Slot slots[2]; };
CELL struct PointerLegacy *pointer_slots[2];
// CHECK-DAG: @pointer_slots = {{.*}} !ejit.metadata ![[PTRS:[0-9]+]]
// CHECK-DAG: ![[PTRS]] = distinct !{![[CELL2]], ![[LEGACY:[0-9]+]]}
// CHECK-DAG: ![[LEGACY]] = !{!"ejit_may_const_field", i32 0}

struct Multi { unsigned char header; struct Slot slots[2][3]; };
CELL struct Multi multi[2];
// CHECK-DAG: @multi = {{.*}} !ejit.metadata ![[MULTI:[0-9]+]]
// CHECK-DAG: ![[MULTI]] = distinct !{![[CELL2]], ![[M0:[0-9]+]], ![[M1:[0-9]+]], ![[M2:[0-9]+]], ![[M3:[0-9]+]], ![[M4:[0-9]+]], ![[M5:[0-9]+]]}
// CHECK-DAG: ![[M0]] = !{!"ejit_may_const_field", i32 32}
// CHECK-DAG: ![[M1]] = !{!"ejit_may_const_field", i32 160}
// CHECK-DAG: ![[M2]] = !{!"ejit_may_const_field", i32 288}
// CHECK-DAG: ![[M3]] = !{!"ejit_may_const_field", i32 416}
// CHECK-DAG: ![[M4]] = !{!"ejit_may_const_field", i32 544}
// CHECK-DAG: ![[M5]] = !{!"ejit_may_const_field", i32 672}

union Alias { struct Slot slot; unsigned char live[128]; };
struct Exclusions {
  volatile struct Slot volatile_slots[2];
  union Alias alias;
  unsigned live;
  MC unsigned fixed;
};
CELL struct Exclusions exclusions[2];
// CHECK-DAG: @exclusions = {{.*}} !ejit.metadata ![[EXCLUDE:[0-9]+]]
// CHECK-DAG: ![[EXCLUDE]] = distinct !{![[CELL2]], ![[SAFE:[0-9]+]]}
// CHECK-DAG: ![[SAFE]] = !{!"ejit_may_const_field", i32 388}

struct AtomicBits {
  MC _Atomic(int) atomic_field;
  MC volatile int volatile_field;
  MC unsigned bits : 3;
  MC int fixed;
};
CELL struct AtomicBits atomic_bits[2];
// CHECK-DAG: @atomic_bits = {{.*}} !ejit.metadata ![[ATOMIC:[0-9]+]]
// CHECK-DAG: ![[ATOMIC]] = distinct !{![[CELL2]], ![[FIXED:[0-9]+]]}
// CHECK-DAG: ![[FIXED]] = !{!"ejit_may_const_field", i32 12}

// The bounded walker visits unmarked array element types only once.
struct Sparse { unsigned char padding[1000000]; struct Slot slots[2]; };
CELL struct Sparse sparse[2];
// CHECK-DAG: @sparse = {{.*}} !ejit.metadata ![[SPARSE:[0-9]+]]
// CHECK-DAG: ![[SPARSE]] = distinct !{![[CELL2]], ![[S0:[0-9]+]], ![[S1:[0-9]+]]}
// CHECK-DAG: ![[S0]] = !{!"ejit_may_const_field", i32 1000031}
// CHECK-DAG: ![[S1]] = !{!"ejit_may_const_field", i32 1000159}

// Failure must discard earlier fields, not emit a partial authorization list.
struct OverBudget { MC unsigned first; struct Slot slots[4096]; };
extern CELL struct OverBudget over_budget[2];
struct Overflow { MC unsigned first; unsigned char pad[4294967296ULL]; struct Slot slot; };
extern CELL struct Overflow overflow[2];
// Refer to extern globals so their declarations and metadata are emitted.
void *references[] = {over_budget, overflow};
// CHECK-DAG: @over_budget = {{.*}} !ejit.metadata ![[BUDGET:[0-9]+]]
// CHECK-DAG: @overflow = {{.*}} !ejit.metadata ![[OVERFLOW:[0-9]+]]
// CHECK-DAG: ![[BUDGET]] = distinct !{![[CELL2]]}
// CHECK-DAG: ![[OVERFLOW]] = distinct !{![[CELL2]]}
