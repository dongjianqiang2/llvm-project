// RUN: %clang_cc1 -O2 -emit-llvm -o - %s 2>&1 | FileCheck %s

// Verify full EmbeddedJIT AOT pipeline works via clang (unified taskpool API).

// PASS1: Bitcode extraction and registration. The raw bitcode global is
// referenced by a registry table emitted into the ".ejit_bitcode" section.
// CHECK: @__ejit_bitcode = internal constant
// CHECK: @.ejit.registry.bitcode = {{.*}} section ".ejit_bitcode"
// CHECK: @llvm.global_ctors = appending global {{.*}} ptr @ejit_auto_register

// PASS3: Wrapper generation in process_cell
// CHECK: jit_entry:
// CHECK: call {{.*}} @ejit_taskpool_compile_or_get_
// CHECK: jit_fallback:
// CHECK: jit_dispatch:

// PASS4: Lifecycle handler in lc_handler - deactivate at entry, activate at exit
// CHECK: call {{.*}} @ejit_deactivate(
// CHECK: call {{.*}} @ejit_activate(

// PASS1+PASS2: Combined registration in ejit_auto_register
// CHECK-DAG: call {{.*}} @ejit_register_bitcode(
// CHECK-DAG: call {{.*}} @ejit_register_period_array({{.*}} @cell_data, i64 16)

// External runtime declarations
// CHECK-DAG: declare {{.*}} @ejit_register_bitcode
// CHECK-DAG: declare {{.*}} @ejit_taskpool_compile_or_get_
// CHECK-DAG: declare {{.*}} @ejit_deactivate
// CHECK-DAG: declare {{.*}} @ejit_activate

int cell_data[16] __attribute__((section(".mc_shared"))) __attribute__((ejit_period_arr("cell")));

__attribute__((ejit_entry))
void process_cell(int __attribute__((ejit_period_arr_ind("cell"))) cell_idx) {
    cell_data[cell_idx]++;
}

__attribute__((ejit_period_lc("cell")))
void lc_handler(int __attribute__((ejit_period_arr_ind("cell"))) cell_idx) {
    cell_data[cell_idx] = 0;
}
