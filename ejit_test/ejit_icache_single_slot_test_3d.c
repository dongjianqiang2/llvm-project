//===-- ejit_icache_single_slot_test_3d.c ----------------------------------===//
//
// 3-dim entry of the single-slot smoke: under
// -mllvm -ejit-icache-single-slot its @__ejit_icache_fn_single_slot_3d table
// is ONE scalar cell (registered numDims=0) even though all three params are
// marked -- MissFn still resolves through the taskpool with the real
// (3-dim) tuple. Owns its period arrays - see the TU-layout note in
// ejit_icache_single_slot_test.c.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

#include "ejit_test_helpers.h"

struct CfgS {
  ejit_may_const uint32_t kind;
};

ejit_period_arr(p1) struct CfgS g_s1[4];
ejit_period_arr(p2) struct CfgS g_s2[4];
ejit_period_arr(p3) struct CfgS g_s3[4];

void single_slot_set_3d(uint8_t i, uint32_t kind) {
  g_s1[i].kind = kind;
  g_s2[i].kind = kind;
  g_s3[i].kind = kind;
}

// 3-dim, deployed all-equal: all-on -> 7, all-off -> 0.
ejit_entry __attribute__((noinline)) uint32_t
single_slot_3d(ejit_period_arr_ind(p1) uint8_t i1,
               ejit_period_arr_ind(p2) uint8_t i2,
               ejit_period_arr_ind(p3) uint8_t i3) {
  uint32_t r = (g_s1[i1].kind == 1u) ? 1u : 0u;
  r |= (g_s2[i2].kind == 1u) ? 2u : 0u;
  r |= (g_s3[i3].kind == 1u) ? 4u : 0u;
  return r;
}
