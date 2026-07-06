#include "ranking.hpp"

#include <omp.h>

#include <vector>

namespace is_lci {

void compute_local_ranks(const std::atomic<Count>* frequency_histogram,
                         Rank* cumulative_ranks,
                         Count min_key_value,
                         Count max_key_value) {
  const Count key_count = max_key_value - min_key_value + 1;
  std::vector<Rank> partial_sums;

#pragma omp parallel
  {
    const int nthreads = omp_get_num_threads();
    const int tid = omp_get_thread_num();

#pragma omp single
    { partial_sums.resize(nthreads); }

    Rank local_sum = 0;
#pragma omp for schedule(static) nowait
    for (Count i = 0; i < key_count; i++) {
      local_sum +=
          frequency_histogram[min_key_value + i].load(std::memory_order_relaxed);
    }
    partial_sums[tid] = local_sum;

#pragma omp barrier

#pragma omp single
    {
      Rank temp_sum = 0;
      for (int i = 0; i < nthreads; i++) {
        Rank value = partial_sums[i];
        partial_sums[i] = temp_sum;
        temp_sum += value;
      }
    }

    Rank offset = partial_sums[tid];
#pragma omp for schedule(static)
    for (Count i = 0; i < key_count; i++) {
      offset +=
          frequency_histogram[min_key_value + i].load(std::memory_order_relaxed);
      cumulative_ranks[min_key_value + i] = offset;
    }
  }
}

} // namespace is_lci
