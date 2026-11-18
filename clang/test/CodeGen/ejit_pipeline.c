// RUN: %clang_cc1 -O2 -emit-llvm -o - %s 2>&1 | FileCheck %s

// Verify full EmbeddedJIT AOT pipeline works via clang (unified taskpool API).

// PASS1: Bitcode extraction and registration. The raw bitcode global is
// referenced by a registry table emitted into the ".ejit_bitcode" section.
// CHECK: @__ejit_bitcode = internal constant
// CHECK: @.ejit.registry.bitcode = {{.*}} section ".ejit_bitcode"
// CHECK: @llvm.global_ctors = appending global {{.*}} ptr @ejit_auto_register

// CHECK-LABEL: define {{.*}} @process_cell(
// CHECK: jit_entry:
// CHECK: musttail call void %ejit_ic_fn(

// PASS4: Lifecycle handler in lc_handler - deactivate at entry, activate at exit
// CHECK-LABEL: define {{.*}} @lc_handler(
// CHECK: call {{.*}} @ejit_deactivate(
// CHECK: call {{.*}} @ejit_activate(

// PASS1+PASS2: Combined registration in ejit_auto_register
// CHECK-DAG: call {{.*}} @ejit_register_bitcode(
// CHECK-DAG: call {{.*}} @ejit_register_period_array({{.*}} @cell_data, i64 16)

// The sentinel miss helper calls the generic ABI and releases its read token.
// CHECK-LABEL: define internal void @process_cell_miss(
// CHECK: call {{.*}} @ejit_taskpool_compile_or_get(
// CHECK: miss_dispatch:
// CHECK: call void @ejit_taskpool_release_read(

int cell_data[16] __attribute__((ejit_period_arr("cell")));

__attribute__((ejit_entry))
void process_cell(int __attribute__((ejit_period_arr_ind("cell"))) cell_idx) {
    cell_data[cell_idx]++;
}

__attribute__((ejit_period_lc("cell")))
void lc_handler(int __attribute__((ejit_period_arr_ind("cell"))) cell_idx) {
    cell_data[cell_idx] = 0;
}
