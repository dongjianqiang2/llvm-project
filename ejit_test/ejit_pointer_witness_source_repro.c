/* Compiler reproduction input, NOT an SRE shell/board demo.
 * Use an EJIT Clang with pointer-layout metadata (PR249):
 * clang -O2 -ffreestanding -I llvm/include -DREPRO_SLOTS=5 -S -emit-llvm \
 *   ejit_test/ejit_pointer_witness_source_repro.c -o pointer-slots.ll
 * Repeat with REPRO_SLOTS=10. Inspect the embedded bitcode through the JIT
 * pipeline, not only the AOT wrapper. Runtime inputs and acceptance are in
 * jit_design_doc/EJIT_POINTER_WITNESS_FALLBACK.md, "C-source reproduction".
 */
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

#ifndef REPRO_SLOTS
#define REPRO_SLOTS 5
#endif

struct Slot {
  uint8_t prefix[31];
  ejit_may_const _Bool flag;
  uint8_t payload[96];
};
struct Cell {
  struct Slot g_2[REPRO_SLOTS];
};
_Static_assert(sizeof(struct Slot) == 128, "expected slot stride");

extern ejit_period_arr(cell) struct Cell *g_1;
extern uint8_t g_gate;
extern void repro_sink(unsigned flag, uint16_t slot, const struct Slot *selected);

#define ARGS ejit_period_arr_ind(cell) uint8_t cell, uint8_t other, \
             uint16_t frame, ejit_free_dim uint16_t slot, uint32_t mode, void *ctx

ejit_entry void repro_direct(ARGS) {
  if (g_gate > 1)
    repro_sink(g_1[cell].g_2[slot % REPRO_SLOTS].flag, slot,
               &g_1[cell].g_2[slot % REPRO_SLOTS]);
}

ejit_entry void repro_address(ARGS) {
  const struct Slot *selected = &(g_1[cell].g_2[slot % REPRO_SLOTS]);
  if (g_gate > 1)
    repro_sink(selected->flag, slot, selected);
}

static __attribute__((always_inline)) inline _Bool read_flag(const struct Slot *p) {
  return p->flag;
}

ejit_entry void repro_inline_helper(ARGS) {
  const struct Slot *selected = &(g_1[cell].g_2[slot % REPRO_SLOTS]);
  if (g_gate > 1)
    repro_sink(read_flag(selected), slot, selected);
}
