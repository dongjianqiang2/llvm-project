// Probe-cost isolation: time only the tiny entry, hit vs plain, under perf stat.
// usage: ./probe hit1 | hit16 | plain | miss
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ejit_test_helpers.h"

struct Tiny { EJIT_PERIOD_CONST int a; int b; };
#define N 16
EJIT_GROUP(cell) struct Tiny g_tiny[N];
EJIT_ENTRY int tiny(EJIT_DIM(cell) uint8_t ci) { return g_tiny[ci].a + g_tiny[ci].b; }
__attribute__((noinline)) int tiny_nojit(uint8_t ci) { return g_tiny[ci].a + g_tiny[ci].b; }
#define CALLS 200000000ULL
volatile int sink;

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "hit1";
  for (int k = 0; k < N; k++) g_tiny[k] = (struct Tiny){1000 + k, 1};
  ejit_config_t cfg;
  ejit_default_config(&cfg);
  if (ejit_init(&cfg) != EJIT_OK) return 1;
  int plain = !strcmp(mode, "plain");
  int rot = !strcmp(mode, "hit16");
  if (!plain && strcmp(mode, "miss")) {
    if (rot) { if (ejit_activate_all("cell") != EJIT_OK) return 1; }
    else if (ejit_activate("cell", 3) != EJIT_OK) return 1;
    for (int round = 0; round < 3; round++) {
      for (int k = 0; k < N; k++) { if (rot || k == 3) tiny((uint8_t)k); }
      ejit_drain_taskpool();
    }
  }
  struct timespec a, b;
  int s = 0;
  clock_gettime(CLOCK_MONOTONIC, &a);
  if (plain) { for (uint64_t k = 0; k < CALLS; k++) s += tiny_nojit(rot ? (uint8_t)(k & 15) : 3); }
  else if (!strcmp(mode, "miss")) { for (uint64_t k = 0; k < CALLS; k++) s += tiny((uint8_t)(k & 7)); }
  else { for (uint64_t k = 0; k < CALLS; k++) s += tiny(rot ? (uint8_t)(k & 15) : 3); }
  clock_gettime(CLOCK_MONOTONIC, &b);
  sink = s;
  double ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / CALLS;
  ejit_taskpool_stats_t ts; memset(&ts, 0, sizeof ts); ejit_taskpool_get_stats(&ts);
  printf("%-7s %.3f ns/call  (compiles=%llu)\n", mode, ns, (unsigned long long)ts.asyncCompiles);
  ejit_shutdown();
  return 0;
}
