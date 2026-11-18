//===-- ejit_dump_test.c - JIT IR dump verification -----------------------===//
// Verify fresh pre/post IR for each entry, with or without audit sampling.
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ejit_test_helpers.h"

//===--- Data types
//--------------------------------------------------------===//

typedef enum { MODE_A = 0, MODE_B = 1, MODE_C = 2 } Mode_t;

typedef struct {
  Mode_t ejit_may_const mode;
  int ejit_may_const threshold;
  int gain;
} CellCfg;

//===--- Period variables
//--------------------------------------------------===//

ejit_period_arr(cell) CellCfg g_cells[16];

//===--- JIT entry functions
//-----------------------------------------------===//

ejit_entry int check_cell(ejit_period_arr_ind(cell) uint8_t idx) {
  if (g_cells[idx].mode == MODE_A)
    return g_cells[idx].threshold * 10 + g_cells[idx].gain;
  else if (g_cells[idx].mode == MODE_B)
    return g_cells[idx].threshold * 20 + g_cells[idx].gain;
  else
    return g_cells[idx].threshold * 5 + g_cells[idx].gain;
}

ejit_entry int simple_return(ejit_period_arr_ind(cell) uint8_t idx) {
  return g_cells[idx].threshold + g_cells[idx].gain;
}

// Match the publication's numeric key, not just the number of .ll files.
static int find_pair(const char *dir, const char *name, char *pre, char *opt) {
  DIR *dp = opendir(dir);
  if (!dp)
    return 0;
  size_t n = strlen(name);
  int matches = 0;
  struct dirent *de;
  while ((de = readdir(dp))) {
    const char *file = de->d_name;
    if (strncmp(file, name, n) || file[n] != '_')
      continue;
    const char *key = file + n + 1;
    size_t digits = strspn(key, "0123456789");
    if (!digits || strcmp(key + digits, "_pre.ll"))
      continue;
    if (snprintf(pre, PATH_MAX, "%s/%s", dir, file) >= PATH_MAX ||
        snprintf(opt, PATH_MAX, "%s/%.*s_opt.ll", dir, (int)(n + 1 + digits),
                 file) >= PATH_MAX) {
      closedir(dp);
      return 0;
    }
    ++matches;
  }
  closedir(dp);
  return matches == 1;
}

static int contains(const char *path, const char *prefix, const char *needle) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  char *line = NULL;
  size_t cap = 0;
  int found = 0;
  while (getline(&line, &cap, f) >= 0)
    if (!strncmp(line, prefix, strlen(prefix)) && strstr(line, needle)) {
      found = 1;
      break;
    }
  free(line);
  fclose(f);
  return found;
}

static int count_dumps(const char *dir) {
  DIR *dp = opendir(dir);
  if (!dp)
    return -1;
  int count = 0;
  struct dirent *de;
  while ((de = readdir(dp))) {
    size_t len = strlen(de->d_name);
    if (len >= 3 && !strcmp(de->d_name + len - 3, ".ll"))
      ++count;
  }
  closedir(dp);
  return count;
}

static int verify_entry(const char *dir, const char *name, uint32_t index,
                        uint32_t dimType, uint8_t cell, int expected,
                        int (*wrapper)(uint8_t), int expectedFiles) {
  if (ejit_activate("cell", cell) != EJIT_OK)
    return 0;
  ejit_dim_pair_t dim = {dimType, cell};
  char pre[PATH_MAX], opt[PATH_MAX], symbol[64], counter[64];
  snprintf(symbol, sizeof(symbol), "@%s(", name);
  snprintf(counter, sizeof(counter), "@__profc_%s =", name);
  void *instrumented = NULL;
  unsigned hits = 0;
  for (unsigned poll = 0; poll < 30000; ++poll) {
    void *fn = NULL;
    uint32_t bucket = 0;
    ejit_status_t st =
        ejit_taskpool_compile_or_get(index, &dim, 1, &fn, &bucket);
    if (st == EJIT_OK && fn) {
      int result = ((int (*)(uint8_t))fn)(cell);
      ejit_taskpool_release_read(bucket);
      if (result != expected) {
        fprintf(stderr, "%s(%u): got %d, expected %d\n", name, cell, result,
                expected);
        return 0;
      }

      // A pointer returned by the cache has been published. On the first
      // publication the on-disk IR tells us whether an audit window is open.
      // Keep hitting Tier-1 until the cache serves a different, final pointer.
      if (!instrumented || fn != instrumented) {
        if (!find_pair(dir, name, pre, opt) ||
            !contains(pre, "define ", symbol) ||
            !contains(opt, "define ", symbol) ||
            !contains(pre, "", "load i32") || !contains(opt, "", "ret i32") ||
            !contains(pre, "", "ejit_period_arr")) {
          fprintf(stderr, "%s: missing or invalid pre/opt pair in %s\n", name,
                  dir);
          return 0;
        }
        if (!contains(opt, counter, "global")) {
          if (!strcmp(name, "check_cell") &&
              !contains(pre, "", "ejit.may_const"))
            return 0;
          struct stat before, after;
          if (count_dumps(dir) != expectedFiles || stat(opt, &before) ||
              wrapper(cell) != expected ||
              !ejit_drain_taskpool_bounded(EJIT_DRAIN_SPIN_BUDGET) ||
              count_dumps(dir) != expectedFiles || stat(opt, &after) ||
              before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
              before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
            fprintf(stderr, "%s: cache hit failed or rewrote final IR\n", name);
            return 0;
          }
          printf("final %s: %s + %s (instrumented calls=%u)\n", name, pre, opt,
                 hits);
          return 1;
        }
        if (instrumented)
          return 0;
        instrumented = fn;
      }
      ++hits;
    } else if (st != EJIT_PENDING) {
      return 0;
    }
    usleep(1000);
  }
  fprintf(stderr, "timed out waiting for final %s (instrumented calls=%u)\n",
          name, hits);
  return 0;
}

int main(int argc, char **argv) {
  int idx = argc > 1 ? atoi(argv[1]) : 0;
  if (idx < 0 || idx >= 16)
    return 1;
  const char *root = getenv("EJIT_DUMP_DIR");
  if (root && mkdir(root, 0700) && errno != EEXIST) {
    perror("creating dump parent");
    return 1;
  }
  if (!root)
    root = getenv("TMPDIR");
  char dir[PATH_MAX];
  if (snprintf(dir, sizeof(dir), "%s/ejit_dump_XXXXXX", root ? root : "/tmp") >=
          (int)sizeof(dir) ||
      !mkdtemp(dir)) {
    perror("creating fresh dump directory");
    return 1;
  }
  printf("dumpDir=%s\n", dir);

  uint32_t cell, first, second;
  ejit_register_lifecycle("cell", &cell);
  ejit_register_funcindex("check_cell", &first);
  ejit_register_funcindex("simple_return", &second);
  ejit_config_t cfg;
  ejit_default_config(&cfg);
  cfg.dumpJITDir = dir;
  if (ejit_init(&cfg) != EJIT_OK)
    return 1;

  for (int i = 0; i < 16; ++i) {
    g_cells[i].mode = i % 3 == 1 ? MODE_B : MODE_A;
    g_cells[i].threshold = 10 + i;
    g_cells[i].gain = i;
  }
  int idx2 = (idx + 3) % 16;
  int ok = verify_entry(dir, "check_cell", first, cell, idx,
                        (10 + idx) * (idx % 3 == 1 ? 20 : 10) + idx, check_cell,
                        2) &&
           verify_entry(dir, "simple_return", second, cell, idx2, 10 + 2 * idx2,
                        simple_return, 4);
  ejit_shutdown();
  if (ok && !getenv("EJIT_DUMP_DIR")) {
    const char *names[] = {"check_cell", "simple_return"};
    char pre[PATH_MAX], opt[PATH_MAX];
    for (unsigned i = 0; i < 2; ++i)
      if (!find_pair(dir, names[i], pre, opt) || unlink(pre) || unlink(opt))
        return 1;
    if (rmdir(dir))
      return 1;
  }
  puts(ok ? "=== PASS ===" : "=== FAIL ===");
  return !ok;
}
