#include "ranking.hpp"

#include <omp.h>

#include <vector>

namespace is_lci {

void compute_local_ranks(const std::atomic<KeyCount>* frequency_histogram, KeyRank* cumulative_ranks,
                         KeyValue min_key_value, KeyValue max_key_value) {
  const KeyValue key_value_range = max_key_value - min_key_value + 1;
  std::vector<KeyRank> partial_sums;

  #pragma omp parallel
  {
    const int nthreads = omp_get_num_threads();
    const int tid = omp_get_thread_num();

    #pragma omp single
    {
      partial_sums.resize(nthreads);
    }

    KeyRank local_sum = 0;
    #pragma omp for schedule(static) nowait
    for (KeyValue i = 0; i < key_value_range; i++) {
      local_sum += frequency_histogram[min_key_value + i].load(std::memory_order_relaxed);
    }
    partial_sums[tid] = local_sum;

    #pragma omp barrier

    #pragma omp single
    {
      KeyRank temp_sum = 0;
      for (int i = 0; i < nthreads; i++) {
        KeyRank value = partial_sums[i];
        partial_sums[i] = temp_sum;
        temp_sum += value;
      }
    }

    KeyRank offset = partial_sums[tid];
    #pragma omp for schedule(static)
    for (KeyValue i = 0; i < key_value_range; i++) {
      offset += frequency_histogram[min_key_value + i].load(std::memory_order_relaxed);
      cumulative_ranks[min_key_value + i] = offset;
    }
  }
}

} // namespace is_lci
