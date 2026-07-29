#pragma once

#include <lci-irregular/irregular_runtime.hpp>
#include "integer_sort/types.hpp"

#include <atomic>

namespace lci_irregular {
class Runtime;
}

namespace is_lci {

// Paper Step 4: express IS key redistribution using the reusable LCI
// irregular active-message interface.
lci_irregular::AmProfile redistribute_keys_with_active_messages(lci_irregular::Runtime& runtime, const KeyValue* keys,
                                                                KeyCount local_key_count, const int* bucket_to_rank,
                                                                int bucket_shift, KeyCount expected_recv_count,
                                                                std::atomic<KeyCount>* frequency_histogram,
                                                                lci_irregular::AmOptions options);

} // namespace is_lci
