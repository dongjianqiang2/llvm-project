// AIMV benchmark: cost model rejection (VectorizationNotBeneficial)
// Expected: loop rejected due to high vector cost relative to small trip count
int compute_sum(short *data, int n) {
  int sum = 0;
  for (int i = 0; i < n; i++) {
    if (data[i] > 0) {
      sum += data[i];
    }
  }
  return sum;
}
