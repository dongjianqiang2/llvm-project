// RUN: %clang -target x86_64-unknown-linux-gnu -fbb-cross-reorder=partial \
// RUN:        -### -c %s 2>&1 | FileCheck %s --check-prefix=PARTIAL
// RUN: %clang -target x86_64-unknown-linux-gnu -fbb-cross-reorder=full \
// RUN:        -### -c %s 2>&1 | FileCheck %s --check-prefix=FULL
// RUN: %clang -target x86_64-unknown-linux-gnu -fbb-cross-reorder=function \
// RUN:        -### -c %s 2>&1 | FileCheck %s --check-prefix=FUNCTION
// RUN: %clang -target x86_64-unknown-linux-gnu -fbb-cross-reorder=none \
// RUN:        -### -c %s 2>&1 | FileCheck %s --check-prefix=NONE
// RUN: %clang -target x86_64-unknown-linux-gnu -### -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=DEFAULT

// : each mode parses to a cc1 -fbb-cross-reorder= argument, and
// the absence of the option leaves no cc1 flag (default off).

// PARTIAL: "-fbb-cross-reorder=partial"
// FULL:    "-fbb-cross-reorder=full"
// FUNCTION:"-fbb-cross-reorder=function"
// NONE:    "-fbb-cross-reorder=none"
// DEFAULT-NOT: -fbb-cross-reorder=
