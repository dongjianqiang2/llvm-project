//===-- ejit_bound_ptr_test.c - Dimension-bound raw pointer test -----------===//

#include <stdint.h>
#include <stdio.h>

#include "ejit_test_helpers.h"

typedef struct {
  uint32_t ejit_may_const algorithm;
  uint32_t ejit_may_const scale;
  uint32_t runtimeBias;
} CellRelated;

ejit_period_arr(cell) uint32_t g_cells[8];
ejit_period_arr(trp) uint32_t g_trps[8];

ejit_entry uint32_t bound_cell_config(ejit_period_arr_ind(cell)
                                          uint8_t cellIndex,
                                      ejit_period_arr_ind(trp) uint8_t trpIndex,
                                      EJIT_BOUND_PTR(cell)
                                          const CellRelated *cellRelated,
                                      uint32_t input) {
  if (cellRelated->algorithm == 7)
    return input * cellRelated->scale + cellRelated->runtimeBias + cellIndex +
           trpIndex;
  return input + cellRelated->runtimeBias;
}

static uint32_t call_with_instance(uint8_t cellIndex, uint8_t trpIndex,
                                   const CellRelated *config) {
  return bound_cell_config(cellIndex, trpIndex, config, 10);
}

int main(void) {
  const uint8_t Cell = 3;
  const uint8_t Trp = 2;
  ejit_config_t RuntimeConfig;
  ejit_default_config(&RuntimeConfig);
  if (ejit_init(&RuntimeConfig) != EJIT_OK)
    return 1;
  if (ejit_activate("cell", Cell) != EJIT_OK ||
      ejit_activate("trp", Trp) != EJIT_OK)
    return 2;

  // Keep the shared object alive until the asynchronous worker has finished.
  // A stack-local object that dies after the entry call is not supported.
  static const CellRelated CellConfig = {7, 5, 100};
  ejit_dump_func("bound_cell_config");
  uint32_t Aot = call_with_instance(Cell, Trp, &CellConfig);
  ejit_drain_taskpool();

  uint32_t Jit = call_with_instance(Cell, Trp, &CellConfig);
  uint32_t ExpectedAot = 10 * 5 + 100 + Cell + Trp;
  uint32_t ExpectedJit = ExpectedAot;
  printf("AOT=%u expected=%u JIT=%u expected=%u\n", Aot, ExpectedAot, Jit,
         ExpectedJit);
  ejit_print_dumped("bound_cell_config");
  if (Aot != ExpectedAot || Jit != ExpectedJit) {
    printf("FAIL\n");
    return 3;
  }
  printf("PASS\n");
  return 0;
}
