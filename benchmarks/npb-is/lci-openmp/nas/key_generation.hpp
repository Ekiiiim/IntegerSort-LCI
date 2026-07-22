#pragma once

#include "integer_sort/types.hpp"

namespace is_lci {

// NAS benchmark input generation: portable random number generator used by the
// original NPB IS benchmark.
double randlc(double* seed, double* multiplier);

// NAS benchmark input generation: compute the starting seed for a rank's
// original NPB random-number subsequence.
double find_my_seed(int rank, int comm_size, long total_random_numbers, double seed, double multiplier);

// NAS benchmark input generation: generate the Gaussian-like integer keys used
// by the original NPB IS benchmark.
void generate_keys(KeyValue* keys, KeyCount local_key_count, KeyRank max_key, double seed, double multiplier);

} // namespace is_lci
