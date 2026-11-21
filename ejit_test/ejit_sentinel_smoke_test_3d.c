//===-- ejit_sentinel_smoke_test_3d.c -------------------------------------===//
//
// 3-dim entry of the sentinel-wrapper smoke: a [16]^3 icache cell table in
// GUARDED form (NumDims > 2 keeps the zero-init table and the null guard;
// drain writes 0). Owns its period arrays - see the TU-layout note in
// ejit_sentinel_smoke_test.c.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

#include "ejit_test_helpers.h"

struct Cfg3 {
  EJIT_PERIOD_CONST uint32_t kind;
};

ejit_group(p1) struct Cfg3 g_q1[4];
ejit_group(p2) struct Cfg3 g_q2[4];
ejit_group(p3) struct Cfg3 g_q3[4];

void sentinel_smoke_set_3d(uint8_t i, uint32_t kind) {
  g_q1[i].kind = kind;
  g_q2[i].kind = kind;
  g_q3[i].kind = kind;
}

// 3-dim: all-on -> 7, all-off -> 0.
ejit_entry __attribute__((noinline)) uint32_t
guarded_3d(EJIT_DIM(p1) uint8_t i1,
           EJIT_DIM(p2) uint8_t i2,
           EJIT_DIM(p3) uint8_t i3) {
  uint32_t r = (g_q1[i1].kind == 1u) ? 1u : 0u;
  r |= (g_q2[i2].kind == 1u) ? 2u : 0u;
  r |= (g_q3[i3].kind == 1u) ? 4u : 0u;
  return r;
}
