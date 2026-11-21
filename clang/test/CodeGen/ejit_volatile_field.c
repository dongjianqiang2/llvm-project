// RUN: %clang_cc1 -emit-llvm -o - %s | FileCheck %s
// Test: volatile fields must NOT get !ejit.period_const metadata on loads.

struct Config {
  __attribute__((ejit_period_const)) int cellType;
  __attribute__((ejit_period_const)) volatile int powerLevel; // volatile - no period_const
  int normalField;
};

__attribute__((ejit_group("cell"))) struct Config g_cfg[10];

__attribute__((ejit_entry))
int test_volatile(__attribute__((ejit_dim("cell"))) int ci) {
  // CHECK: load i32, {{.*}} !ejit.period_const ![[PERIODCONST:[0-9]+]]
  int x = g_cfg[ci].cellType; // period_const applies

  // Volatile field: must NOT get !ejit.period_const
  // CHECK-NOT: load volatile i32, {{.*}} !ejit.period_const
  int y = g_cfg[ci].powerLevel; // volatile, no period_const

  return x + y;
}

// Non-period_const field also gets no metadata
__attribute__((ejit_entry))
int test_normal_field(__attribute__((ejit_dim("cell"))) int ci) {
  // Normal field: no period_const metadata
  // CHECK-NOT: load i32, {{.*}} !ejit.period_const
  int z = g_cfg[ci].normalField;
  return z;
}
