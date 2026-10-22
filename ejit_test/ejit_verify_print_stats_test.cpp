//===-- ejit_verify_print_stats_test.cpp - verifier report regression
//------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
/* Standalone test of the real verifier with SRE logging/delay adapters:
     clang++ -std=c++17 -Illvm/include -DEJIT_VERIFY_SUBSTITUTION \
       -DEJIT_DIAG_ENABLE -DEJIT_SRE_DIAG -DEJIT_FREESTANDING \
       llvm/lib/ExecutionEngine/EJIT/EJitVerify.cpp \
       ejit_test/ejit_verify_print_stats_test.cpp -o /tmp/ejit_verify_print_test
     /tmp/ejit_verify_print_test
   Also run without EJIT_DIAG_ENABLE to test the compiled-out path, and with
   EJIT_DIAG_PRINT_THROTTLE_TICKS=0 to test disabled throttling.
 */
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitVerify.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace llvm::ejit;

int gEJitDiagLevel = EJIT_LOG_LVL_INFO;
static std::string output;
static unsigned lines, delays;

static void require(bool ok, const char *message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n%s", message, output.c_str());
    std::exit(1);
  }
}

extern "C" int SRE_printf(const char *fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  int n = std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  require(n > 0 && static_cast<size_t>(n) < sizeof(line), "bounded SRE line");
  output += line;
  ++lines;
  return n;
}

extern "C" uint32_t SRE_TaskDelay(uint32_t ticks) {
  require(ticks == EJIT_DIAG_PRINT_THROTTLE_TICKS, "configured delay");
  ++delays;
  return 0;
}

static void report() {
  output.clear();
  lines = delays = 0;
  VerifyStats before, after;
  ejitVerifyGetStats(&before);
  ejitVerifyPrintStats();
  ejitVerifyGetStats(&after);
  require(before.sites == after.sites && before.checks == after.checks &&
              before.mismatches == after.mismatches,
          "printing preserves counters");
#if defined(EJIT_DIAG_ENABLE) && EJIT_DIAG_PRINT_THROTTLE_TICKS > 0
  require(delays == lines, "every printed line throttled");
#else
  require(delays == 0, "disabled diagnostics/throttle never delay");
#endif
}

int main() {
  ejitVerifyResetStats();
  report();
#ifdef EJIT_DIAG_ENABLE
  require(output.find("sites=0 checks=0 mismatches=0 records=0/256") !=
                  std::string::npos &&
              output.find("no verification coverage") != std::string::npos,
          "empty run explicitly reports missing coverage");
#else
  require(output.empty(), "diagnostics compiled out");
#endif

  ejitVerifyNoteSite();
  report();
#ifdef EJIT_DIAG_ENABLE
  require(output.find("sites=1 checks=0 mismatches=0") != std::string::npos &&
              output.find("no verification coverage") != std::string::npos,
          "instrumented but unexecuted code is not a verified run");
#endif
  ejitVerifyResetStats();

  // Silence hot-path mismatch messages: the final report must still show
  // everything, including mismatches beyond the first-32 logging cap.
  gEJitDiagLevel = EJIT_LOG_LVL_OFF;
  ejitVerifyNoteSite();
  ejitVerifyNoteSite();
  __ejit_verify_check("probe:cfg+0", 4, 4);
  for (unsigned i = 0; i < 40; ++i)
    __ejit_verify_check("probe:cfg+4", 0x123456789abcdef0ULL,
                        0xfedcba9876543210ULL);
  report();
  require(output.empty() && lines == 0 && delays == 0,
          "log OFF suppresses output and delays");
  gEJitDiagLevel = EJIT_LOG_LVL_INFO;
  report();
#ifdef EJIT_DIAG_ENABLE
  require(output.find("sites=2 checks=41 mismatches=40 records=2/256") !=
              std::string::npos,
          "accurate totals");
  require(output.find("site=probe:cfg+0 checks=1 mismatches=0") !=
              std::string::npos,
          "stable field reported");
  require(output.find("site=probe:cfg+4 checks=40 mismatches=40") !=
              std::string::npos,
          "moving field reported beyond log cap");
  require(output.find(
              "lastFrozen=0x123456789abcdef0 lastActual=0xfedcba9876543210") !=
              std::string::npos,
          "full 64-bit mismatch evidence");
  require(lines == 4, "summary, two site lines and one evidence line");
#else
  require(output.empty(), "populated report compiled out");
#endif
  VerifySite site;
  require(ejitVerifyGetSite(1, &site) && site.mismatches == 40 &&
              site.lastActual == 0xfedcba9876543210ULL,
          "printing preserves site evidence");

  // Exceed fixed capacity, including a truncated name at its maximum length.
  gEJitDiagLevel = EJIT_LOG_LVL_OFF;
  ejitVerifyResetStats();
  char name[96];
  std::memset(name, 'x', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  __ejit_verify_check(name, 1, 1);
  for (unsigned i = 1; i < 258; ++i) {
    std::snprintf(name, sizeof(name), "probe:cfg+%u", i);
    __ejit_verify_check(name, 1, 1);
  }
  // Records beyond the old 64-row limit retain individual evidence.
  VerifySite extra;
  require(ejitVerifySiteCount() == 256 && ejitVerifyGetSite(255, &extra) &&
              std::strcmp(extra.site, "probe:cfg+255") == 0,
          "all 256 records available");
  __ejit_verify_check("probe:cfg+255", 1, 2);
  require(ejitVerifyGetSite(255, &extra) && extra.checks == 2 &&
              extra.mismatches == 1 && extra.lastActual == 2,
          "record 255 keeps updating after table fills");
  gEJitDiagLevel = EJIT_LOG_LVL_INFO;
  report();
#ifdef EJIT_DIAG_ENABLE
  require(output.find("checks=259 mismatches=1 records=256/256") !=
              std::string::npos,
          "overflow still counted in totals");
  require(output.find("site table full; additional sites may be totals-only") !=
              std::string::npos,
          "table capacity limitation reported");
  require(lines == 259, "bounded report at table capacity");
#endif
  ejitVerifyResetStats();
  report();
#ifdef EJIT_DIAG_ENABLE
  require(output.find("site=probe:") == std::string::npos,
          "reset removes previous site records");
#endif
  std::puts("PASS: verifier diagnostic report");
}
