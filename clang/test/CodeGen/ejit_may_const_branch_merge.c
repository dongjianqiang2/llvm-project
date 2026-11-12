// RUN: %clang_cc1 -triple aarch64-none-elf -O2 -disable-llvm-passes -emit-llvm -o - %s | FileCheck %s --check-prefix=RAW
// RUN: rm -rf %t.dump && mkdir -p %t.dump
// RUN: %clang_cc1 -triple aarch64-none-elf -O2 -emit-llvm -mllvm -ejit-dump-bitcode-dir=%t.dump -o %t.aot.ll %s
// RUN: FileCheck %s --check-prefix=AOT < %t.aot.ll
// RUN: cat %t.dump/*.ll | FileCheck %s --check-prefix=EMBED

struct __attribute__((aligned(64))) Algo {
  unsigned long long bits __attribute__((ejit_may_const));
  char padding[56];
};
extern struct Algo *table __attribute__((ejit_period_arr("trp")));
struct Context { char padding[20]; unsigned short slot; unsigned char choice; };
struct Flags { char padding[8]; _Bool flag; };
extern void before_a(void);
extern void before_b(void);
extern int sink_a(int, unsigned short, const struct Algo *);
extern int sink_b(int, unsigned short, const struct Algo *);
extern int sink_c(int, unsigned short, const struct Algo *);

__attribute__((ejit_entry))
int branch_local(unsigned char trp __attribute__((ejit_dim("trp"))),
                 const struct Context *ctx, const struct Flags *flags) {
  const unsigned short slot __attribute__((ejit_free_dim)) = ctx->slot;
  if (flags->flag) {
    before_b();
    const struct Algo *p = &table[slot % 5];
    return sink_b((p->bits & 8) != 0, slot, p);
  }
  before_a();
  if (ctx->choice) {
    const struct Algo *p = &table[slot % 5];
    return sink_a((p->bits & 8) != 0, slot, p);
  }
  const struct Algo *p = &table[slot % 5];
  return sink_c((p->bits & 8) != 0, slot, p);
}

// RAW-LABEL: define {{.*}} @branch_local(
// RAW-COUNT-3: load i64, {{.*}}align 64, {{.*}}!ejit.may_const
// RAW-NOT: load i64
// AOT-NOT: @__ejit_local_free_dim
// EMBED-LABEL: define {{.*}} @branch_local(
// EMBED: call i16 @__ejit_local_free_dim_v1_i16
// EMBED-COUNT-2: load i64, {{.*}}align 64, {{.*}}!ejit.may_const
// EMBED-NOT: load i64
