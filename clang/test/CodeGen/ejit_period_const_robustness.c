// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s
//
// EmbeddedJIT period_const metadata robustness (CodeGen level).
//
// Complements:
//   - ejit_metadata.c       (basic period/entry/period_const metadata)
//   - ejit_volatile_field.c (volatile + plain sibling fields are NOT annotated)
//
// New coverage here: repeated loads of the *same* period_const field are EACH
// annotated, and a period_const load reached through a nested struct-field GEP
// carries the annotation. These are the access patterns the JIT-time
// EJitStructFieldPass relies on (see the robustness tests in EJitRuntimeTest.cpp).

struct Inner {
  __attribute__((ejit_period_const)) int v; // nested period_const field
  int pad;
};

struct Cfg {
  __attribute__((ejit_period_const)) int a; // top-level period_const field
  int b;                                 // plain sibling
  struct Inner inner;
};

__attribute__((ejit_group("cell"))) struct Cfg g_cfg[8];

extern void barrier(void);

// Two reads of the SAME period_const field, separated by an opaque call so the
// front end cannot coalesce them into a single load: BOTH must be annotated.
// CHECK-LABEL: define {{.*}}@read_a_twice(
// CHECK: load i32, ptr {{.*}}, !ejit.period_const
// CHECK: load i32, ptr {{.*}}, !ejit.period_const
__attribute__((ejit_entry))
int read_a_twice(__attribute__((ejit_dim("cell"))) int ci) {
  int x = g_cfg[ci].a;
  barrier();
  int y = g_cfg[ci].a;
  return x + y;
}

// A period_const field reached through a nested struct GEP is annotated.
// CHECK-LABEL: define {{.*}}@read_nested(
// CHECK: load i32, ptr {{.*}}, !ejit.period_const
__attribute__((ejit_entry))
int read_nested(__attribute__((ejit_dim("cell"))) int ci) {
  return g_cfg[ci].inner.v;
}
