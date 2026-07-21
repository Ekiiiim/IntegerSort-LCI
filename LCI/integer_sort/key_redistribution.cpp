#include "integer_sort/key_redistribution.hpp"

#include "irregular/irregular_runtime.hpp"

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

void redistribute_keys_with_active_messages(lci_irregular::IrregularRuntime& runtime, const KeyValue* keys,
                                            KeyCount local_key_count, const int* bucket_to_rank, int bucket_shift,
                                            KeyCount expected_recv_count, std::atomic<KeyCount>* frequency_histogram,
                                            lci_irregular::AmExchangeOptions options) {
  auto route_key = [&](KeyValue key) { return bucket_to_rank[key >> bucket_shift]; };

  std::atomic<size_t> received_count{0};
  auto receive_keys = [&](const KeyValue* received_keys, size_t count, int source_rank) {
    (void)source_rank;
    for (size_t i = 0; i < count; ++i) {
      frequency_histogram[received_keys[i]].fetch_add(1, std::memory_order_relaxed);
    }
    received_count.fetch_add(count, std::memory_order_relaxed);
  };
  auto is_done = [&]() {
    return received_count.load(std::memory_order_relaxed) >= static_cast<size_t>(expected_recv_count);
  };

  auto exchange = runtime.am_exchange_start<KeyValue>(receive_keys, is_done, options);

  #pragma omp parallel
  {
    size_t worker_index = current_worker_index();
    auto sender = exchange.make_sender(worker_index);

    #pragma omp for nowait
    for (KeyCount i = 0; i < local_key_count; ++i) {
      KeyValue key = keys[i];
      sender.am_send(route_key(key), key);
    }

    sender.flush();
    while (!exchange.is_done()) {
      exchange.progress(worker_index);
    }
  }

  exchange.wait();
}

} // namespace is_lci
