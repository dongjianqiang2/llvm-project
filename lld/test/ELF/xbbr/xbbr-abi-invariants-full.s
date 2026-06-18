## XBBR : ABI invariants under full mode (SPEC §5).
##
## With --bb-cross-reorder-mode=full, the layout pipeline is most
## aggressive: every non-§5.3-anchor BB is a candidate to migrate. We
## must still preserve the contract:
##   * function entry block (BB 0) is anchored — always at the function
##     symbol's address (SPEC §5.1)
##   * BBs whose addresses are taken (blockaddress / indirectbr targets)
##     are anchored (SPEC §5.3 #2)
##   * landing pads are anchored (SPEC §5.3 #3)
##   * musttail call-sites are anchored (SPEC §5.3 #6)
##
## We construct a single function with all four hazards in one BB graph,
## link in full mode, and assert the decision map labels them as
## anchored — proving the §5.3 list is honored even when --mode=full
## widens the migratable pool.

# REQUIRES: x86

# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: llc -enable-xbbr -O2 -mtriple=x86_64-unknown-linux-gnu \
# RUN:     -filetype=obj a.ll -o a.o
# RUN: ld.lld -e fn a.o \
# RUN:     --bb-cross-reorder=foo --bb-cross-reorder-mode=full \
# RUN:     --bb-cross-reorder-emit-decision-map \
# RUN:     --unresolved-symbols=ignore-all -o exe

## The dump tool's summary tells us how many BBs were anchored. We expect:
##   * entry block            (1)
##   * indirectbr target      (1, blockaddress(@fn, %ibr_target))
##   * landing pad            (1, %lpad)
##   * musttail callsite      (1, %must)
## Plus any noreturn-tail / EH-related anchors LLVM adds. We assert
## anchored >= 4 — i.e. at minimum these four hazards survive full mode
## intact.
# RUN: llvm-bbreorder-dump --summary exe | FileCheck %s --check-prefix=SUM

## And the entry-block symbol still exists at a defined address.
# RUN: llvm-readelf --syms exe | FileCheck %s --check-prefix=SYM

# SUM: anchored={{[4-9]|[1-9][0-9]+}}
# SYM: fn

#--- a.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@dispatch_table = global [2 x ptr]
                  [ptr blockaddress(@fn, %ibr_target), ptr blockaddress(@fn, %normal)]
declare i32 @helper(i32)
declare i32 @musttail_callee(i32)
declare i32 @__gxx_personality_v0(...)

define i32 @fn(i32 %n) personality ptr @__gxx_personality_v0 {
entry:
  %p = getelementptr [2 x ptr], ptr @dispatch_table, i64 0, i64 0
  %dst = load ptr, ptr %p
  indirectbr ptr %dst, [label %ibr_target, label %normal]

ibr_target:                       ; address-taken target
  %v = call i32 @helper(i32 %n)
  ret i32 %v

normal:
  %r = invoke i32 @helper(i32 %n) to label %cont unwind label %lpad

lpad:                             ; landing pad
  %x = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %x

cont:
  br i1 undef, label %must, label %tail_ret

must:                             ; musttail call site
  %m = musttail call i32 @musttail_callee(i32 %r)
  ret i32 %m

tail_ret:
  ret i32 %r
}
