// RUN: %clang_cc1 -fsyntax-only -verify %s

extern volatile unsigned state;
extern _Atomic(unsigned) atomic_state;
extern unsigned next_slot(void);
extern void escape(const unsigned *);

__attribute__((ejit_entry))
unsigned good(unsigned cell __attribute__((ejit_dim("cell"))), unsigned input) {
  const unsigned a __attribute__((ejit_free_dim)) = input;
  unsigned b __attribute__((ejit_free_dim)) = state;
  unsigned c __attribute__((ejit_free_dim)) = atomic_state;
  unsigned d __attribute__((ejit_free_dim)) = next_slot();
  return a + (b) + c + d + cell;
}

unsigned global __attribute__((ejit_free_dim)); // expected-error {{requires automatic storage duration}}

void helper(void) {
  unsigned x __attribute__((ejit_free_dim)) = state; // expected-error {{directly in an ejit_entry function}}
}

__attribute__((ejit_entry))
void bad(unsigned cell __attribute__((ejit_dim("cell")))) {
  unsigned missing __attribute__((ejit_free_dim)); // expected-error {{an initializer}}
  static unsigned st __attribute__((ejit_free_dim)) = 0; // expected-error {{automatic storage duration}}
  _Thread_local static unsigned tls __attribute__((ejit_free_dim)) = 0; // expected-error {{automatic storage duration}}
  volatile unsigned vol __attribute__((ejit_free_dim)) = 0; // expected-error {{non-volatile, non-atomic integer scalar}}
  _Atomic(unsigned) atom __attribute__((ejit_free_dim)) = 0; // expected-error {{non-volatile, non-atomic integer scalar}}
  double fp __attribute__((ejit_free_dim)) = 0; // expected-error {{integer scalar}}
  unsigned *ptr __attribute__((ejit_free_dim)) = 0; // expected-error {{integer scalar}}
  unsigned a __attribute__((ejit_free_dim)) = cell;
  a = 1; // expected-error {{value reads only}}
  unsigned b __attribute__((ejit_free_dim)) = cell;
  b++; // expected-error {{value reads only}}
  unsigned c __attribute__((ejit_free_dim)) = cell;
  --c; // expected-error {{value reads only}}
  unsigned d __attribute__((ejit_free_dim)) = cell;
  d += 2; // expected-error {{value reads only}}
  const unsigned e __attribute__((ejit_free_dim)) = cell;
  escape(&e); // expected-error {{value reads only}}
}

__attribute__((ejit_entry))
void no_lifecycle(void) {
  unsigned x __attribute__((ejit_free_dim)) = state; // expected-error {{managed ejit_dim lifecycle}}
}
