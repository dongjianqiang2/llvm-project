// RUN: %clang_cc1 -std=c++17 -fsyntax-only -verify %s

__attribute__((ejit_entry))
unsigned good(unsigned cell __attribute__((ejit_dim("cell"))), unsigned value) {
  const unsigned local __attribute__((ejit_free_dim)) = value;
  return local + cell;
}

__attribute__((ejit_entry))
void references(unsigned cell __attribute__((ejit_dim("cell")))) {
  const unsigned a __attribute__((ejit_free_dim)) = cell;
  const unsigned &ref = a; // expected-error {{value reads only}}
  unsigned b __attribute__((ejit_free_dim)) = cell;
  auto capture = [&b] { return b; }; // expected-error {{no closure capture}}
  unsigned c __attribute__((ejit_free_dim)) = cell;
  auto copy = [c] { return c; }; // expected-error {{no closure capture}}
  unsigned d __attribute__((ejit_free_dim)) = cell;
  auto init_copy = [value = d] { return value; }; // expected-error {{no closure capture}}
}
