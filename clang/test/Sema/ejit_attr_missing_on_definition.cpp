// RUN: %clang_cc1 -fsyntax-only -verify %s

// EmbeddedJIT: a function declared with ejit_entry / ejit_guard in a
// header but defined without the attribute is not JIT-specialized. The
// definition must repeat the attribute; otherwise a warning is emitted and
// the inherited attribute is dropped, so the definition compiles as a plain
// AOT function (see clang/test/CodeGen/ejit_attr_missing_on_definition.c).

// --- Warning: declared with ejit_entry, defined without it ---

__attribute__((ejit_entry)) int entry_fn(void); // header declaration
// expected-note@-1 {{'ejit_entry' declared here}}
int entry_fn(void) { return 1; } // expected-warning {{function 'entry_fn' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// --- Warning: declared with ejit_guard, defined without it ---

__attribute__((ejit_guard("static")))
void lc_fn(__attribute__((ejit_dim("static"))) int idx);
// expected-note@-2 {{'ejit_guard' declared here}}
void lc_fn(int idx) {} // expected-warning {{function 'lc_fn' is declared with 'ejit_guard' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// --- Error: ejit_entry and ejit_guard cannot be combined ---
// PASS3 (EJitWrapperGen) rewrites an entry's body and PASS4
// (EJitPeriodHandler) inserts lifecycle guards; one function cannot be both.

__attribute__((ejit_entry, ejit_guard("static")))
void conflict_fn(__attribute__((ejit_dim("static"))) int idx) {}
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'conflict_fn'}}
// expected-note@-3 {{conflicting attribute is here}}

// --- Error regardless of attribute order ---

__attribute__((ejit_guard("static"), ejit_entry))
void conflict_fn_rev(__attribute__((ejit_dim("static"))) int idx) {}
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'conflict_fn_rev'}}
// expected-note@-3 {{conflicting attribute is here}}

// --- Error when the two attributes arrive on different declarations ---

__attribute__((ejit_entry))
void cross_fn(__attribute__((ejit_dim("static"))) int idx);
// expected-note@-2 {{conflicting attribute is here}}
__attribute__((ejit_guard("static")))
void cross_fn(__attribute__((ejit_dim("static"))) int idx);
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'cross_fn'}}

// --- No duplicate conflict error: the pair was already diagnosed on the
//     prototype; the definition repeating only ejit_entry gets the usual
//     missing-on-definition treatment for the omitted ejit_guard ---

__attribute__((ejit_entry, ejit_guard("static")))
void once_fn(__attribute__((ejit_dim("static"))) int idx);
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'once_fn'}}
// expected-note@-3 {{conflicting attribute is here}}
// expected-note@-4 {{'ejit_guard' declared here}}
__attribute__((ejit_entry))
void once_fn(__attribute__((ejit_dim("static"))) int idx) {}
// expected-warning@-1 {{function 'once_fn' is declared with 'ejit_guard' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// --- Error with the reverse cross-declaration order: the error points at
//     the attribute written on THIS declaration, the note at the inherited
//     one ---

__attribute__((ejit_guard("static")))
void cross_fn_rev(__attribute__((ejit_dim("static"))) int idx);
// expected-note@-2 {{conflicting attribute is here}}
__attribute__((ejit_entry))
void cross_fn_rev(__attribute__((ejit_dim("static"))) int idx);
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'cross_fn_rev'}}

// --- Error: both attributes written on an explicit instantiation declarator
//     (that path bypasses ActOnFunctionDeclarator) ---

template <typename T>
void ei_fn(__attribute__((ejit_dim("static"))) int) {}
template __attribute__((ejit_entry, ejit_guard("static")))
void ei_fn<int>(__attribute__((ejit_dim("static"))) int);
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'ei_fn<int>'}}
// expected-note@-3 {{conflicting attribute is here}}

// --- Error: an explicit instantiation assembles the pair (ejit_entry on the
//     pattern, ejit_guard written here) ---

template <typename T>
__attribute__((ejit_entry))
void ei_fn2(__attribute__((ejit_dim("static"))) int) {}
// expected-note@-2 {{conflicting attribute is here}}
template __attribute__((ejit_guard("static")))
void ei_fn2<int>(__attribute__((ejit_dim("static"))) int);
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'ei_fn2<int>'}}

// --- No duplicate: the pair was diagnosed on the pattern; a plain explicit
//     instantiation reproduces it silently ---

template <typename T>
__attribute__((ejit_entry, ejit_guard("static")))
void ei_fn3(__attribute__((ejit_dim("static"))) int) {}
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'ei_fn3'}}
// expected-note@-3 {{conflicting attribute is here}}
template void ei_fn3<int>(__attribute__((ejit_dim("static"))) int);

// --- Error with the mirror order: ejit_guard on the pattern, ejit_entry
//     written on the instantiation. The error anchors at the attribute
//     written HERE, the note at the pre-existing one ---

template <typename T>
__attribute__((ejit_guard("static")))
void ei_fn4(__attribute__((ejit_dim("static"))) int) {}
// expected-note@-2 {{conflicting attribute is here}}
template __attribute__((ejit_entry))
void ei_fn4<int>(__attribute__((ejit_dim("static"))) int);
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'ei_fn4<int>'}}

// --- The pair assembled across TWO explicit instantiation declarations
//     (an extern declaration then the definition), lc first then entry: the
//     error anchors at the entry written on the current declaration ---

template <typename T>
void m_fn(__attribute__((ejit_dim("static"))) int) {}
extern template __attribute__((ejit_guard("static")))
void m_fn<int>(__attribute__((ejit_dim("static"))) int);
// expected-note@-2 {{conflicting attribute is here}}
template __attribute__((ejit_entry))
void m_fn<int>(__attribute__((ejit_dim("static"))) int);
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'm_fn<int>'}}

// --- ... and entry first then lc: the error anchors at the lc written on
//     the current declaration ---

template <typename T>
void m_fn2(__attribute__((ejit_dim("static"))) int) {}
extern template __attribute__((ejit_entry))
void m_fn2<int>(__attribute__((ejit_dim("static"))) int);
// expected-note@-2 {{conflicting attribute is here}}
template __attribute__((ejit_guard("static")))
void m_fn2<int>(__attribute__((ejit_dim("static"))) int);
// expected-error@-2 {{'ejit_entry' and 'ejit_guard' cannot be combined on function 'm_fn2<int>'}}

// --- OK: the ejit_dim may live on an earlier declaration's
//     parameter; it is propagated by the declaration merge before the
//     ejit_guard no-index check runs ---

void ok_split_a(__attribute__((ejit_dim("cell"))) int idx);
__attribute__((ejit_guard("cell"))) void ok_split_a(int idx);

__attribute__((ejit_guard("cell")))
void ok_split_b(__attribute__((ejit_dim("cell"))) int idx);
__attribute__((ejit_guard("cell"))) void ok_split_b(int idx);

void ok_split_def(__attribute__((ejit_dim("cell"))) int idx);
__attribute__((ejit_guard("cell"))) void ok_split_def(int idx) {}

// --- Error: NO declaration provides a matching ejit_dim; the lc
//     written on the second declaration is diagnosed there ---

void missing_idx(__attribute__((ejit_dim("other"))) int idx);
__attribute__((ejit_guard("cell"))) void missing_idx(int idx);
// expected-error@-1 {{ejit_guard(cell) requires a corresponding ejit_dim(cell) parameter}}

// --- OK: an ejit_guard written on an explicit instantiation declarator
//     finds its ejit_dim on the pattern's parameter ---

template <typename T>
void ei_lc_ok(__attribute__((ejit_dim("cell"))) int) {}
template __attribute__((ejit_guard("cell")))
void ei_lc_ok<int>(int);

// --- Error: the pattern's parameter lacks the arr_ind, so an
//     ejit_guard written on the instantiation declarator has no index ---

template <typename T>
void ei_lc_missing(int) {}
template __attribute__((ejit_guard("cell")))
void ei_lc_missing<int>(int);
// expected-error@-2 {{ejit_guard(cell) requires a corresponding ejit_dim(cell) parameter}}

// --- Known limitation (pinned): clang silently discards parameter
//     attributes on explicit instantiation declarators, so an
//     ejit_dim written here never attaches and the check errors.
//     The arr_ind must live on the pattern's parameter. If clang ever starts
//     applying declarator param attributes on explicit instantiations, this
//     expectation must be revisited. ---

template <typename T>
void ei_lc_together(int) {}
template __attribute__((ejit_guard("cell")))
void ei_lc_together<int>(__attribute__((ejit_dim("cell"))) int);
// expected-error@-2 {{ejit_guard(cell) requires a corresponding ejit_dim(cell) parameter}}

// --- No warning: definition repeats the attribute ---

__attribute__((ejit_entry)) int ok_entry_fn(void);
__attribute__((ejit_entry)) int ok_entry_fn(void) { return 2; }

__attribute__((ejit_guard("static")))
void ok_lc_fn(__attribute__((ejit_dim("static"))) int idx);
__attribute__((ejit_guard("static")))
void ok_lc_fn(__attribute__((ejit_dim("static"))) int idx) {}

// --- No warning: mere redeclaration without attribute (no definition here) ---

__attribute__((ejit_entry)) int redecl_fn(void);
int redecl_fn(void);

// --- No warning: no prior declaration with the attribute ---

int plain_fn(void) { return 3; }

// --- Warning: out-of-line member definition without the attribute ---

struct Worker {
  __attribute__((ejit_entry)) void run(int idx); // in-class declaration
};
// expected-note@-2 {{'ejit_entry' declared here}}
void Worker::run(int idx) {} // expected-warning {{function 'run' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// --- Warning once at the template pattern; explicit instantiations are
//     handled silently (no duplicate warning) ---

template <class T> __attribute__((ejit_entry)) T tmpl_fn(T v);
// expected-note@-1 {{'ejit_entry' declared here}}
template <class T> T tmpl_fn(T v) { return v; } // expected-warning {{function 'tmpl_fn' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}
template int tmpl_fn(int); // instantiates the body: no extra warning

// --- Warning: explicit specialization is a definition of its own ---

template <class T> __attribute__((ejit_entry)) T spec_fn(T v);
// expected-note@-1 {{'ejit_entry' declared here}}
template <> int spec_fn<int>(int v) { return v; } // expected-warning {{function 'spec_fn<int>' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}
