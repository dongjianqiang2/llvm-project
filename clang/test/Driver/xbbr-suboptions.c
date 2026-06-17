// RUN: %clang -target x86_64-unknown-linux-gnu \
// RUN:     -fbb-cross-reorder=partial -fbb-cross-reorder-stats \
// RUN:     -fbb-cross-reorder-blacklist=/tmp/foo.txt \
// RUN:     -### -c %s 2>&1 | FileCheck %s

// M1-T06 SPEC §6.1 sub-options: blacklist + stats round-trip from the
// driver to cc1. The cold-threshold sub-option is intentionally
// deferred to M3 (the lld consumer doesn't read IsCold yet).

// CHECK: "-fbb-cross-reorder=partial"
// CHECK-SAME: "-fbb-cross-reorder-blacklist=/tmp/foo.txt"
// CHECK-SAME: "-fbb-cross-reorder-stats"
