// RUN: not %clang -target x86_64-unknown-linux-gnu -fbb-cross-reorder=bogus \
// RUN:     -c %s 2>&1 | FileCheck %s --check-prefix=BAD

// RUN: not %clang -target wasm32 -fbb-cross-reorder=partial \
// RUN:     -c %s 2>&1 | FileCheck %s --check-prefix=NOELF

// Invalid mode values are rejected; non-ELF targets are rejected for
// partial/full per SPEC §8.

// BAD:   error: invalid value 'bogus' in '-fbb-cross-reorder=bogus'
// NOELF: unsupported option '-fbb-cross-reorder=partial' for target
