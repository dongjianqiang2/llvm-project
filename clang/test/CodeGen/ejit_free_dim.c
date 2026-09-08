// RUN: %clang_cc1 -emit-llvm -o - %s | FileCheck %s
// EmbeddedJIT: ejit_free_dim metadata emission.

struct UnitParams {
  __attribute__((ejit_may_const)) int laneCount;
  int dynamic;
};

__attribute__((ejit_period_arr("unit"))) struct UnitParams g_unitParams[80];

// The motivating shape. The free dim rides in the same !ejit.metadata node list
// as the period dim, with an empty period name and its parameter index.
// CHECK: define {{.*}}i32 @init_unit({{.*}} !ejit.metadata ![[ENTRY_META:[0-9]+]]
__attribute__((ejit_entry))
int init_unit(__attribute__((ejit_period_arr_ind("unit"))) unsigned unitIdx,
              __attribute__((ejit_free_dim)) unsigned slotNo) {
  struct UnitParams *p = &g_unitParams[unitIdx * 5 + slotNo % 5];
  p->dynamic = 1;
  return p->laneCount;
}

// Several free dims on one function, recorded by parameter index.
// CHECK: define {{.*}}i32 @two_free({{.*}} !ejit.metadata ![[TWO_META:[0-9]+]]
__attribute__((ejit_entry))
int two_free(__attribute__((ejit_period_arr_ind("unit"))) unsigned unitIdx,
             __attribute__((ejit_free_dim)) unsigned slotNo,
             __attribute__((ejit_free_dim)) unsigned subSlot) {
  return g_unitParams[unitIdx * 5 + (slotNo + subSlot) % 5].laneCount;
}

// CHECK-DAG: ![[ENTRY_META]] = distinct !{![[ENTRY:[0-9]+]], ![[IND:[0-9]+]], ![[FREE1:[0-9]+]]}
// CHECK-DAG: ![[ENTRY]] = !{!"ejit_entry"}
// CHECK-DAG: ![[IND]] = !{!"ejit_period_arr_ind", !"unit", i32 0}
// CHECK-DAG: ![[FREE1]] = !{!"ejit_free_dim", !"", i32 1}
// CHECK-DAG: ![[TWO_META]] = distinct !{![[ENTRY]], ![[IND]], ![[FREE1]], ![[FREE2:[0-9]+]]}
// CHECK-DAG: ![[FREE2]] = !{!"ejit_free_dim", !"", i32 2}

// The recorded index must be an LLVM IR argument number, not a source
// parameter number. On x86-64 a two-eightbyte struct expands to two IR
// arguments, so `slot` at source index 2 is IR argument 3. Recording 2 would
// name the second half of the struct and let the JIT freeze a may_const access
// made through it -- a substitution the user never authorised.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=ABI %s
struct Pair { long x, y; };

// ABI: define {{.*}}i32 @expanded_arg(i32 {{[^,]*}}, i64 {{[^,]*}}, i64 {{[^,]*}}, i32 {{[^,]*}}, i32 {{.*}}!ejit.metadata ![[ABI_META:[0-9]+]]
__attribute__((ejit_entry))
int expanded_arg(__attribute__((ejit_period_arr_ind("unit"))) unsigned unitIdx,
                 struct Pair pair,
                 __attribute__((ejit_free_dim)) unsigned slotNo,
                 int dynamic) {
  return (int)pair.x + (int)g_unitParams[unitIdx * 5 + slotNo % 5].laneCount
         + dynamic;
}
// ABI-DAG: ![[ABI_META]] = distinct !{![[ABI_ENTRY:[0-9]+]], ![[ABI_IND:[0-9]+]], ![[ABI_FREE:[0-9]+]]}
// ABI-DAG: ![[ABI_ENTRY]] = !{!"ejit_entry"}
// ABI-DAG: ![[ABI_IND]] = !{!"ejit_period_arr_ind", !"unit", i32 0}
// ABI-DAG: ![[ABI_FREE]] = !{!"ejit_free_dim", !"", i32 3}

// pass_object_size inserts a hidden size argument AFTER its pointer parameter,
// so it shifts LATER parameters rather than forming a prefix. Deriving the
// offset from a total would put both annotations one argument too late --
// keying the wrapper on `slot` and freezing accesses made through `live`,
// which carries no annotation at all.
// ABI-DAG: define {{.*}}i32 @interleaved_objsize(i32 {{[^,]*}}, i32 {{[^,]*}}, i32 {{[^,]*}}, ptr {{[^,]*}}, i64 {{.*}}!ejit.metadata ![[POS_META:[0-9]+]]
__attribute__((ejit_entry))
int interleaved_objsize(
    __attribute__((ejit_period_arr_ind("unit"))) unsigned unitIdx,
    __attribute__((ejit_free_dim)) unsigned slotNo,
    int live,
    const char *const p __attribute__((pass_object_size(0)))) {
  return (int)g_unitParams[unitIdx * 5 + slotNo % 5].laneCount + live
         + (int)__builtin_object_size(p, 0);
}
// The period-dim node is identical to expanded_arg's and MDNodes are uniqued,
// so reuse that capture rather than matching the same line twice.
// ABI-DAG: ![[POS_META]] = distinct !{![[ABI_ENTRY]], ![[ABI_IND]], ![[POS_FREE:[0-9]+]]}
// ABI-DAG: ![[POS_FREE]] = !{!"ejit_free_dim", !"", i32 1}

// A parameter the target passes indirectly has no single IR argument holding
// its value, so there is no index to record. The annotation is dropped with a
// warning rather than attached to the pointer: silently emitting nothing would
// look like the attribute simply had no effect, and emitting the pointer's
// index would specialize on an address.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -verify \
// RUN:   -DEJIT_WIDE_PARAM -o - %s | FileCheck --check-prefix=WIDE %s
#ifdef EJIT_WIDE_PARAM
// WIDE: define {{.*}}i32 @byval_param(i32 {{[^,]*}}, ptr {{.*}}byval{{.*}}!ejit.metadata ![[W_META:[0-9]+]]
__attribute__((ejit_entry))
int byval_param(__attribute__((ejit_period_arr_ind("unit"))) unsigned unitIdx,
                __attribute__((ejit_free_dim)) _BitInt(256) slotNo) {
  // expected-warning@-1 {{ejit_free_dim on parameter 'slotNo' is ignored: the parameter is not passed as a single value, so the JIT has no argument to attribute it to}}
  return (int)g_unitParams[unitIdx].laneCount + (int)slotNo;
}
// The period dim is unaffected and keeps its own index; only the free dim goes.
// WIDE-DAG: ![[W_META]] = distinct !{![[W_ENTRY:[0-9]+]], ![[W_IND:[0-9]+]]}
// WIDE-DAG: ![[W_ENTRY]] = !{!"ejit_entry"}
// WIDE-DAG: ![[W_IND]] = !{!"ejit_period_arr_ind", !"unit", i32 0}
// WIDE-NOT: ejit_free_dim
#endif
