// REQUIRES: aarch64-registered-target

// Phase 0a: -fbb-cross-reorder=partial|full must imply
// -fbasic-block-sections=all (SPEC §6.3 maps full→all; partial reuses the
// same substrate). Each BB becomes its own InputSection with real cross-BB
// branch relocations — the only substrate lld can physically reorder without
// instruction re-encoding. =function must NOT imply BB sections.
//
// On AArch64, -fbasic-block-sections=all is driver-rejected for users
// (branch relaxation creates untracked MBBs); XBBR sidesteps that by setting
// Options.BBSections in BackendUtil (bypassing the driver check) and disabling
// BranchRelaxation. So user-passed =all must still error, while
// -fbb-cross-reorder=partial silently enables it.

// --- partial implies =all: per-BB sections + per-BB symbols + multi-range ---
// RUN: %clang -target aarch64-linux-gnu -O2 \
// RUN:     -fbb-cross-reorder=partial -c %s -o %t.o
// RUN: llvm-readelf -SW %t.o | FileCheck %s --check-prefix=PARTIAL-SEC
// RUN: llvm-readelf -sW %t.o | FileCheck %s --check-prefix=PARTIAL-SYM
// RUN: llvm-readobj --bb-addr-map %t.o | FileCheck %s --check-prefix=PARTIAL-RANGES

// The entry BB plus two successor BBs each become their own section under =all.
// Entry section is .text.<fn>; parts are .text.<fn>.<fn>.__part.N (the doubled
// name comes from -funique-basic-block-section-names, also implied).
// PARTIAL-SEC-DAG: .text.worker{{ *}}PROGBITS
// PARTIAL-SEC-DAG: .text.worker.worker.__part.1{{ *}}PROGBITS
// PARTIAL-SEC-DAG: .text.worker.worker.__part.2{{ *}}PROGBITS

// Per-BB local symbols exist (used as cross-BB branch relocation targets).
// readelf -s puts the name last: "... FUNC GLOBAL DEFAULT <ndx> worker".
// PARTIAL-SYM-DAG: worker.__part.1
// PARTIAL-SYM-DAG: worker.__part.2
// PARTIAL-SYM-DAG: FUNC{{.*}}GLOBAL{{.*}}worker

// BBAddrMap carries one BB range per section (multi-range = per-BB sections).
// readobj prints one outer "BB Ranges [" then a "{ Base Address: 0x0 ... }"
// block per range; >=2 Base Address lines prove multi-range.
// PARTIAL-RANGES: BB Ranges [
// PARTIAL-RANGES: Base Address: 0x0
// PARTIAL-RANGES: Base Address: 0x0

// --- function mode: NO per-BB sections (CGProfile-only, SPEC §6.3) ---
// function mode does not force -ffunction-sections, so the whole function
// lands in plain .text as one section.
// RUN: %clang -target aarch64-linux-gnu -O2 \
// RUN:     -fbb-cross-reorder=function -c %s -o %t_fn.o
// RUN: llvm-readelf -SW %t_fn.o | FileCheck %s --check-prefix=FUNC-SEC
// RUN: llvm-readelf -SW %t_fn.o | not grep "__part"
// FUNC-SEC: .text{{ *}}PROGBITS

// --- user-passed -fbasic-block-sections=all on AArch64 still rejected ---
// RUN: not %clang -target aarch64-linux-gnu -O2 \
// RUN:     -fbasic-block-sections=all -c %s -o %t_user.o 2>&1 \
// RUN:   | FileCheck %s --check-prefix=REJECT
// REJECT: invalid value 'all' in '-fbasic-block-sections=all'

extern int sink_a(int), sink_b(int);

// Two successor BBs so -O2 lowering yields >= 2 non-entry BBs to split.
int worker(int n) {
  if (n < 0)
    return sink_a(n);
  return sink_b(n);
}
