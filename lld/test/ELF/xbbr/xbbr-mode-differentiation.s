## XBBR : full vs partial mode produce *different* migration
## decisions. With IRPGO branch-weight metadata in the IR, threshold=0.5
## marks the cold BB; partial keeps it anchored, full lets it migrate.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

## Compile IR (with explicit !prof entry_count and branch_weights) so BFI
## actually sees real frequencies — only then does IsCold get filled by
## XBBRMetadataEmitter.
# RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu \
# RUN:     -xbbr-cold-threshold=0.5 \
# RUN:     -filetype=obj a.ll -o a.o

## Verify a.o has the cold bit set on the cold BB (sanity).
# RUN: llvm-readobj --hex-dump=.llvm_xbbr_attr a.o | FileCheck %s --check-prefix=ATTR

## Link two ways and dump.
# RUN: ld.lld -e hot a.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=partial \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --unresolved-symbols=ignore-all -o exe.partial
# RUN: ld.lld -e hot a.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --unresolved-symbols=ignore-all -o exe.full

## In partial: 1 entry-anchor + 1 cold-anchor = anchored=2, moved=1.
## In full:    1 entry-anchor                  = anchored=1, moved=2.
# RUN: llvm-bbreorder-dump --summary exe.partial \
# RUN:   | FileCheck %s --check-prefix=PART
# RUN: llvm-bbreorder-dump --summary exe.full \
# RUN:   | FileCheck %s --check-prefix=FULL

## ABI invariant proxy: 'hot' symbol exists and resolves in both modes
## (entry block was not migrated away from its anchored position).
# RUN: llvm-readelf --syms exe.partial | FileCheck %s --check-prefix=SYM
# RUN: llvm-readelf --syms exe.full    | FileCheck %s --check-prefix=SYM

## ATTR: 02030100 00008000

# PART: entries=3 moved=1 anchored=2
# FULL: entries=3 moved=2 anchored=1
# SYM:  hot

#--- a.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @helper(i32)

define i32 @hot(i32 %n) !prof !0 {
entry:
  %c = icmp slt i32 %n, 0
  br i1 %c, label %neg, label %pos, !prof !1
neg:
  ret i32 -1
pos:
  %r = call i32 @helper(i32 %n)
  ret i32 %r
}

!0 = !{!"function_entry_count", i64 1000}
!1 = !{!"branch_weights", i32 100, i32 900}
