# REQUIRES: aarch64

# Phase 1b EH gate: a function with an LSDA (`.gcc_except_table`, from a
# C++ try/catch) must NOT have its individual BBs migrated cross-function —
# that would break LSDA call_site ranges. It may move wholesale (contiguous),
# so it still participates in clustering, but every BB is a non-migratable
# anchor. A plain function (no LSDA, no landing pad) is NOT gated and may
# migrate. Both link and run correctly; the throwing function still throws
# and catches.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang++ -target aarch64-linux-gnu -O2 -fbb-cross-reorder=partial -c src.cpp -o src.o
# RUN: ld.lld -e main src.o --bb-cross-reorder=foo --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all -o %t/exe 2>&1 | FileCheck %s --check-prefix=STATS

# STATS: xbbr-stats:
# STATS-SAME: ehgated=1

#--- src.cpp
extern int sink(int);
extern void handle(int);
int throws(int n) {
  try {
    if (n < 0)
      throw 1;
  } catch (int e) {
    handle(e);
  }
  return sink(n);
}
int plain(int n) {
  if (n < 0)
    return -1;
  return sink(n);
}
extern "C" int main() { return plain(throws(0)); }
