// RUN: %clang_cc1 -fsyntax-only -Wembedded-jit-addr-of-period-const -verify=expected %s
// EmbeddedJIT attribute semantic analysis tests

// === Correct usage -- should produce no diagnostics ===

struct CellConfig {
  __attribute__((ejit_period_const)) int cellType;
  __attribute__((ejit_period_const)) unsigned flags;
  __attribute__((ejit_period_const)) float ratio;
  int xx;
};

__attribute__((ejit_group("static"))) int g_board;
__attribute__((ejit_group("cell"))) struct CellConfig g_cells[16];

__attribute__((ejit_entry))
void process_task(__attribute__((ejit_dim("cell"))) int idx);

__attribute__((ejit_guard("cell")))
void update_config(__attribute__((ejit_dim("cell"))) int idx);

// === ejit_group infers the form from the variable type ===
__attribute__((ejit_group("cell"))) struct CellConfig g_group_arr[8]; // indexed
__attribute__((ejit_group("static"))) int g_group_scalar;             // scalar
__attribute__((ejit_group("cell"))) struct CellConfig *g_group_ptr;   // indexed, dynamic size

// === Error: ejit_group on a pointer to a non-struct ===
__attribute__((ejit_group("cell"))) int *g_group_bad_ptr;
// expected-error@-1 {{ejit_group attribute requires an array type or a pointer to a struct; 'g_group_bad_ptr' does not qualify}}

// === Error: duplicate group attributes (only second usage triggers error) ===
__attribute__((ejit_group("one")))
__attribute__((ejit_group("two")))
// expected-error@-1 {{variable 'g_conflict' cannot have multiple ejit_group attributes}}
int g_conflict;

// === Legacy spellings are removed: unknown attributes, ignored with a warning ===
__attribute__((ejit_period("cell"))) int g_legacy_scalar; // expected-warning {{unknown attribute 'ejit_period' ignored}}
__attribute__((ejit_period_arr("cell"))) int g_legacy_array[4]; // expected-warning {{unknown attribute 'ejit_period_arr' ignored}}

// === Error: ejit_dim on non-integer parameter ===
__attribute__((ejit_entry))
void bad_ind_type(__attribute__((ejit_dim("cell"))) float badIdx);
// expected-error@-1 {{ejit_dim parameter 'badIdx' must have integer type}}

// === Error: too many ejit_dim parameters ===
__attribute__((ejit_entry))
void too_many_ind(
    __attribute__((ejit_dim("a"))) int a,
    __attribute__((ejit_dim("b"))) int b,
    __attribute__((ejit_dim("c"))) int c,
    __attribute__((ejit_dim("d"))) int d,
    __attribute__((ejit_dim("e"))) int e);
// expected-error@-1 {{function 'too_many_ind' has 5 ejit_dim parameters, which exceeds the maximum of 4}}

// === Error: ejit_guard without matching dim parameter ===
// Note: %0 is printed without quotes for string arguments
__attribute__((ejit_guard("nonexistent")))
// expected-error@-1 {{ejit_guard(nonexistent) requires a corresponding ejit_dim(nonexistent) parameter}}
void bad_lc(int x);

// === Warning: writing an ejit_period_const field without ejit_guard ===
struct WarnCfg {
  __attribute__((ejit_period_const)) int cellType;
  __attribute__((ejit_period_const)) unsigned flags;
  int plain;
};

__attribute__((ejit_group("cell"))) struct WarnCfg g_warn[4];

void consume(int *p);
void inspect(const int *p);
__attribute__((ejit_guard("cell")))
void lc_consume(__attribute__((ejit_dim("cell"))) int idx, int *p);

// A plain function (no ejit_guard) that writes period_const fields -> warn.
void bad_writer(int i, int v) {
  g_warn[i].cellType = v;  // expected-warning {{modifying ejit_period_const field 'cellType' of 'g_warn' without ejit_guard attribute}}
  g_warn[i].flags += v;    // expected-warning {{modifying ejit_period_const field 'flags' of 'g_warn' without ejit_guard attribute}}
  g_warn[i].cellType++;    // expected-warning {{modifying ejit_period_const field 'cellType' of 'g_warn' without ejit_guard attribute}}
  ++g_warn[i].flags;       // expected-warning {{modifying ejit_period_const field 'flags' of 'g_warn' without ejit_guard attribute}}
  g_warn[i].plain = v;     // no warning: 'plain' is not ejit_period_const
  int r = g_warn[i].cellType; // no warning: read, not a write
  (void)r;
  int *p1 = &g_warn[i].cellType;        // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}}
  const int *p2 = &g_warn[i].cellType;  // no warning: const qualifier on pointee
  int *p3 = (int *)&g_warn[i].cellType; // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}} (explicit cast does not add const)
  int *p4; p4 = &g_warn[i].cellType;    // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}} (plain assignment)
  int &r1 = g_warn[i].cellType;         // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}} (non-const reference)
  const int &r2 = g_warn[i].cellType;   // no warning: const reference
  consume(&g_warn[i].cellType);         // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}} (call argument)
  inspect(&g_warn[i].cellType);         // no warning: const parameter
  lc_consume(i, &g_warn[i].cellType);   // no warning: ejit_guard callee is sanctioned
  int *p5{&g_warn[i].cellType};         // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}} (braced init)
  (void)p1; (void)p2; (void)p3; (void)p4; (void)r1; (void)r2; (void)p5;
}

// Returning a non-const reference to a period_const field -> warn (same escape
// as a non-const pointer).
int &bad_ref_return(int i) {
  return g_warn[i].cellType; // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}}
}

// Extern declaration whose definition lives in another TU: the definition may
// carry the group attribute, so treat it as possibly period data -> warn.
extern struct WarnCfg g_ext[4];
void extern_writer(int i) {
  g_ext[i].cellType = 1;       // expected-warning {{modifying ejit_period_const field 'cellType' of 'g_ext' without ejit_guard attribute}}
  int *p = &g_ext[i].cellType; // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_ext' without const qualifier}}
  (void)p;
}

// Return statements inside a lambda are checked against the lambda's return
// type, not the enclosing function's.
const int *lambda_escape(int i) {
  auto L = [i]() -> int * { return &g_warn[i].cellType; }; // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}}
  return L();
}
int *lambda_safe(int i) {
  auto L = [i]() -> const int * { return &g_warn[i].cellType; }; // no warning: lambda returns const pointer
  return const_cast<int *>(L());
}

// Methods of local classes are checked once, via the enclosing function's
// traversal, against the method's own return type.
int *local_class_escape(int i) {
  struct H { int *get() { return &g_warn[0].cellType; } }; // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}}
  H h; return h.get();
}
const int *local_class_safe(int i) {
  struct H { const int *get() { return &g_warn[0].cellType; } }; // no warning: const method return type
  H h; return h.get();
}

// Returning a non-const pointer to a period_const field -> warn.
int *bad_return(int i) {
  return &g_warn[i].cellType; // expected-warning {{taking address of ejit_period_const field 'cellType' of 'g_warn' without const qualifier}}
}

// Non-period global: value-type base, not period data -> no warning.
WarnCfg g_plain[4];

void plain_global_writer(int i) {
  g_plain[i].cellType = 1;       // no warning: g_plain is not period data
  int *p = &g_plain[i].cellType; // no warning: value-type non-period global
  (void)p;
}

// Local struct copy: value type, not period data -> no warning.
void local_copy_writer(int i) {
  WarnCfg local = g_warn[i];
  local.cellType = 1;             // no warning: local copy, not period data
  int *p = &local.cellType;       // no warning: local copy
  (void)p;
}

// Member pointer formation does not escape a writable pointer -> no warning.
void member_ptr_writer() {
  int WarnCfg::*mp = &WarnCfg::cellType;  // no warning
  (void)mp;
}

// An ejit_guard function is sanctioned to modify period_const fields -> no warning.
__attribute__((ejit_guard("cell")))
void good_writer(__attribute__((ejit_dim("cell"))) int i, int v) {
  g_warn[i].cellType = v;  // no warning: ejit_guard sanctions the write
  g_warn[i].flags += v;    // no warning
  int *p1 = &g_warn[i].cellType;        // no warning: guard is exempt
  (void)p1;
}

// === Warning: always_inline conflicts with ejit_entry / ejit_guard ===
// These functions must stay out-of-line: CodeGen/PASS3 mark them noinline so
// they survive the LTO inliner for PASS1/PASS3/PASS4. always_inline is
// incompatible ('noinline and alwaysinline' aborts the verifier), so it is
// dropped after warning. The check is deferred to ActOnFunctionDeclarator
// (after all attributes are processed), so both source orders are rejected.
__attribute__((always_inline, ejit_entry))  // expected-warning {{'always_inline' is incompatible with ejit_entry}} expected-note {{conflicting attribute is here}}
void ai_entry(__attribute__((ejit_dim("cell"))) int idx) { g_warn[idx].plain = idx; }

__attribute__((ejit_entry, always_inline))  // expected-warning {{'always_inline' is incompatible with ejit_entry}} expected-note {{conflicting attribute is here}}
void ai_entry_rev(__attribute__((ejit_dim("cell"))) int idx) { g_warn[idx].plain = idx; }

__attribute__((always_inline, ejit_guard("cell")))  // expected-warning {{'always_inline' is incompatible with ejit_guard}} expected-note {{conflicting attribute is here}}
void ai_lc(__attribute__((ejit_dim("cell"))) int idx) { g_warn[idx].plain = idx; }

// No ejit attribute -> always_inline is fine, no warning.
__attribute__((always_inline)) inline void plain_ai(int x) { (void)x; }
