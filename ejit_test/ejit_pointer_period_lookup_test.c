// Discover pointer-period bounds through one common allocator lookup.
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
ejit_period_arr(cell) struct CellConfig *automatic_cells;

// A synthetic allocator record. A board adapter would query its common
// allocator's records here, without knowing any period-global names.
static const void *allocation_base;
static uint64_t allocation_bytes;
static unsigned allocation_queries;

static uint64_t allocation_size(const void *base, void *context) {
  if (context != &allocation_queries)
    return 0;
  __atomic_fetch_add(&allocation_queries, 1, __ATOMIC_RELAXED);
  return base == allocation_base ? allocation_bytes : 0;
}

ejit_entry unsigned probe_automatic(ejit_period_arr_ind(cell) unsigned char cell,
                                    ejit_free_dim unsigned short slot) {
  struct SlotConfig *selected = &automatic_cells[cell].slots[slot % 10];
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
  automatic_cells = calloc(2, sizeof(*automatic_cells));
  if (!automatic_cells)
    return 1;
  allocation_base = automatic_cells;
  allocation_bytes = 2 * sizeof(*automatic_cells);
  for (unsigned slot = 0; slot < 10; ++slot)
    automatic_cells[1].slots[slot].enabled = 1;
  if (ejit_set_allocation_size_lookup(allocation_size, &allocation_queries) !=
      EJIT_OK)
    return 1;
  ejit_config_t config;
  ejit_default_config(&config);
  if (ejit_init(&config) != EJIT_OK)
    return 1;
  if (ejit_set_allocation_size_lookup(allocation_size, &allocation_queries) !=
      EJIT_ERR_INVALID_PARAM)
    return 1;
  if (ejit_activate("cell", 0) != EJIT_OK ||
      ejit_activate("cell", 1) != EJIT_OK)
    return 1;
  for (unsigned round = 0; round < 2; ++round) {
    for (unsigned cell = 0; cell < 2; ++cell)
      for (unsigned slot = 0; slot < 10; ++slot)
        if (probe_automatic(cell, slot) != (cell ? 11 : 7))
          return 1;
    if (!wait_for_compilation())
      return 1;
  }
  for (unsigned cell = 0; cell < 2; ++cell) {
    if (probe_automatic(cell, 0) != (cell ? 11 : 7) || !wait_for_compilation())
      return 1;
  }
  for (unsigned cell = 0; cell < 2; ++cell)
    for (unsigned slot = 0; slot < 10; ++slot)
      if (automatic_cells[cell].slots[slot].live != (slot ? 2 : 3))
        return 1;
  if (!__atomic_load_n(&allocation_queries, __ATOMIC_RELAXED)) {
    printf("FAIL: allocator lookup was never queried\n");
    return 1;
  }
#ifdef EJIT_SRE_TASKPOOL
  ejit_taskpool_stats_t stats;
  memset(&stats, 0, sizeof(stats));
  ejit_taskpool_get_stats(&stats);
  if (stats.readyEntries < 2)
    return 1;
#endif
  ejit_shutdown();
  free(automatic_cells);
  printf("PASS: automatic pointer-period bounds and live slot stores\n");
  return 0;
}
