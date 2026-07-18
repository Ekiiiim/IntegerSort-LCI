#include "integer_sort/key_redistribution.hpp"

#include "irregular/irregular_runtime.hpp"

namespace is_lci {

void redistribute_keys_with_active_messages(lci_irregular::IrregularRuntime& runtime, const KeyValue* keys,
                                            KeyCount local_key_count, const int* bucket_to_rank, int bucket_shift,
                                            KeyCount expected_recv_count,
                                            std::atomic<KeyCount>* frequency_histogram,
                                            lci_irregular::AmExchangeOptions options) {
  auto route_key = [&](const KeyValue& key) {
    return bucket_to_rank[key >> bucket_shift];
  };

  auto receive_keys = [&](const KeyValue* received_keys, size_t count, int source_rank) {
    (void)source_rank;
    for (size_t i = 0; i < count; ++i) {
      frequency_histogram[received_keys[i]].fetch_add(1, std::memory_order_relaxed);
    }
  };

  runtime.am_exchange_counted<KeyValue>(keys, static_cast<size_t>(local_key_count), route_key, receive_keys,
                                        static_cast<size_t>(expected_recv_count), options);
}

} // namespace is_lci
