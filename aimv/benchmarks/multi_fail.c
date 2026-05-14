// AIMV benchmark: mixed failures (alias + cost)
// Expected: multiple loops fail vectorization for different reasons
void multi_loop(int *a, int *b, short *c, int n) {
  // Loop 1: alias failure (a/b may alias)
  for (int i = 0; i < n; i++) {
    a[i] = b[i] + b[i + 1];
  }
  // Loop 2: cost model may reject (conditional + small type)
  for (int i = 0; i < n; i++) {
    if (c[i] > 0) {
      c[i] = c[i] * 2;
    }
  }
}
