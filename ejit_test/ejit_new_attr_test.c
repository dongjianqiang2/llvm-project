/**
 * EmbeddedJIT New Attribute Macro Test
 *
 * Verifies the new attribute macros compile and JIT correctly. Uses the new
 * naming convention exclusively (no legacy compat aliases):
 *
 *   EJIT_PERIOD_CONST       (attribute ejit_period_const)
 *   ejit_group(x)           (merged period / period_arr)
 *   EJIT_DIM(x)             (attribute ejit_dim)
 *   EJIT_ENTRY              (attribute ejit_entry)
 *   ejit_guard(x)           (attribute ejit_guard)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- Structs using EJIT_PERIOD_CONST -----------------------------------===//

struct BoardCfg {
    EJIT_PERIOD_CONST uint32_t boardType;
    uint32_t _pad;
};

struct CellCfg {
    EJIT_PERIOD_CONST uint32_t cellType;
    uint32_t traffic;
};

//===-- Globals using ejit_group ---------------===//

ejit_group(static)  struct BoardCfg g_board;
ejit_group(cell) struct CellCfg g_cells[8];

//===-- JIT functions using EJIT_ENTRY / EJIT_DIM / ejit_guard -----===//

EJIT_ENTRY uint32_t check_cell(EJIT_DIM(cell) uint8_t ci) {
    if (g_cells[ci].cellType == 42) return 100;
    if (g_cells[ci].cellType == 99) return 200;
    return 0;
}

ejit_guard(cell)
void update_cell(EJIT_DIM(cell) uint8_t ci) {
    g_cells[ci].cellType = 99;
}

//===-- Main --------------------------------------------------------------===//

int main(int argc, char **argv) {
    uint8_t ci = (uint8_t)(argc > 1 ? (uint8_t)(argv[1][0] - '0') : 0);
    int failures = 0;

    printf("=== EJIT New Attribute Test ===\n");
    printf("cellIdx=%u\n\n", ci);

    // Init
    ejit_config_t cfg;
    ejit_default_config(&cfg);
    int rc = (int)ejit_init(&cfg);
    if (rc != 0) { printf("FAIL: init\n"); return 1; }
    printf("--- init OK ---\n");

    // Test 1: JIT compile with EJIT_PERIOD_CONST
    printf("\n--- 1. EJIT_PERIOD_CONST in struct ---\n");
    g_cells[ci].cellType = 42;
    ejit_activate("cell", ci);
    ejit_drain_taskpool();
    uint32_t r = check_cell(ci);
    if (r != 100) { printf("  FAIL: check_cell=%u (expected 100)\n", r); failures++; }
    else printf("  OK: check_cell=%u\n", r);

    // Test 2: ejit_guard with EJIT_DIM
    printf("\n--- 2. ejit_guard + EJIT_DIM ---\n");
    update_cell(ci);
    ejit_drain_taskpool();
    r = check_cell(ci);
    if (r != 200) { printf("  FAIL: after guard, check_cell=%u (expected 200)\n", r); failures++; }
    else printf("  OK: after guard, check_cell=%u\n", r);

    // Test 3: ejit_group static always active
    printf("\n--- 3. ejit_group(static) ---\n");
    g_board.boardType = 7;
    // static period is always active — no explicit activate needed
    // This just verifies the attribute compiles and the global is accessible
    printf("  OK: boardCfg.boardType=%u\n", g_board.boardType);

    printf("\n=== Result: %d failures ===\n", failures);
    ejit_shutdown();
    return failures > 0 ? 1 : 0;
}
