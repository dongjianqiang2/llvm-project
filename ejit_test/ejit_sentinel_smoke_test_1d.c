//===-- ejit_sentinel_smoke_test_1d.c -------------------------------------===//
//
// 1-dim entry of the sentinel-wrapper smoke: a [16] icache cell table in
// sentinel form (defined pre-filled with &MissFn, branchless probe). Owns
// its period array - see the TU-layout note in ejit_sentinel_smoke_test.c.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

#include "ejit_test_helpers.h"

struct Cfg {
  EJIT_PERIOD_CONST uint32_t kind; // 1 -> "on" branch, 0 -> "off"
};

ejit_group(p0) struct Cfg g_p0[4];

void sentinel_smoke_set_p0(uint8_t i, uint32_t kind) { g_p0[i].kind = kind; }

// 1-dim: [16] sentinel table. on -> 100+i, off -> 200+i.
ejit_entry __attribute__((noinline)) uint32_t
sentinel_1d(EJIT_DIM(p0) uint8_t i0) {
  return (g_p0[i0].kind == 1u) ? 100u + i0 : 200u + i0;
}
