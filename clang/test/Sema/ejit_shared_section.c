// RUN: %clang_cc1 -fsyntax-only -verify=expected %s
// EmbeddedJIT: ejit_period / ejit_period_arr must carry
// __attribute__((section(".mc_shared")))

// === Correct usage: period + section(".mc_shared") ===
__attribute__((section(".mc_shared"))) __attribute__((ejit_period("static"))) int g_board;
__attribute__((section(".mc_shared"))) __attribute__((ejit_period_arr("cell"))) int g_table[64];

struct CellConfig {
  __attribute__((ejit_may_const)) int cellType;
};

// === Error: ejit_period without section(".mc_shared") ===
__attribute__((ejit_period("cell"))) int g_missing_shared;
// expected-error@-1 {{global variable 'g_missing_shared' has ejit_period but is missing '__attribute__((section(".mc_shared")))'; ejit period variables must reside in cross-core shared memory}}

// === Error: ejit_period_arr without section(".mc_shared") ===
__attribute__((ejit_period_arr("cell"))) struct CellConfig g_arr_missing[10];
// expected-error@-1 {{global variable 'g_arr_missing' has ejit_period_arr but is missing '__attribute__((section(".mc_shared")))'; ejit period variables must reside in cross-core shared memory}}

// === Correct: section can be in any order relative to period ===
__attribute__((ejit_period("late"))) __attribute__((section(".mc_shared"))) int g_late_shared;
// The deferred check runs after all attributes are processed, so either order works.
