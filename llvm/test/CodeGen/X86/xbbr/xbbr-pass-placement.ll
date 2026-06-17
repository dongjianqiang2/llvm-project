; REQUIRES: asserts
; RUN: llc -enable-xbbr -O2 -debug-pass=Structure \
; RUN:     -mtriple=x86_64-unknown-linux-gnu %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=STRUCT

; Verify XBBRMetadataEmitter is scheduled after MachineBlockPlacement.

; STRUCT: Branch Probability Basic Block Placement
; STRUCT: XBBR Metadata Emitter

define void @foo() {
  ret void
}
