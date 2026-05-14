// AIMV benchmark: cross-iteration RAW dependency
// Expected: loop not vectorized due to backward dependency between iterations
void filter_samples(int *a, int *b, int n) {
  for (int i = 1; i < n; i++) {
    a[i] = a[i - 1] + b[i];
  }
}
