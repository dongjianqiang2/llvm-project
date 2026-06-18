## XBBR (TASK M2-T05 / PLAN §9.4): --bb-cross-reorder-emit-decision-map
## emits a `.debug_xbbr_decision` section into the output ELF. Section
## type is SHT_PROGBITS, flags = 0 (NOT SHF_ALLOC). The `.debug_`
## prefix is intentional — it makes the standard `strip --strip-debug`
## recognize and remove the section, matching SPEC §9.4's expectation
## that the decision map is strippable along with debug info.
##
## In M2 the section emits only the 16-byte header with num_entries=0
## (Stage 5 in M3 will populate the per-BB entries). This test pins
## the format and the strip behavior so a future Stage 5 patch keeping
## the same magic/version is observable here.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: clang -target x86_64-unknown-linux-gnu -O2 -fbb-cross-reorder=partial \
# RUN:     -c a.c -o a.o

## (a) With --emit-decision-map: section present, header correct.
# RUN: ld.lld -e _start a.o --bb-cross-reorder=foo \
# RUN:     --bb-cross-reorder-emit-decision-map -o out.map
# RUN: llvm-readelf -SW out.map | FileCheck %s --check-prefix=PRESENT
# RUN: llvm-readobj --hex-dump=.debug_xbbr_decision out.map \
# RUN:   | FileCheck %s --check-prefix=HEADER

## (b) Without --emit-decision-map: section absent (default off).
# RUN: ld.lld -e _start a.o --bb-cross-reorder=foo -o out.nomap
# RUN: llvm-readelf -SW out.nomap \
# RUN:   | FileCheck %s --check-prefix=ABSENT --allow-empty

## (c) The section is non-loadable: strip --strip-debug removes it.
# RUN: cp out.map out.map.stripped
# RUN: llvm-strip --strip-debug out.map.stripped
# RUN: llvm-readelf -SW out.map.stripped \
# RUN:   | FileCheck %s --check-prefix=ABSENT --allow-empty

# Type SHT_PROGBITS, flags blank (not SHF_ALLOC).
# PRESENT: .debug_xbbr_decision PROGBITS

# Header layout (PLAN §9.4):
#   magic   = "XBBR" (LE bytes 58 42 42 52)
#   version = 0x00010000 (LE: 00 00 01 00)
#   num_entries = 0 (LE: 00 00 00 00)
#   flags   = 0 (LE: 00 00 00 00)
# HEADER: 0x00000000 58424252 00000100 00000000 00000000

# ABSENT-NOT: xbbr_decision

#--- a.c
int foo(int n) { return n < 0 ? -n : n + 1; }
int _start(void) { return foo(42); }
