//===-- EJitPassOptions.cpp - Shared Command-Line Options -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
using namespace llvm;

cl::opt<bool> EnableEJitGlobalCtors(
    "enable-ejit-global-ctors", cl::init(true), cl::Hidden,
    cl::desc("Generate llvm.global_ctors for auto-registration "
             "(disable for bare-metal / testing)"));

cl::opt<std::string> EJitDumpBitcodeDir(
    "ejit-dump-bitcode-dir", cl::init(""), cl::Hidden,
    cl::desc("Directory to dump extracted EmbeddedJIT bitcode (.bc and .ll) "
             "at AOT compile time, for debugging symbol extraction. Each TU "
             "writes a PID + module-named file so parallel -j builds do not "
             "collide or serialize."));

// AOT specialization diagnostics (PASS1). Both default on; each guards its own
// warning so users can silence them independently via -mllvm.
cl::opt<bool> EJitWarnNoSpecialization(
    "ejit-warn-no-specialization", cl::init(true), cl::Hidden,
    cl::desc("Warn when an ejit_entry function reads no ejit_may_const field "
             "in its specialization closure (no JIT specialization value)."));

cl::opt<bool> EJitWarnUnusedDim(
    "ejit-warn-unused-dim", cl::init(true), cl::Hidden,
    cl::desc("Warn when an ejit_entry declares ejit_period_arr_ind(P) but its "
             "specialization closure neither reads that parameter nor "
             "indexes an ejit_period_arr(P), and no ejit_bound_ptr is bound "
             "to P (unused specialization dimension)."));

// Optional info-level report (not a warning): per ejit_entry, count the
// ejit_may_const reads in its specialization closure and how many sit inside
// loops. A raw count is a poor specialization-value metric on its own (a
// single read in a hot loop or feeding a branch is high value), so this only
// reports the numbers for the user to judge - it does not gate or warn.
cl::opt<bool> EJitReportMayConst(
    "ejit-report-mayconst", cl::init(false), cl::Hidden,
    cl::desc("Report per-ejit_entry ejit_may_const read counts (total and "
             "in-loop) in the specialization closure, for manual "
             "specialization-value assessment."));

// Optional warning (off by default): warn when an ejit_entry's specialization
// closure has fewer than N ejit_may_const reads. N=0 disables the check,
// N>0 enables it with that threshold. A low load count means the JIT has
// little to specialize against, but the significance depends on whether those
// few loads gate branches or sit in hot loops — this warning flags low counts
// for manual review rather than asserting a defect.
cl::opt<unsigned> EJitWarnFewMayConst(
    "ejit-warn-few-mayconst", cl::init(0), cl::Hidden,
    cl::desc("Warn when an ejit_entry's specialization closure has fewer than "
             "N ejit_may_const reads (0 = off). Example: "
             "-ejit-warn-few-mayconst=4 warns on 0..3 may-const reads."));

// Closure-slimming threshold (PASS1): non-entry closure helpers with at
// least this many raw-IR instructions are externalized from the extracted
// bitcode (declaration only, resolved via the AOT-side registration). Below
// it, the body is smaller than the ~190-byte registration record that
// replaces it. 0 removes the size floor (externalize every surviving
// helper). See jit_design_doc/EJIT_BITCODE_SLIMMING.md.
cl::opt<unsigned> EJitExternalizeMinInsts(
    "ejit-externalize-min-insts", cl::init(16), cl::Hidden,
    cl::desc("Externalize non-entry closure helpers with at least this many "
             "raw-IR instructions from the extracted bitcode (0 = no size "
             "floor; see jit_design_doc/EJIT_BITCODE_SLIMMING.md)"));

cl::opt<bool> EJitForceInlineHintedHelpers(
    "ejit-force-inline-hinted-helpers", cl::init(false), cl::Hidden,
    cl::desc("Promote eligible local inlinehint helpers to alwaysinline only "
             "in extracted EJIT bitcode (requires an NDEBUG compiler build)"));
