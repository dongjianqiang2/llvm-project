/*
 * AIMV TSVC-style Vectorization Test Suite
 *
 * Based on classic TSVC (Test Suite for Vectorizing Compilers) loop patterns.
 * Each function targets a specific vectorization failure mode that AIMV
 * should be able to diagnose and potentially fix.
 *
 * Categories:
 *   alias_*   — alias analysis failures (need restrict)
 *   stride_*  — non-unit stride or unknown stride
 *   cost_*    — cost model rejections
 *   reduc_*   — reduction patterns
 *   ctrl_*    — control flow (if/switch in loop body)
 *   slp_*     — SLP vectorizer targets
 *   unroll_*  — loop unrolling targets
 */

#include <stddef.h>

/* ================================================================
 * Alias Analysis Failures
 * ================================================================ */

/* alias_dep1: read-after-write through possibly aliasing pointers.
 * Expected: UnsafeDep or CantReorderMemOps.
 * AI fix: add restrict to a and b. */
void alias_dep1(int *a, int *b, int n) {
  for (int i = 0; i < n; i++) { /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */ /* AIMV: Add __builtin_assume(n >= 16) to hint trip count range */
    a[i] = b[i] + b[i + 1];
  }
}

/* alias_dep2: write-after-read in overlapping arrays.
 * Expected: UnsafeDep.
 * AI fix: add restrict, or loop fission. */
void alias_dep2(int *a, int *b, int n) {
  for (int i = 1; i < n; i++) {
    a[i] = a[i - 1] + b[i];
  }
}

/* alias_dep3: load from a[i], store to b[i]; a and b may alias.
 * Expected: CantReorderMemOps.
 * AI fix: add restrict to a and b. */
void alias_dep3(short *a, short *b, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = b[i] * 2 + 1;
  }
}

/* ================================================================
 * Non-unit Stride Access
 * ================================================================ */

/* stride_col: column-wise access in row-major layout.
 * Expected: stride != 1, may prevent vectorization.
 * AI fix: suggest loop interchange or data layout change. */
void stride_col(int a[][256], int b[][256], int n, int m) {
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < m; j++) {
      a[j][i] = b[j][i] + 1;
    }
  }
}

/* stride_indirect: indirect indexing with unpredictable stride.
 * Expected: non-constant stride, vectorization uncertain.
 * AI fix: suggest restructuring to direct indexing. */
void stride_indirect(int *a, int *b, int *idx, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = b[idx[i]] + 1;
  }
}

/* ================================================================
 * Cost Model Rejections
 * ================================================================ */

/* cost_small_trip: very short trip count may be rejected.
 * Expected: VectorizationNotBeneficial for small n.
 * AI fix: suggest loop unrolling or different pragma. */
void cost_small_trip(int *a, int *b, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = a[i] + b[i];
  }
}

/* cost_complex_body: complex loop body with many operations.
 * Expected: cost model may reject due to high scalar expansion cost.
 * AI fix: suggest simplification or split. */
void cost_complex_body(int *a, int *b, int *c, int *d, int n) {
  for (int i = 0; i < n; i++) {
    int t1 = a[i] * b[i];
    int t2 = c[i] * d[i];
    int t3 = t1 + t2;
    int t4 = t1 - t2;
    a[i] = t3 + t4;
    b[i] = t3 - t4;
    c[i] = t1 * t2;
    d[i] = t3 / (t4 + 1);
  }
}

/* ================================================================
 * Reduction Patterns
 * ================================================================ */

/* reduc_sum: simple summation reduction.
 * Expected: should vectorize with fast-math or -ffast-math. */
float reduc_sum(float *a, int n) {
  float s = 0.0f;
  for (int i = 0; i < n; i++) {
    s += a[i];
  }
  return s;
}

/* reduc_max: max reduction.
 * Expected: may need -ffast-math or explicit reduction order. */
int reduc_max(int *a, int n) {
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (a[i] > m) m = a[i];
  }
  return m;
}

/* reduc_dotprod: dot product with two arrays.
 * Expected: should vectorize with fast-math. */
float reduc_dotprod(float *a, float *b, int n) {
  float s = 0.0f;
  for (int i = 0; i < n; i++) {
    s += a[i] * b[i];
  }
  return s;
}

/* ================================================================
 * Control Flow in Loops
 * ================================================================ */

/* ctrl_conditional: if-then-else inside loop body.
 * Expected: may prevent vectorization or require predication. */
void ctrl_conditional(int *a, int *b, int *c, int n) {
  for (int i = 0; i < n; i++) {
    if (b[i] > 0) {
      a[i] = b[i] + c[i];
    } else {
      a[i] = b[i] - c[i];
    }
  }
}

/* ctrl_break: early exit from loop.
 * Expected: cannot vectorize due to early exit.
 * AI fix: suggest restructuring (not likely fixable). */
int ctrl_break(int *a, int n) {
  int found = -1;
  for (int i = 0; i < n; i++) {
    if (a[i] == 42) {
      found = i;
      break;
    }
  }
  return found;
}

/* ================================================================
 * SLP Vectorizer Targets
 * ================================================================ */

/* slp_simple: two independent scalar operations on adjacent elements.
 * Expected: SLP should combine a[0],a[1] and b[0],b[1] into 2-wide vectors. */
void slp_simple(int *a, int *b, int *c, int *d) {
  a[0] = b[0] + c[0];
  a[1] = b[1] + c[1];
  d[0] = a[0] * 2;
  d[1] = a[1] * 2;
}

/* slp_struct: struct field access pattern.
 * Expected: SLP may or may not vectorize depending on layout. */
typedef struct { int x, y, z, w; } Vec4;
void slp_struct(Vec4 *a, Vec4 *b, Vec4 *c, int n) {
  for (int i = 0; i < n; i++) {
    a[i].x = b[i].x + c[i].x;
    a[i].y = b[i].y + c[i].y;
  }
}

/* slp_mixed_types: mixed type sizes prevent SLP.
 * Expected: SLP UnsupportedType.
 * AI fix: suggest type normalization. */
void slp_mixed_types(int *a, short *b, int n) {
  for (int i = 0; i < n; i++) {
    a[2*i] = (int)b[2*i] + 1;
    a[2*i + 1] = (int)b[2*i + 1] + 1;
  }
}

/* ================================================================
 * Loop Unrolling Targets
 * ================================================================ */

/* unroll_unknown_trip: trip count unknown at compile time.
 * Expected: CantUnrollTripCount.
 * AI fix: __builtin_assume(n >= 16) or similar. */
void unroll_unknown_trip(int *a, int *b, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = a[i] + b[i];
  }
}

/* unroll_small_body: very small loop body.
 * Expected: may need unrolling to enable vectorization.
 * AI fix: suggest unroll_and_jam or manual unrolling. */
void unroll_small_body(int *a, int *b, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = b[i];
  }
}

/* unroll_nested: nested loops where inner loop is small.
 * Expected: inner loop may benefit from unrolling.
 * AI fix: suggest unrolling inner loop or interchange. */
void unroll_nested(int a[][64], int b[][64], int n) {
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < 4; j++) {
      a[i][j] = b[i][j] + 1;
    }
  }
}

/* ================================================================
 * Combined / Mixed Patterns
 * ================================================================ */

/* mixed_alias_slp: alias issue combined with SLP-able pattern.
 * Expected: both LV alias diagnostic and SLP NotBeneficial. */
void mixed_alias_slp(int *a, int *b, int *c, int *d, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = b[i] + c[i];
    d[i] = b[i] - c[i];
  }
}

/* multi_dim_fail: multiple reasons for vectorization failure.
 * Expected: multiple diagnostic types from different passes. */
void multi_dim_fail(int *a, int *b, int *c, int *idx, int n) {
  for (int i = 1; i < n - 1; i++) {
    int j = idx[i];
    a[i] = b[i - 1] + c[i + 1];
    b[i] = a[j] * 2;
    if (c[i] > 0) {
      c[i] = a[i] + b[i];
    }
  }
}
