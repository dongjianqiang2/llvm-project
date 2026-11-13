// RUN: %clang_cc1 -fsyntax-only -verify %s
// REQUIRES: ejit-switch-case

struct Cfg { int value; };

// The motivating shape: one lifecycle dimension plus the runtime dim.
__attribute__((ejit_entry))
int good(__attribute__((ejit_period_arr_ind("cell"))) unsigned char cell,
         __attribute__((ejit_runtime_dim)) unsigned slotNo);

// The per-entry arm limit.
__attribute__((ejit_entry))
int good_n(__attribute__((ejit_period_arr_ind("cell"))) unsigned char cell,
           __attribute__((ejit_runtime_dim(4))) unsigned slotNo);

// A 0-dim entry may carry it: its single identity gets the arms.
__attribute__((ejit_entry))
int zero_dim(__attribute__((ejit_runtime_dim)) unsigned short slotNo);

// The macro spelling used by EJitRuntime.h's ejit_runtime_dim_n(n).
__attribute__((ejit_entry))
int dunder(__attribute__((__ejit_runtime_dim__(2))) unsigned slotNo);

// Narrow and signed integers are fine; the JIT only needs 32 bits.
__attribute__((ejit_entry))
int narrow(__attribute__((ejit_runtime_dim)) signed char slotNo);

__attribute__((ejit_entry))
int wide(__attribute__((ejit_runtime_dim)) unsigned long long slotNo);
// expected-error@-1 {{ejit_runtime_dim parameter 'slotNo' must have integer type of at most 32 bits}}

__attribute__((ejit_entry))
int pointer(__attribute__((ejit_runtime_dim)) struct Cfg *slotNo);
// expected-error@-1 {{ejit_runtime_dim parameter 'slotNo' must have integer type of at most 32 bits}}

__attribute__((ejit_entry))
int zero_arms(__attribute__((ejit_runtime_dim(0))) unsigned slotNo);
// expected-error@-1 {{ejit_runtime_dim arm limit must be between 1 and 255}}

__attribute__((ejit_entry))
int many_arms(__attribute__((ejit_runtime_dim(256))) unsigned slotNo);
// expected-error@-1 {{ejit_runtime_dim arm limit must be between 1 and 255}}

// Conflicts, in either attribute order: the checks run post-merge.
__attribute__((ejit_entry))
int conflict_dim(
    __attribute__((ejit_period_arr_ind("cell"), ejit_runtime_dim)) unsigned c);
// expected-error@-1 {{parameter 'c' cannot be both ejit_runtime_dim and ejit_period_arr_ind}}

__attribute__((ejit_entry))
int conflict_dim_reversed(
    __attribute__((ejit_runtime_dim, ejit_period_arr_ind("cell"))) unsigned c);
// expected-error@-1 {{parameter 'c' cannot be both ejit_runtime_dim and ejit_period_arr_ind}}

__attribute__((ejit_entry))
int conflict_free(__attribute__((ejit_period_arr_ind("cell"))) unsigned cell,
                  __attribute__((ejit_free_dim, ejit_runtime_dim)) unsigned s);
// expected-error@-1 {{parameter 's' cannot be both ejit_runtime_dim and ejit_free_dim}}

// At most one per function in v1.
__attribute__((ejit_entry))
int two(__attribute__((ejit_runtime_dim)) unsigned a,
        __attribute__((ejit_runtime_dim)) unsigned b);
// expected-error@-1 {{function 'two' has more than one ejit_runtime_dim parameter}}

// Only an entry is specialized.
int not_an_entry(__attribute__((ejit_runtime_dim)) unsigned slotNo);
// expected-error@-1 {{ejit_runtime_dim parameter 'slotNo' requires function 'not_an_entry' to be ejit_entry}}

// ejit_entry written on an earlier declaration still counts.
__attribute__((ejit_entry))
int split_entry(unsigned slotNo);
int split_entry(__attribute__((ejit_runtime_dim)) unsigned slotNo);

int not_a_parameter __attribute__((ejit_runtime_dim));
// expected-warning@-1 {{'ejit_runtime_dim' attribute only applies to parameters}}
