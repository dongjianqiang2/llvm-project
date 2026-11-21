// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s

struct Cfg {
  __attribute__((ejit_period_const)) int frozen;
  int dynamic;
};

struct Holder {
  __attribute__((ejit_entry))
  int method(__attribute__((ejit_dim("cell"))) unsigned cell,
             __attribute__((ejit_bound_ptr("cell"))) Cfg *cfg,
             __attribute__((ejit_free_dim)) unsigned slot);
};

// A non-static method has an implicit `this` prefix. All three source
// parameter attributes must name their LLVM arguments after that prefix.
// CHECK: define {{.*}}i32 @_ZN6Holder6methodEjP3Cfgj(ptr {{[^,]*}}, i32 {{[^,]*}}, ptr {{[^,]*}}, i32 {{.*}}!ejit.metadata ![[META:[0-9]+]]
__attribute__((ejit_entry))
int Holder::method(
    __attribute__((ejit_dim("cell"))) unsigned cell,
    __attribute__((ejit_bound_ptr("cell"))) Cfg *cfg,
    __attribute__((ejit_free_dim)) unsigned slot) {
  cfg->dynamic = (int)slot;
  return cfg->frozen + (int)cell;
}

// CHECK-DAG: ![[META]] = distinct !{![[ENTRY:[0-9]+]], ![[DIM:[0-9]+]], ![[FREE:[0-9]+]], ![[BOUND:[0-9]+]]}
// CHECK-DAG: ![[ENTRY]] = !{!"ejit_entry"}
// CHECK-DAG: ![[DIM]] = !{!"ejit_dim", !"cell", i32 1}
// CHECK-DAG: ![[FREE]] = !{!"ejit_free_dim", !"", i32 3}
// CHECK-DAG: ![[BOUND]] = !{!"ejit_bound_ptr", !"cell", i32 2, i64 8, i64 4, ![[FIELD:[0-9]+]]}
// CHECK-DAG: ![[FIELD]] = !{i64 0, i64 4}
