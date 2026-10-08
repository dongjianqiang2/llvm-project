/* Actual annotated PR231 business TU. Keep separate from the controller and
 * do not LTO these two files together before EJIT bitcode extraction. */
#include "ejit_smalltable_sre_fixture.h"
ejit_period_arr(pr231_cell) PR231Config
    g_pr231_config[PR231_CELLS][PR231_TRPS] EJIT_PR231_SHARED;
int64_t g_pr231_output[PR231_CELLS][PR231_TRPS] EJIT_PR231_SHARED;

__attribute__((noinline)) int64_t
pr231_positive(int64_t x, uint32_t gain, uint32_t bias) {
  return x * (int64_t)gain + (int64_t)bias;
}
__attribute__((noinline)) int64_t
pr231_negative(int64_t x, uint32_t gain, uint32_t bias) {
  return x * (int64_t)gain - (int64_t)bias;
}

ejit_entry int64_t pr231_smalltable_entry(
    ejit_period_arr_ind(pr231_cell) uint8_t cell,
    ejit_period_arr_ind(pr231_trp) uint8_t trp, int64_t x) {
  pr231_probe_inflight();
  const PR231Config *cfg = &g_pr231_config[cell][trp];
  const uint32_t bias = cfg->trpBias + cfg->jointBias;
  int64_t result = x >= 0 ? pr231_positive(x, cfg->cellGain, bias)
                          : pr231_negative(x, cfg->cellGain, bias);
  /* Uniform, cell-only, TRP-only and joint fields, plus a genuine live load
   * and store. Dynamic sign branches keep the profile nontrivial. */
  if (cfg->mode == 1u)
    result += (int64_t)cfg->liveBias;
  else
    result -= (int64_t)cfg->liveBias;
  g_pr231_output[cell][trp] = result;
  return result;
}
