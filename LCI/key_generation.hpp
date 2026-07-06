#pragma once

#include "is_config.hpp"

namespace is_lci {

// Paper Step 1: portable NAS random number generator used for key creation.
double randlc(double* seed, double* multiplier);

// Paper Step 1: compute the starting seed for a rank's subsequence.
double find_my_seed(int rank,
                    int comm_size,
                    long total_random_numbers,
                    double seed,
                    double multiplier);

// Paper Step 1: generate Gaussian-like integer keys for one rank.
void generate_keys(Count* keys,
                   int local_key_count,
                   Rank max_key,
                   double seed,
                   double multiplier);

} // namespace is_lci
