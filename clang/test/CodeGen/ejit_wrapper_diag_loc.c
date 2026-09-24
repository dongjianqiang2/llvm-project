// REQUIRES: x86-registered-target
//
// The wrapper-gen backend error for invalid !ejit.metadata (here: a
// duplicated lifecycle dimension in ejit_period_arr_ind) must be anchored at
// the offending function instead of surfacing as a bare message: with debug
// info the diagnostic carries the subprogram's file:line, and without it
// clang falls back to the function's definition location
// (ManglingFullSourceLocs). The definition is written on one line so both
// anchors (subprogram line and decl name location) coincide.

// RUN: not %clang_cc1 -emit-obj -triple x86_64-unknown-linux-gnu -O2 -debug-info-kind=limited %s -o %t.o 2>&1 | FileCheck %s
// RUN: not %clang_cc1 -emit-obj -triple x86_64-unknown-linux-gnu -O2 %s -o %t.o 2>&1 | FileCheck %s

int cellCfg[16] __attribute__((ejit_period_arr("cell")));

__attribute__((ejit_entry))
int dup_dim_entry(int a __attribute__((ejit_period_arr_ind("cell"))), int b __attribute__((ejit_period_arr_ind("cell")))) { // CHECK: [[@LINE]]:{{.*}} error: {{.*}}ejit-wrapper-gen: duplicated lifecycle dimension 'cell' in ejit_period_arr_ind metadata (arguments 0 and 1)
  return a + b;
}
