#ifndef EJIT_PR231_SRE_FIXTURE_H
#define EJIT_PR231_SRE_FIXTURE_H
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#ifndef EJIT_PR231_SHARED
#define EJIT_PR231_SHARED __attribute__((section(".mc_shared"), aligned(64)))
#endif
#define PR231_CELLS 6u
#define PR231_TRPS 2u
#define PR231_WORKER 6u
#define PR231_PRODUCER 16u
typedef struct {
  ejit_may_const uint32_t mode;
  ejit_may_const uint32_t cellGain;
  ejit_may_const uint32_t trpBias;
  ejit_may_const uint32_t jointBias;
  uint32_t liveBias;
} PR231Config;
extern PR231Config g_pr231_config[PR231_CELLS][PR231_TRPS];
extern int64_t g_pr231_output[PR231_CELLS][PR231_TRPS];
int64_t pr231_smalltable_entry(uint8_t cell, uint8_t trp, int64_t x);
/* Defined in the separate controller TU, so the actual entry bitcode retains
 * an external observer call, not a JIT-cloned controller/private-state graph. */
void pr231_probe_inflight(void);
#endif
