// RUN: not %clang -target x86_64-unknown-linux-gnu \
// RUN:     -fbb-cross-reorder=partial -flto=thin -c %s 2>&1 \
// RUN:   | FileCheck %s

// SPEC §8.3: XBBR does not support ThinLTO. The driver must reject
// `-fbb-cross-reorder=partial|full` combined with `-flto=thin` rather
// than silently producing inconsistent per-partition metadata.

// CHECK: unsupported option '-fbb-cross-reorder=partial' for target 'ThinLTO
