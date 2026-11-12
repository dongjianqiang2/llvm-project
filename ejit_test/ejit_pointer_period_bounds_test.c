// Verify bounded pointer periods freeze fields and keep slot stores dynamic.
#include "ejit_test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct SlotConfig {
  ejit_may_const unsigned char enabled;
  unsigned char live;
};
struct CellConfig {
  struct SlotConfig slots[10];
};
ejit_period_arr(cell) struct CellConfig *bounded_cells;

ejit_entry unsigned probe_bounded(ejit_period_arr_ind(cell) unsigned char cell,
                                  ejit_free_dim unsigned short slot) {
  struct SlotConfig *selected = &bounded_cells[cell].slots[slot % 10];
  ++selected->live;
  return selected->enabled ? 11 : 7;
}

static int wait_for_compilation(void) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  for (unsigned attempt = 0; attempt < 5000; ++attempt) {
    if (!ejit_taskpool_pending_count())
      return 1;
    usleep(1000);
  }
  return 0;
#else
  return 1;
#endif
}

int main(void) {
  bounded_cells = calloc(2, sizeof(*bounded_cells));
  if (!bounded_cells)
    return 1;
  for (unsigned slot = 0; slot < 10; ++slot)
    bounded_cells[1].slots[slot].enabled = 1;
  if (ejit_register_pointer_period_bounds("bounded_cells", bounded_cells,
                                          2 * sizeof(*bounded_cells)) !=
      EJIT_OK)
    return 1;
  ejit_config_t config;
  ejit_default_config(&config);
  if (ejit_init(&config) != EJIT_OK)
    return 1;
  // Registration must never race the compiler's lock-free registry reads.
  if (ejit_register_pointer_period_bounds("bounded_cells", bounded_cells,
                                          sizeof(*bounded_cells)) !=
      EJIT_ERR_INVALID_PARAM)
    return 1;
  if (ejit_activate("cell", 0) != EJIT_OK ||
      ejit_activate("cell", 1) != EJIT_OK)
    return 1;
  for (unsigned round = 0; round < 2; ++round) {
    for (unsigned cell = 0; cell < 2; ++cell)
      for (unsigned slot = 0; slot < 10; ++slot)
        if (probe_bounded(cell, slot) != (cell ? 11 : 7))
          return 1;
    if (!wait_for_compilation())
      return 1;
  }
  // Revisit both keys after the first async compilation/dedup window.
  for (unsigned cell = 0; cell < 2; ++cell) {
    if (probe_bounded(cell, 0) != (cell ? 11 : 7))
      return 1;
    if (!wait_for_compilation())
      return 1;
  }
  for (unsigned cell = 0; cell < 2; ++cell)
    for (unsigned slot = 0; slot < 10; ++slot)
      if (bounded_cells[cell].slots[slot].live != (slot ? 2 : 3))
        return 1;
#ifdef EJIT_SRE_TASKPOOL
  ejit_taskpool_stats_t stats;
  memset(&stats, 0, sizeof(stats));
  ejit_taskpool_get_stats(&stats);
  // Ready entries are measured directly, even with EJIT_STATS_ENABLE=OFF.
  if (stats.readyEntries < 2) {
#else
  ejit_stats_t stats;
  ejit_get_stats(&stats);
  if (!stats.entryCount) {
#endif
    printf("FAIL: no specialization compiled\n");
    return 1;
  }
  ejit_shutdown();
  free(bounded_cells);
  printf("PASS: bounded pointer-period free_dim and live slot stores\n");
  return 0;
}
