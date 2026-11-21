// RUN: %clang_cc1 -fsyntax-only -verify=on %s
// RUN: %clang_cc1 -fsyntax-only -Wno-embedded-jit-attr-missing-on-def -verify=off %s
// RUN: %clang_cc1 -fsyntax-only -Wno-embedded-jit -verify=groupoff %s

// EmbeddedJIT: the "definition omits ejit_entry / ejit_guard" warning is
// default-on and has its own switch: -Wno-embedded-jit-attr-missing-on-def
// silences just this warning; -Wno-embedded-jit silences the whole group.

__attribute__((ejit_entry)) int entry_fn(void); // header declaration
// on-note@-1 {{'ejit_entry' declared here}}
int entry_fn(void) { return 1; } // on-warning {{function 'entry_fn' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// off-no-diagnostics
// groupoff-no-diagnostics
