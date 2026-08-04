// RUN: %clang_cc1 -fsyntax-only -verify=on %s
// RUN: %clang_cc1 -fsyntax-only -Wno-embedded-jit -verify=off %s
// RUN: %clang_cc1 -fsyntax-only -Wembedded-jit -verify=explicit %s

// EmbeddedJIT: verify the -Wembedded-jit toggle for the
// "may_const field modified without ejit_period_lc" warning.
// The warning is default-on (not in -Wall), silenceable via -Wno-embedded-jit.

struct S {
  __attribute__((ejit_may_const)) int a;
  int b;
};

__attribute__((ejit_period_arr("cell"))) struct S g_s[2];

// Default-on (-Wembedded-jit, whether implicit or explicit) -> warn.
// -Wno-embedded-jit -> silent.
void write_without_lc(int i) {
  g_s[i].a = 1; // on-warning {{modifying ejit_may_const field 'a' of 'g_s' without ejit_period_lc attribute}} explicit-warning {{modifying ejit_may_const field 'a' of 'g_s' without ejit_period_lc attribute}}
  g_s[i].b = 1; // no warning: 'b' is not ejit_may_const
}

// ejit_period_lc sanctions the write -> never warns, under any toggle.
__attribute__((ejit_period_lc("cell")))
void write_with_lc(__attribute__((ejit_period_arr_ind("cell"))) int i) {
  g_s[i].a = 1; // no warning
}

// off-no-diagnostics
