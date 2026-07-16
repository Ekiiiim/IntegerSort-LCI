/*************************************************************************
 * NAS Parallel Benchmarks IS input generation.
 *
 * These routines preserve the baseline NPB randlc/find_my_seed/create_seq
 * behavior while parameterizing the destination key buffer for the LCI driver.
 *************************************************************************/

#include "nas/key_generation.hpp"

namespace is_lci {

double randlc(double* seed, double* multiplier) {
  static int ks = 0;
  static double r23, r46, t23, t46;
  double t1, t2, t3, t4;
  double a1;
  double a2;
  double x1;
  double x2;
  double z;
  int i, j;

  if (ks == 0) {
    r23 = 1.0;
    r46 = 1.0;
    t23 = 1.0;
    t46 = 1.0;

    for (i = 1; i <= 23; i++) {
      r23 = 0.50 * r23;
      t23 = 2.0 * t23;
    }
    for (i = 1; i <= 46; i++) {
      r46 = 0.50 * r46;
      t46 = 2.0 * t46;
    }
    ks = 1;
  }

  t1 = r23 * *multiplier;
  j = t1;
  a1 = j;
  a2 = *multiplier - t23 * a1;

  t1 = r23 * *seed;
  j = t1;
  x1 = j;
  x2 = *seed - t23 * x1;
  t1 = a1 * x2 + a2 * x1;

  j = r23 * t1;
  t2 = j;
  z = t1 - t23 * t2;
  t3 = t23 * z + a2 * x2;
  j = r46 * t3;
  t4 = j;
  *seed = t3 - t46 * t4;
  return r46 * *seed;
}

double find_my_seed(int rank, int comm_size, long total_random_numbers, double seed, double multiplier) {
  long i;
  double t1, t2, t3, an;
  long mq, nq, kk, ik;

  nq = total_random_numbers / comm_size;

  for (mq = 0; nq > 1; mq++, nq /= 2) {
  }

  t1 = multiplier;

  for (i = 1; i <= mq; i++) {
    t2 = randlc(&t1, &t1);
  }

  an = t1;
  kk = rank;
  t1 = seed;
  t2 = an;

  for (i = 1; i <= 100; i++) {
    ik = kk / 2;
    if (2 * ik != kk) {
      t3 = randlc(&t1, &t2);
    }
    if (ik == 0) {
      break;
    }
    t3 = randlc(&t2, &t2);
    kk = ik;
  }

  return t1;
}

void generate_keys(KeyValue* keys, KeyCount local_key_count, KeyRank max_key, double seed, double multiplier) {
  const int key_scale = static_cast<int>(max_key / 4);

  for (KeyCount i = 0; i < local_key_count; i++) {
    double x = randlc(&seed, &multiplier);
    x += randlc(&seed, &multiplier);
    x += randlc(&seed, &multiplier);
    x += randlc(&seed, &multiplier);

    keys[i] = static_cast<KeyValue>(key_scale * x);
  }
}

} // namespace is_lci
