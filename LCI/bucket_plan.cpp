#include "bucket_plan.hpp"

#include <omp.h>

#include <vector>

namespace is_lci {

void count_local_buckets(const Count* keys,
                         int local_key_count,
                         int bucket_shift,
                         Count* local_bucket_counts,
                         int num_buckets) {
#pragma omp parallel
  {
    std::vector<Count> bucket_size_private(num_buckets, 0);

#pragma omp for nowait
    for (int i = 0; i < local_key_count; i++) {
      bucket_size_private[keys[i] >> bucket_shift]++;
    }

#pragma omp critical
    {
      for (int bucket = 0; bucket < num_buckets; ++bucket) {
        local_bucket_counts[bucket] += bucket_size_private[bucket];
      }
    }
  }
}

BucketPlan build_bucket_plan(const Count* local_bucket_counts,
                             const Count* global_bucket_counts,
                             int* bucket_to_rank,
                             Count* first_bucket_by_rank,
                             Count* last_bucket_by_rank,
                             int comm_size,
                             int my_rank,
                             int num_buckets,
                             int bucket_shift,
                             int average_keys_per_rank) {
  Rank bucket_sum_accumulator = 0;
  Count expected_recv_count = 0;
  Count previous_bucket_sum_accumulator = 0;
  int owner_rank = 0;

  (void)local_bucket_counts;
  first_bucket_by_rank[0] = 0;

  for (int bucket = 0; bucket < num_buckets; bucket++) {
    bucket_sum_accumulator += global_bucket_counts[bucket];
    bucket_to_rank[bucket] = owner_rank;

    if (bucket_sum_accumulator >=
        static_cast<Rank>(owner_rank + 1) * average_keys_per_rank) {
      if (owner_rank == my_rank) {
        expected_recv_count = static_cast<Count>(
            bucket_sum_accumulator - previous_bucket_sum_accumulator);
      }
      if (owner_rank != 0) {
        first_bucket_by_rank[owner_rank] =
            last_bucket_by_rank[owner_rank - 1] + 1;
      }
      last_bucket_by_rank[owner_rank++] = bucket;
      previous_bucket_sum_accumulator =
          static_cast<Count>(bucket_sum_accumulator);
    }
  }

  while (owner_rank < comm_size) {
    first_bucket_by_rank[owner_rank] = 1;
    owner_rank++;
  }

  const Count min_key_value = first_bucket_by_rank[my_rank] << bucket_shift;
  const Count max_key_value =
      ((last_bucket_by_rank[my_rank] + 1) << bucket_shift) - 1;

  Rank lesser_key_count = 0;
  for (int rank = 0; rank < my_rank; rank++) {
    for (int bucket = first_bucket_by_rank[rank];
         bucket <= last_bucket_by_rank[rank];
         bucket++) {
      lesser_key_count += global_bucket_counts[bucket];
    }
  }

  Rank local_key_count = 0;
  for (int bucket = first_bucket_by_rank[my_rank];
       bucket <= last_bucket_by_rank[my_rank];
       bucket++) {
    local_key_count += global_bucket_counts[bucket];
  }

  return BucketPlan{
      expected_recv_count,
      min_key_value,
      max_key_value,
      lesser_key_count,
      local_key_count,
  };
}

} // namespace is_lci
