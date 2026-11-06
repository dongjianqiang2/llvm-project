// RUN: %clang_cc1 -O2 -disable-llvm-passes -emit-llvm -o - %s | FileCheck %s --check-prefix=RAW
// RUN: rm -rf %t.dump && mkdir -p %t.dump
// RUN: %clang_cc1 -O2 -emit-llvm -mllvm -ejit-dump-bitcode-dir=%t.dump -o %t.aot.ll %s
// RUN: FileCheck %s --check-prefix=AOT < %t.aot.ll
// RUN: cat %t.dump/*.ll | FileCheck %s --check-prefix=EMBED
// RUN: %clang_cc1 -O2 -emit-llvm -mllvm -enable-ejit-bitcode=false -mllvm -enable-ejit-aot=false -o - %s | FileCheck %s --check-prefix=AOT

struct Slot {
  unsigned mode __attribute__((ejit_may_const));
  unsigned payload;
};
struct Cell { struct Slot slots[10]; };
struct Cell config[2] __attribute__((ejit_period_arr("cell")));
extern volatile unsigned runtime_slot;
extern struct RuntimeState { unsigned slot; } global_runtime;
extern unsigned next_slot(void);
extern void sink(unsigned);

static __attribute__((always_inline)) unsigned read_mode(const struct Slot *p) {
  return p->mode;
}

__attribute__((ejit_entry))
unsigned work(unsigned cell __attribute__((ejit_dim("cell"))), unsigned raw) {
  const unsigned local __attribute__((ejit_free_dim)) = raw;
  const struct Slot *p = &config[cell].slots[local];
  sink(local);
  return read_mode(p) + p->payload + config[cell].slots[raw].mode;
}

__attribute__((ejit_entry))
unsigned effects(unsigned cell __attribute__((ejit_dim("cell")))) {
  const unsigned local __attribute__((ejit_free_dim)) = next_slot();
  const unsigned vol __attribute__((ejit_free_dim)) = runtime_slot;
  return config[cell].slots[local].mode + config[cell].slots[vol].mode;
}

__attribute__((ejit_entry))
unsigned constant_local(unsigned cell __attribute__((ejit_dim("cell")))) {
  const unsigned local __attribute__((ejit_free_dim)) = 3;
  return config[cell].slots[local].mode;
}

__attribute__((ejit_entry))
unsigned global_offset(unsigned cell __attribute__((ejit_dim("cell")))) {
  const __SIZE_TYPE__ offset __attribute__((ejit_free_dim)) =
      (__SIZE_TYPE__)global_runtime.slot * sizeof(struct Slot);
  const struct Slot *p = (const struct Slot *)(
      (const char *)&config[cell].slots[0] + offset);
  return p->mode + p->payload;
}

// RAW-LABEL: define {{.*}} @work(
// RAW: call i32 @__ejit_local_free_dim_v1_i32
// RAW-LABEL: define {{.*}} @effects(
// RAW: call i32 @next_slot()
// RAW: call i32 @__ejit_local_free_dim_v1_i32
// RAW: load volatile i32
// RAW: call i32 @__ejit_local_free_dim_v1_i32
// RAW-LABEL: define {{.*}} @constant_local(
// RAW: call i32 @__ejit_local_free_dim_v1_i32(i32 3)
// RAW-LABEL: define {{.*}} @global_offset(
// RAW: load i32
// RAW: call i{{32|64}} @__ejit_local_free_dim_v1_i{{32|64}}
// AOT-NOT: @__ejit_local_free_dim_v1
// EMBED-LABEL: define {{.*}} @work(
// EMBED: [[LOCAL:%.*]] = {{(tail )?}}call i32 @__ejit_local_free_dim_v1_i32(i32 [[INPUT:%.*]])
// EMBED: zext i32 [[LOCAL]] to i64
// EMBED: zext i32 [[INPUT]] to i64
// EMBED-LABEL: define {{.*}} @effects(
// EMBED: call i32 @next_slot()
// EMBED: call i32 @__ejit_local_free_dim_v1_i32
// EMBED: load volatile i32
// EMBED: call i32 @__ejit_local_free_dim_v1_i32
