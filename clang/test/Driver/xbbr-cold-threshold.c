// RUN: %clang -target x86_64-unknown-linux-gnu -fbb-cross-reorder=partial \
// RUN:        -fbb-cross-reorder-cold-threshold=0.05 \
// RUN:        -### -c %s 2>&1 | FileCheck %s --check-prefix=OK
//
// RUN: not %clang -target x86_64-unknown-linux-gnu -fbb-cross-reorder=partial \
// RUN:        -fbb-cross-reorder-cold-threshold=1.5 \
// RUN:        -### -c %s 2>&1 | FileCheck %s --check-prefix=BAD
//
// RUN: not %clang -target x86_64-unknown-linux-gnu -fbb-cross-reorder=partial \
// RUN:        -fbb-cross-reorder-cold-threshold=foo \
// RUN:        -### -c %s 2>&1 | FileCheck %s --check-prefix=NAN

// clang driver validates -fbb-cross-reorder-cold-threshold=
// (must be in [0.0, 1.0)) and forwards to cc1.

// OK: "-fbb-cross-reorder-cold-threshold=0.05"
// BAD: error: invalid value '1.5'
// NAN: error: invalid value 'foo'
