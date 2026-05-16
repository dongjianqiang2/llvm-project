// AIMV benchmark: alias analysis failure (CantReorderMemOps)
// Expected: loop not vectorized due to possible aliasing between a[] and b[]
void process_task(int *a, int *b, int n) {
  for (int i = 0; i < n; i++) {
    a[i] = b[i] + b[i + 1];
  }
}
