// RUN: %clang_cc1 -fsyntax-only -verify %s

struct Cfg { int value; };

// The motivating shape: one lifecycle dimension plus one free dimension.
__attribute__((ejit_entry))
void good(__attribute__((ejit_period_arr_ind("unit"))) unsigned trp,
          __attribute__((ejit_free_dim)) unsigned slotNo);

// A free dim is not a dimension on the wire, so it does not consume one of the
// four ejit_period_arr_ind slots: four dims plus a free dim is fine.
__attribute__((ejit_entry))
void four_dims_plus_free(
    __attribute__((ejit_period_arr_ind("a"))) int a,
    __attribute__((ejit_period_arr_ind("b"))) int b,
    __attribute__((ejit_period_arr_ind("c"))) int c,
    __attribute__((ejit_period_arr_ind("d"))) int d,
    __attribute__((ejit_free_dim)) int slotNo);

// Width is irrelevant: nothing narrows a free dim for the ABI, and the only
// value ever substituted is the witness 0.
__attribute__((ejit_entry))
void wide(__attribute__((ejit_period_arr_ind("unit"))) unsigned trp,
          __attribute__((ejit_free_dim)) unsigned long long slotNo);

__attribute__((ejit_entry))
void not_integer(__attribute__((ejit_period_arr_ind("unit"))) unsigned trp,
                 __attribute__((ejit_free_dim)) struct Cfg *slotNo);
// expected-error@-1 {{ejit_free_dim parameter 'slotNo' must have integer type}}

__attribute__((ejit_entry))
void floating(__attribute__((ejit_period_arr_ind("unit"))) unsigned trp,
              __attribute__((ejit_free_dim)) double slotNo);
// expected-error@-1 {{ejit_free_dim parameter 'slotNo' must have integer type}}

// Conflicts: a free dim asserts the data does not vary with the parameter,
// which contradicts putting the same parameter on the specialization identity.
__attribute__((ejit_entry))
void conflict_dim(
    __attribute__((ejit_period_arr_ind("unit"), ejit_free_dim)) unsigned trp);
// expected-error@-1 {{parameter 'trp' cannot be both ejit_free_dim and ejit_period_arr_ind}}

// Order-independent: the conflict check runs on the merged declaration, so it
// fires whichever attribute is written first.
__attribute__((ejit_entry))
void conflict_dim_reversed(
    __attribute__((ejit_free_dim, ejit_period_arr_ind("unit"))) unsigned trp);
// expected-error@-1 {{parameter 'trp' cannot be both ejit_free_dim and ejit_period_arr_ind}}

// No conflict check is needed against ejit_bound_ptr: it requires a pointer and
// a free dim requires an integer, so the type rule already excludes the pair.
__attribute__((ejit_entry))
void conflict_bound(__attribute__((ejit_period_arr_ind("cell"))) int cell,
                    __attribute__((ejit_bound_ptr("cell"), ejit_free_dim))
                        struct Cfg *cfg);
// expected-error@-2 {{ejit_free_dim parameter 'cfg' must have integer type}}

// A free dim only means something on an entry: nothing specializes anything
// else, so the assertion would have no consumer.
void not_an_entry(__attribute__((ejit_free_dim)) unsigned slotNo);
// expected-error@-1 {{ejit_free_dim parameter 'slotNo' requires function 'not_an_entry' to be ejit_entry}}

// A free dim adds no dimension of its own, so an entry with no lifecycle
// dimension has nothing that can ever invalidate the values it freezes.
__attribute__((ejit_entry))
void no_lifecycle(__attribute__((ejit_free_dim)) unsigned slotNo);
// expected-warning@-1 {{ejit_entry function 'no_lifecycle' has no ejit_period_arr_ind parameter; ejit_may_const values frozen through ejit_free_dim parameter 'slotNo' can never be invalidated}}

// ejit_entry written on an earlier declaration still counts: the check runs
// after the redeclaration merge.
__attribute__((ejit_entry))
void split_entry(unsigned trp, unsigned slotNo);
void split_entry(__attribute__((ejit_period_arr_ind("unit"))) unsigned trp,
                 __attribute__((ejit_free_dim)) unsigned slotNo);

// Note: whether a parameter gets a usable IR argument index is an ABI question
// and cannot be decided here. Sema accepts any integer; CodeGen drops the
// annotation with a warning when the target passes it indirectly. That case is
// target-specific, so it lives in CodeGen/ejit_free_dim.c under an explicit
// triple rather than here.
