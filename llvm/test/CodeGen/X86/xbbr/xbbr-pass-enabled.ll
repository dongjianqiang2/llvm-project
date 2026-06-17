; REQUIRES: asserts
; RUN: llc -enable-xbbr -O2 -debug-only=xbbr-metadata-emitter \
; RUN:     -mtriple=x86_64-unknown-linux-gnu %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ON
; RUN: llc -O2 -debug-only=xbbr-metadata-emitter \
; RUN:     -mtriple=x86_64-unknown-linux-gnu %s -o /dev/null 2>&1 \
; RUN:   | not FileCheck %s --check-prefix=ON

; Verify the XBBRMetadataEmitter pass runs only when -enable-xbbr is given.

; ON: XBBRMetadataEmitter running on foo

define void @foo() {
  ret void
}
