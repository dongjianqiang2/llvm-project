//===-- ejit_local_free_dim_test.c - source-to-online-PGO integration -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// This host test uses the real frontend-generated wrapper and embedded bitcode.
// Link with the matching LLVMEJIT; it is not a multicore board acceptance test.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include <stdio.h>
#include <time.h>

struct HostSlot {
  ejit_may_const unsigned mode;
  unsigned payload;
};
struct HostCell { struct HostSlot slots[10]; };
ejit_period_arr(cell) struct HostCell local_host_cells[2];
struct { unsigned slot; } local_host_runtime;
volatile unsigned local_host_initializer_calls;

static __attribute__((always_inline)) size_t local_host_offset(void) {
  ++local_host_initializer_calls;
  return (size_t)local_host_runtime.slot * sizeof(struct HostSlot);
}

ejit_entry unsigned
local_host_work(ejit_period_arr_ind(cell) unsigned cell) {
  const size_t offset ejit_free_dim = local_host_offset();
  const struct HostSlot *p = (const struct HostSlot *)(
      (const char *)&local_host_cells[cell].slots[0] + offset);
  return p->mode * 100u + p->payload;
}

static unsigned (*volatile local_host_call)(unsigned) = local_host_work;

static int check(unsigned cell, unsigned slot, unsigned mode) {
  local_host_runtime.slot = slot;
  unsigned before = local_host_initializer_calls;
  unsigned actual = local_host_call(cell);
  unsigned expected = mode * 100u + cell * 1000u + slot;
  if (actual != expected || local_host_initializer_calls != before + 1u) {
    printf("LFD source FAIL cell=%u slot=%u got=%u expected=%u init=%u/%u\n",
           cell, slot, actual, expected, local_host_initializer_calls, before + 1u);
    return 0;
  }
  return 1;
}

static int generation(unsigned cell, unsigned mode, uint64_t wanted) {
  const clock_t start = clock();
  const struct timespec delay = {0, 1000000};
  for (unsigned poll = 0; poll != 30000; ++poll) {
    for (unsigned slot = 0; slot != 10; ++slot)
      if (!check(cell, slot, mode))
        return 0;
    ejit_taskpool_stats_t stats;
    if (ejit_taskpool_get_stats(&stats) != EJIT_OK ||
        stats.compileFailed || stats.publishFailed)
      return 0;
    if (stats.asyncCompiles >= wanted && !ejit_taskpool_pending_count())
      return stats.asyncCompiles == wanted;
    if (clock() - start > 30 * CLOCKS_PER_SEC)
      return 0;
    nanosleep(&delay, 0);
  }
  return 0;
}

int main(int argc, char **argv) {
  for (unsigned cell = 0; cell != 2; ++cell)
    for (unsigned slot = 0; slot != 10; ++slot) {
      local_host_cells[cell].slots[slot].mode = cell ? 11 : 7;
      local_host_cells[cell].slots[slot].payload = cell * 1000u + slot;
    }
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  // Native ELF links use the existing constructor registration path; the
  // companion SRE demo uses the SDK's static-registry linker bounds.
  cfg.forceStaticRegistry = false;
  cfg.dumpJITDir = argc > 1 ? argv[1] : 0;
  if (ejit_init_pgo(&cfg) != EJIT_OK)
    return 1;
  for (unsigned cell = 0; cell != 2; ++cell) {
    if (ejit_activate("cell", cell) != EJIT_OK ||
        !check(cell, 9, cell ? 11 : 7) ||
        !generation(cell, cell ? 11 : 7, 2u * (cell + 1u))) {
      ejit_shutdown();
      return 2;
    }
  }
  if (ejit_deactivate("cell", 1) != EJIT_OK)
    return 3;
  for (unsigned slot = 0; slot != 10; ++slot)
    local_host_cells[1].slots[slot].mode = 19;
  if (ejit_activate("cell", 1) != EJIT_OK ||
      !check(1, 8, 19) || !generation(1, 19, 6) || !check(0, 7, 7)) {
    ejit_shutdown();
    return 4;
  }
  ejit_taskpool_stats_t stats;
  ejit_taskpool_get_stats(&stats);
  printf("LFD source PASS compiles=%llu hits=%llu initializer_calls=%u\n",
         (unsigned long long)stats.asyncCompiles,
         (unsigned long long)stats.cacheHits, local_host_initializer_calls);
  ejit_shutdown();
  return 0;
}
