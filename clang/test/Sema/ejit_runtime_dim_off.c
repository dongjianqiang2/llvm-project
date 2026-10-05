// RUN: %clang_cc1 -fsyntax-only -verify %s
// UNSUPPORTED: ejit-switch-case

// Built without EJIT_SWITCH_CASE, the attribute is diagnosed and dropped. It
// asserts nothing, so ignoring it is always safe.

__attribute__((ejit_entry))
int process(__attribute__((ejit_period_arr_ind("cell"))) unsigned char cell,
            __attribute__((ejit_runtime_dim)) unsigned slotNo) {
// expected-warning@-1 {{ejit_runtime_dim is ignored: this compiler was built without EJIT_SWITCH_CASE}}
  return slotNo;
}

// Type errors are still errors, so a source does not start failing only when
// the option is turned on.
__attribute__((ejit_entry))
int wide(__attribute__((ejit_runtime_dim)) unsigned long long slotNo);
// expected-error@-1 {{ejit_runtime_dim parameter 'slotNo' must have integer type of at most 32 bits}}
