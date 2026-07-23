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

lci_irregular::AmExchangeProfile redistribute_keys_with_active_messages(lci_irregular::IrregularRuntime& runtime,
                                                                        const KeyValue* keys, KeyCount local_key_count,
                                                                        const int* bucket_to_rank, int bucket_shift,
                                                                        KeyCount expected_recv_count,
                                                                        std::atomic<KeyCount>* frequency_histogram,
                                                                        lci_irregular::AmExchangeOptions options) {
  auto route_key = [&](KeyValue key) { return bucket_to_rank[key >> bucket_shift]; };

  std::atomic<size_t> received_count{0};
  auto am_handler = [&](lci_irregular::RecordBatchView<KeyValue> received_keys, int source_rank) {
    (void)source_rank;
    for (size_t i = 0; i < received_keys.size(); ++i) {
      frequency_histogram[received_keys[i]].fetch_add(1, std::memory_order_relaxed);
    }
    received_count.fetch_add(received_keys.size(), std::memory_order_relaxed);
  };
  auto is_done = [&]() {
    return received_count.load(std::memory_order_relaxed) >= static_cast<size_t>(expected_recv_count);
  };

  auto send_phase = [keys, local_key_count, route_key](auto run_worker) noexcept {
#pragma omp parallel
    {
      const size_t worker_index = current_worker_index();
      run_worker(worker_index, [=](auto am_send) noexcept {
#pragma omp for nowait
        for (KeyCount i = 0; i < local_key_count; ++i) {
          const KeyValue key = keys[i];
          am_send(route_key(key), key);
        }
      });
    }
  };

  return runtime.am_exchange_until<KeyValue>(am_handler, is_done, send_phase, options);
}

} // namespace is_lci
