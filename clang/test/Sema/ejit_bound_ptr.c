// RUN: %clang_cc1 -fsyntax-only -verify %s

struct Cfg { int value; };

__attribute__((ejit_entry))
void good(__attribute__((ejit_period_arr_ind("cell"))) int cell,
          __attribute__((ejit_bound_ptr("cell"))) const struct Cfg *cfg);

__attribute__((ejit_entry))
void not_pointer(__attribute__((ejit_period_arr_ind("cell"))) int cell,
                 __attribute__((ejit_bound_ptr("cell"))) int cfg);
// expected-error@-1 {{ejit_bound_ptr parameter 'cfg' must be a pointer to a complete object type}}

struct Incomplete;
__attribute__((ejit_entry))
void incomplete(__attribute__((ejit_period_arr_ind("cell"))) int cell,
                __attribute__((ejit_bound_ptr("cell"))) struct Incomplete *cfg);
// expected-error@-1 {{ejit_bound_ptr parameter 'cfg' must be a pointer to a complete object type}}

__attribute__((ejit_entry))
void missing_dim(__attribute__((ejit_period_arr_ind("trp"))) int trp,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *cfg);
// expected-error@-1 {{ejit_bound_ptr(cell) requires exactly one matching ejit_period_arr_ind(cell) parameter}}

__attribute__((ejit_entry))
void two_bound(__attribute__((ejit_period_arr_ind("cell"))) int cell,
               __attribute__((ejit_bound_ptr("cell"))) struct Cfg *a,
               __attribute__((ejit_bound_ptr("cell"))) struct Cfg *b);

__attribute__((ejit_entry))
void eight_bound(__attribute__((ejit_period_arr_ind("cell"))) int cell,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *a,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *b,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *c,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *d,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *e,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *f,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *g,
                 __attribute__((ejit_bound_ptr("cell"))) struct Cfg *h);

__attribute__((ejit_entry))
void nine_bound(__attribute__((ejit_period_arr_ind("cell"))) int cell,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *a,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *b,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *c,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *d,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *e,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *f,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *g,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *h,
                __attribute__((ejit_bound_ptr("cell"))) struct Cfg *i);
// expected-error@-1 {{function 'nine_bound' has 9 ejit_bound_ptr parameters; at most 8 are supported}}
