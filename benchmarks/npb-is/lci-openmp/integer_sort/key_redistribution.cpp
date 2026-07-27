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

lci_irregular::AmProfile redistribute_keys_with_active_messages(lci_irregular::IrregularRuntime& runtime,
                                                                const KeyValue* keys, KeyCount local_key_count,
                                                                const int* bucket_to_rank, int bucket_shift,
                                                                KeyCount expected_recv_count,
                                                                std::atomic<KeyCount>* frequency_histogram,
                                                                lci_irregular::AmOptions options) {
  auto route_key = [&](KeyValue key) { return bucket_to_rank[key >> bucket_shift]; };

  auto am_handler = [&](lci_irregular::AmRecords<KeyValue> received_keys) {
    for (size_t i = 0; i < received_keys.size(); ++i) {
      frequency_histogram[received_keys[i]].fetch_add(1, std::memory_order_relaxed);
    }
  };

  runtime.set_am_handler<KeyValue>(am_handler, options);

  // clang-format off: preserve C++-scope indentation for OpenMP pragmas.
  #pragma omp parallel
  {
    const size_t worker_index = current_worker_index();

    #pragma omp for nowait
    for (KeyCount i = 0; i < local_key_count; ++i) {
      const KeyValue key = keys[i];
      runtime.post_am(worker_index, route_key(key), key);
    }

    runtime.flush_remaining_buffers(worker_index);

    while (runtime.received_record_count() < static_cast<size_t>(expected_recv_count)) {
      runtime.progress(worker_index);
    }

    runtime.quiet(worker_index);
  }
  // clang-format on

  lci_irregular::AmProfile profile = runtime.am_profile();
  runtime.clear_am_handler();
  return profile;
}

} // namespace is_lci
