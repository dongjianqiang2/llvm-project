// RUN: %clang_cc1 -fsyntax-only -Wembedded-jit-undeclared-period-dep -verify=expected %s
// EmbeddedJIT: ejit_const_dim semantic analysis.

struct TrpCfg {
  __attribute__((ejit_may_const)) int cpriMode;
  int xx;
};

__attribute__((ejit_period_arr("trp"))) struct TrpCfg g_trp[16];

// === Correct usage -- no diagnostics ===

// A const dim alongside a period dim: the driving case.
__attribute__((ejit_entry))
void init_trp(__attribute__((ejit_dim("trp"))) unsigned trpIdx,
              __attribute__((ejit_const_dim)) unsigned slotNo);

// A const dim on its own, with no period array in sight. Nothing about
// ejit_const_dim requires a period to exist.
__attribute__((ejit_entry))
int mode_select(__attribute__((ejit_const_dim)) unsigned char numerology);

// Every integer width up to 32 bits is accepted, signed or not.
__attribute__((ejit_entry))
void widths(__attribute__((ejit_const_dim)) char a,
            __attribute__((ejit_const_dim)) short b,
            __attribute__((ejit_const_dim)) int c,
            __attribute__((ejit_const_dim)) unsigned d);

// A const dim declares no period, so it must NOT be demanded as one: this
// function indexes g_trp under the "trp" dim it does declare, and the
// undeclared-period-dependency warning must stay silent about slotNo.
__attribute__((ejit_entry))
int uses_period(__attribute__((ejit_dim("trp"))) unsigned trpIdx,
                __attribute__((ejit_const_dim)) unsigned slotNo) {
  return g_trp[trpIdx * 5 + slotNo % 5].cpriMode;
}

// === Error: non-integer parameter ===
__attribute__((ejit_entry))
void bad_type(__attribute__((ejit_const_dim)) float slotNo);
// expected-error@-1 {{ejit_const_dim parameter 'slotNo' must have integer type}}

__attribute__((ejit_entry))
void bad_type_ptr(__attribute__((ejit_const_dim)) int *slotNo);
// expected-error@-1 {{ejit_const_dim parameter 'slotNo' must have integer type}}

// === Error: wider than 32 bits ===
// The wrapper narrows the argument to i32 before the runtime can range-check
// it, so a wider parameter would alias two values onto one specialization.
__attribute__((ejit_entry))
void too_wide(__attribute__((ejit_const_dim)) long long slotNo);
// expected-error@-1 {{ejit_const_dim parameter 'slotNo' has 64 bits, which exceeds the maximum of 32}}

__attribute__((ejit_entry))
void too_wide_unsigned(__attribute__((ejit_const_dim)) unsigned long long slotNo);
// expected-error@-1 {{ejit_const_dim parameter 'slotNo' has 64 bits, which exceeds the maximum of 32}}

// === Error: both dim kinds on one parameter, in either order ===
__attribute__((ejit_entry))
void both_kinds(__attribute__((ejit_dim("trp")))
                __attribute__((ejit_const_dim)) unsigned idx);
// expected-error@-1 {{'ejit_const_dim' and 'ejit_period_arr_ind' cannot be combined on parameter 'idx'}}

__attribute__((ejit_entry))
void both_kinds_reversed(__attribute__((ejit_const_dim))
                         __attribute__((ejit_dim("trp"))) unsigned idx);
// expected-error@-1 {{'ejit_const_dim' and 'ejit_period_arr_ind' cannot be combined on parameter 'idx'}}

// === Error: the 4-dim budget is shared between both kinds ===
// Three period dims plus two const dims is five, even though neither kind
// exceeds four on its own.
__attribute__((ejit_entry))
void too_many_mixed(
    __attribute__((ejit_dim("a"))) int a,
    __attribute__((ejit_dim("b"))) int b,
    __attribute__((ejit_dim("c"))) int c,
    __attribute__((ejit_const_dim)) int d,
    __attribute__((ejit_const_dim)) int e);
// expected-error@-1 {{function 'too_many_mixed' has 5 specialization dimension parameters (ejit_period_arr_ind / ejit_const_dim), which exceeds the maximum of 4}}

// Exactly four, mixed, is fine.
__attribute__((ejit_entry))
void four_mixed(
    __attribute__((ejit_dim("a"))) int a,
    __attribute__((ejit_dim("b"))) int b,
    __attribute__((ejit_const_dim)) int c,
    __attribute__((ejit_const_dim)) int d);

// === Warning: wrong subject (attribute is dropped) ===
__attribute__((ejit_const_dim)) int g_not_a_param;
// expected-warning@-1 {{'ejit_const_dim' attribute only applies to parameters}}

// === Error: takes no arguments ===
__attribute__((ejit_entry))
void takes_no_args(__attribute__((ejit_const_dim(20))) int slotNo);
// expected-error@-1 {{'ejit_const_dim' attribute takes no arguments}}
