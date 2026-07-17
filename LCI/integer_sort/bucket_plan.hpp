#pragma once

#include "types.hpp"

namespace is_lci {

// Paper Steps 3-4 boundary: rank-local result of greedy bucket ownership
// planning consumed by the redistribution stage.
struct BucketPlan {
  KeyCount expected_recv_count;
  KeyValue min_key_value;
  KeyValue max_key_value;
  KeyRank lesser_key_count;
  KeyCount local_key_count;
};

// Paper Step 2: count one rank's keys into local buckets before the
// cross-rank reduction in the iteration driver.
void count_local_buckets(const KeyValue* keys, KeyCount local_key_count, int bucket_shift,
                         KeyCount* local_bucket_counts, int num_buckets);

// Paper Step 3: greedily map global buckets to ranks and compute the local
// bucket interval metadata needed before Step 4 redistribution.
BucketPlan build_bucket_plan(const KeyCount* local_bucket_counts, const KeyCount* global_bucket_counts,
                             int* bucket_to_rank, int* first_bucket_by_rank, int* last_bucket_by_rank, int comm_size,
                             int my_rank, int num_buckets, int bucket_shift, KeyCount average_keys_per_rank);

} // namespace is_lci
