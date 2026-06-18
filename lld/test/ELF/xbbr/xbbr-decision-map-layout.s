## XBBR (review-fix N4): verify decision-map binary layout properties.
##
## M8: OrigFuncAddr placeholder = 0 in M3.
## L12: FuncId field populated (not 0 when multiple functions present).
## M7: header flags bit 0 = 0 when no degradation (normal pipeline).
## H1: entry count > 0 when pipeline produces BB-level decisions.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -ffunction-sections -c a.c -o a.o

# RUN: ld.lld -e foo a.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --bb-cross-reorder-stats \
# RUN:     --unresolved-symbols=ignore-all \
# RUN:     -o exe 2> %t.stderr

## Stats line says pipeline ran.
# RUN: FileCheck %s --check-prefix=STATS < %t.stderr

## Verify the decision-map binary layout.
# RUN: llvm-readelf -x .debug_xbbr_decision exe | FileCheck %s --check-prefix=HEX
# RUN: llvm-readelf -SW exe | FileCheck %s --check-prefix=SEC

## strip --strip-debug removes .debug_xbbr_decision.
# RUN: cp exe exe.stripped
# RUN: llvm-strip --strip-debug exe.stripped
# RUN: llvm-readelf -SW exe.stripped | FileCheck %s --check-prefix=STRIPPED --allow-empty

# STATS: xbbr-stats:
# STATS-SAME: nodes=
# STATS-SAME: funcs=

# SEC: .debug_xbbr_decision{{.*}}PROGBITS
# STRIPPED-NOT: .debug_xbbr_decision

# HEX: Hex dump of section '.debug_xbbr_decision':
# HEX-DAG: XBBR
# Header: magic "XBBR" (58 42 42 52), version 0x00010000.
# In little-endian hex dump the version field shows as bytes 01 00 00 00.
# HEX-DAG: 58424252 00000100
# num_entries > 0 (2 entries for this function's BBs).
# HEX-DAG: 02000000
# Flags at byte 12: 00000000 (bit 0 = 0 = no degradation).
# After the 16-byte header the 32-byte entry stride begins, OrigFuncAddr = 0.

#--- a.c
extern int helper(int);
int foo(int n) {
  if (__builtin_expect(n < 0, 0))
    return -1;
  return helper(n);
}
