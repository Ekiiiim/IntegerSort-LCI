#include "integer_sort/key_redistribution.hpp"

#include <lci-irregular/irregular_runtime.hpp>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace is_lci {
namespace {

size_t current_worker_index() {
#ifdef _OPENMP
  return static_cast<size_t>(omp_get_thread_num());
#else
  return 0;
#endif
}

} // namespace

lci_irregular::AmProfile redistribute_keys_with_active_messages(lci_irregular::Runtime& runtime, const KeyValue* keys,
                                                                KeyCount local_key_count, const int* bucket_to_rank,
                                                                int bucket_shift, KeyCount expected_recv_count,
                                                                std::atomic<KeyCount>* frequency_histogram,
                                                                lci_irregular::AmOptions options) {
  auto route_key = [&](KeyValue key) { return bucket_to_rank[key >> bucket_shift]; };

  auto am_handler = [&](KeyValue key) { frequency_histogram[key].fetch_add(1, std::memory_order_relaxed); };

  runtime.set_am_handler<KeyValue>(am_handler, options);

  // clang-format off: preserve C++-scope indentation for OpenMP pragmas.
  #pragma omp parallel
  {
    lci_irregular::WorkerHandler worker = runtime.get_worker_handler(current_worker_index());

    #pragma omp for nowait
    for (KeyCount i = 0; i < local_key_count; ++i) {
      const KeyValue key = keys[i];
      worker.post_am(route_key(key), key);
    }

    worker.flush();

    while (runtime.recv_count() < static_cast<size_t>(expected_recv_count)) {
      worker.progress();
    }
  }
  // clang-format on

  lci_irregular::AmProfile profile = runtime.am_profile();
  runtime.clear_am_handler();
  return profile;
}

} // namespace is_lci
