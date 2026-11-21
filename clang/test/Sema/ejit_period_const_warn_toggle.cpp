// RUN: %clang_cc1 -fsyntax-only -verify=on %s
// RUN: %clang_cc1 -fsyntax-only -Wno-embedded-jit -verify=off %s
// RUN: %clang_cc1 -fsyntax-only -Wembedded-jit -Wembedded-jit-addr-of-period-const -verify=explicit %s

// EmbeddedJIT: verify the -Wembedded-jit toggle for the
// "period_const field modified without ejit_guard" warning
// and the separate -Wembedded-jit-addr-of-period-const toggle for the
// "address of period_const field without const qualifier" warning.
// Both are off-by-default in the driver; the latter is not part of
// -Wembedded-jit and must be enabled explicitly.

struct S {
  __attribute__((ejit_period_const)) int a;
  int b;
};

__attribute__((ejit_group("cell"))) struct S g_s[2];

// -Wembedded-jit (default-on in cc1) enables the write warning.
// -Wembedded-jit-addr-of-period-const (off by default) enables the address-of
// warning and must be passed explicitly.
// -Wno-embedded-jit -> silent for write warning.
void write_without_lc(int i) {
  g_s[i].a = 1; // on-warning {{modifying ejit_period_const field 'a' of 'g_s' without ejit_guard attribute}} explicit-warning {{modifying ejit_period_const field 'a' of 'g_s' without ejit_guard attribute}}
  g_s[i].b = 1; // no warning: 'b' is not ejit_period_const
  int *p1 = &g_s[i].a;        // explicit-warning {{taking address of ejit_period_const field 'a' of 'g_s' without const qualifier}}
  const int *p2 = &g_s[i].a;  // no warning: const qualified
}

// ejit_guard sanctions the write -> never warns, under any toggle.
__attribute__((ejit_guard("cell")))
void write_with_lc(__attribute__((ejit_dim("cell"))) int i) {
  g_s[i].a = 1; // no warning
}

// off-no-diagnostics
