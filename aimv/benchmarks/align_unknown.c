// AIMV benchmark: unknown alignment
// Expected: loop not vectorized due to unknown pointer alignment
void copy_buffer(char *src, char *dst, int n) {
  for (int i = 0; i < n; i++) {
    dst[i] = src[i];
  }
}
