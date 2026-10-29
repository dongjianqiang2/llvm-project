// RUN: %clang_cc1 -fblocks -fsyntax-only -verify %s
__attribute__((ejit_entry))
void block_capture(unsigned cell __attribute__((ejit_dim("cell")))) {
  unsigned local __attribute__((ejit_free_dim)) = cell;
  void (^capture)(void) = ^{ (void)local; }; // expected-error {{no closure capture}}
}
