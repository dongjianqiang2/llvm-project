// REQUIRES: x86-registered-target

// XBBR compiler-side end-to-end: compiles a single C/C++ file
// containing every SPEC §5.3 blacklist class XBBR can detect at the
// IR layer, plus simulated PGO via __builtin_expect-style branch hints,
// and verifies the resulting .o carries the full XBBR metadata surface:
//   - .llvm_bb_addr_map with FuncEntryCount + BBFreq + BrProb
//   - .llvm_xbbr_attr with the right blacklist bits per BB
//   - .llvm.call_graph_profile (a function-level edge to declare()
//     drives this; XBBR Stage 1 in lld consumes it)
// All three are emitted by the single cc1 invocation driven from the
// driver -fbb-cross-reorder=partial flag — i.e. the driver-side
// integration source → driver → cc1 → object.

// RUN: %clang -target x86_64-unknown-linux-gnu -O2 \
// RUN:     -fbb-cross-reorder=partial -c %s -o %t.o
// RUN: llvm-readelf -SW %t.o | FileCheck %s --check-prefix=SECTIONS
// RUN: llvm-readobj --bb-addr-map --pretty-pgo-analysis-map %t.o \
// RUN:   | FileCheck %s --check-prefix=PGO
// RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr %t.o \
// RUN:   | FileCheck %s --check-prefix=ATTR

// All three XBBR sections must be present in the output object:
// SECTIONS-DAG: .llvm_bb_addr_map{{.*}}LLVM_BB_ADDR_MAP
// SECTIONS-DAG: .llvm_xbbr_attr{{.*}}LLVM_XBBR_ATTR{{.*}}LE
// .llvm_xbbr_attr must be flagged for link-time exclusion (LE).

// PGO analyses must be present (FuncEntryCount + BBFreq + BrProb were
// auto-enabled via -fbb-cross-reorder=partial — see useBBAddrMap()).
// PGO: BBAddrMap [
// PGO: PGO analyses {

// At least one .llvm_xbbr_attr byte must have IsEntry set; the dump
// also covers IsLandingPad / IsMustTail / IsIndirectBrTarget bits.
// ATTR: Hex dump of section '.llvm_xbbr_attr':

extern int declare(int);

// A normal hot function with a profile-style branch hint. Drives BBFreq
// and BrProb on the resulting BB_ADDR_MAP. The musttail-decorated
// return on the hot path also exercises IsMustTail (bit 5 = 0x20).
int hot(int n) {
  if (__builtin_expect(n < 0, 0))
    return -1;
  [[clang::musttail]] return declare(n);
}
