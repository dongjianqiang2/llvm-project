//===-- ejit_verify_indirect_test.c - verifier identities ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ejit_test_helpers.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct Config {
  uint32_t pad;
  ejit_may_const uint32_t gain;
};

ejit_period_arr(cell) uint32_t g_indirectCells[2];

ejit_entry uint32_t indirect_probe(ejit_period_arr_ind(cell) uint8_t cellIndex,
                                   EJIT_BOUND_PTR(cell)
                                       const struct Config *left,
                                   EJIT_BOUND_PTR(cell)
                                       const struct Config *right) {
  return left->gain * 100u + right->gain + cellIndex;
}

static int failures;
#define CHECK(c, message)                                                      \
  do {                                                                         \
    if (!(c)) {                                                                \
      puts("FAIL: " message);                                                  \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

int main(void) {
  ejit_config_t cfg;
  ejit_default_config(&cfg);
  if (ejit_init_verify(&cfg) != EJIT_OK)
    return 1;
  ejit_verify_reset_stats();
  if (ejit_activate("cell", 0) != EJIT_OK)
    return 1;

  // Keep both borrowed objects alive for compilation and all instrumented
  // calls.
  static struct Config left = {0, 7}, right = {0, 9};
  ejit_verify_stats_t stats = {0};
  for (unsigned i = 0; i < 10000; ++i) {
    CHECK(indirect_probe(0, &left, &right) == 709, "warmup result");
    ejit_verify_get_stats(&stats);
    if (stats.checks != 0)
      break;
    usleep(1000);
  }
  CHECK(stats.checks != 0, "instrumented JIT code must execute");
  if (stats.checks == 0)
    return 1;

  right.gain = 10; // Deliberate may_const violation, only on the right object.
  CHECK(indirect_probe(0, &left, &right) == 710, "live result after mutation");

  ejit_verify_site_t sites[4];
  size_t count = ejit_verify_get_sites(sites, 4);
  CHECK(count == 2, "same field offset on different arguments has two records");
  const ejit_verify_site_t *a = NULL, *b = NULL;
  for (size_t i = 0; i < count; ++i) {
    if (strstr(sites[i].site, ":arg1"))
      a = &sites[i];
    if (strstr(sites[i].site, ":arg2"))
      b = &sites[i];
  }
  CHECK(a && b, "report identifies both argument positions");
  if (a && b) {
    CHECK(a->checks >= 2 && a->mismatches == 0, "left field stays stable");
    CHECK(b->checks >= 2 && b->mismatches == 1, "only right field mismatches");
    CHECK(b->lastFrozen == 9 && b->lastActual == 10, "right mismatch evidence");
  }
  ejit_shutdown();
  ejit_verify_print_stats();
  printf("%s: indirect verifier identities (%d failures)\n",
         failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
