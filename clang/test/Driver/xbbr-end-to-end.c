// REQUIRES: x86-registered-target

// End-to-end: -fbb-cross-reorder=partial must produce an ELF .o that
// contains a .llvm_xbbr_attr section (XBBR attrs) plus a populated
// .llvm_bb_addr_map section with PGO analysis bytes (FuncEntryCount /
// BBFreq / BrProb). This verifies the full clang→codegen→object path.

// RUN: %clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
// RUN:        -c %s -o %t.o
// RUN: llvm-readelf -SW %t.o | FileCheck %s --check-prefix=SECTION
// RUN: llvm-readobj --bb-addr-map --pretty-pgo-analysis-map %t.o \
// RUN:   | FileCheck %s --check-prefix=PGO

// Negative: =none must NOT emit XBBR sections.
// RUN: %clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=none \
// RUN:        -c %s -o %t-off.o
// RUN: llvm-readelf -SW %t-off.o | FileCheck %s --check-prefix=OFF

// Both sections must be present (use -DAG so the section file order
// doesn't matter — XBBR attrs come after BB_ADDR_MAP in this layout).
// SECTION-DAG: .llvm_xbbr_attr{{.*}}LLVM_XBBR_ATTR
// SECTION-DAG: .llvm_bb_addr_map{{.*}}LLVM_BB_ADDR_MAP

// PGO: BBAddrMap [
// PGO: PGO analyses {

// OFF-NOT: LLVM_XBBR_ATTR
// OFF-NOT: LLVM_BB_ADDR_MAP

int hot(int n) {
  if (n < 0)
    return -1;
  return 1;
}
