// RUN: %clang_cc1 -flto=full -O1 -emit-llvm-bc -o %t.bc %s
// RUN: opt -passes='cgscc(inline),ejit-aot-module' -S %t.bc | FileCheck %s
//
// FullLTO hazard: in buildLTODefaultPipeline the inliner runs BEFORE
// EJitAotModulePass (PASS2-4 run last, after all LTO optimization). A small,
// otherwise-inlineable ejit_entry / ejit_period_lc would be merged into its
// caller before PASS3/PASS4 can wrap/instrument it, silently dropping the
// JIT wrapper and the lifecycle activate/deactivate.
//
// Reproduction: step 1 is a real FullLTO pre-link (-flto=full): the LTO
// pre-link pipeline runs PASS1 but SKIPS PASS2-4 (gated on !isLTOPreLink),
// so the bitcode carries the CodeGen noinline on the ejit functions but no
// wrapper yet. Step 2 runs the inliner (cgscc(inline)) followed by PASS2-4
// (ejit-aot-module) - the same inliner-before-PASS3 ordering as the FullLTO
// post-link. The static entry/lifecycle must survive the inliner (noinline;
// without it they would be inlined into @caller and dropped by GlobalDCE)
// and still receive the wrapper (jit_entry label + taskpool call) and the
// ejit_deactivate/ejit_activate calls.

struct Cfg { int t; };
__attribute__((ejit_period_arr("cell"))) struct Cfg g_cell[16];
extern void sink(int);

// Small, otherwise-inlineable entry. @caller calls it; the functions are
// static so that, without noinline, the -O1 inliner would merge them into
// @caller and GlobalDCE would drop them - leaving PASS3/PASS4 nothing to
// wrap/instrument. With the CodeGen noinline they survive for PASS3/PASS4.
__attribute__((ejit_entry))
static void entry(__attribute__((ejit_period_arr_ind("cell"))) int i) { sink(g_cell[i].t); }

__attribute__((ejit_period_lc("cell")))
static void lc(__attribute__((ejit_period_arr_ind("cell"))) int i) { g_cell[i].t = 1; }

void caller(int i) { entry(i); lc(i); }

// PASS3 wrapper survives on @entry (it was not inlined into @caller).
// CHECK: define {{.*}} @entry(
// CHECK: jit_entry:
// CHECK: call {{.*}} @ejit_taskpool_compile_or_get_

// PASS4 lifecycle survives on @lc (it was not inlined into @caller).
// CHECK: define {{.*}} @lc(
// CHECK: call {{.*}} @ejit_deactivate(
// CHECK: call {{.*}} @ejit_activate(
