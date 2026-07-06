#pragma once

#include "is_config.hpp"

namespace is_lci {

// Paper Steps 3-4 boundary: rank-local result of greedy bucket ownership
// planning consumed by the redistribution stage.
struct BucketPlan {
  Count expected_recv_count;
  Count min_key_value;
  Count max_key_value;
  Rank lesser_key_count;
  Rank local_key_count;
};

// Paper Step 2: count one rank's keys into local buckets before the
// cross-rank reduction in is.cpp.
void count_local_buckets(const Count* keys,
                         int local_key_count,
                         int bucket_shift,
                         Count* local_bucket_counts,
                         int num_buckets);

// Paper Step 3: greedily map global buckets to ranks and compute the local
// bucket interval metadata needed before Step 4 redistribution.
BucketPlan build_bucket_plan(const Count* local_bucket_counts,
                             const Count* global_bucket_counts,
                             int* bucket_to_rank,
                             Count* first_bucket_by_rank,
                             Count* last_bucket_by_rank,
                             int comm_size,
                             int my_rank,
                             int num_buckets,
                             int bucket_shift,
                             int average_keys_per_rank);

} // namespace is_lci
