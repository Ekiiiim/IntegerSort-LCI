#pragma once

#include "irregular/am_exchange_options.hpp"
#include "types.hpp"

#include <atomic>

namespace lci_irregular {
class IrregularRuntime;
}

namespace is_lci {

// Paper Step 4: express IS key redistribution using the reusable LCI
// irregular active-message interface.
void redistribute_keys_with_active_messages(lci_irregular::IrregularRuntime& runtime, const KeyValue* keys,
                                            KeyCount local_key_count, const int* bucket_to_rank, int bucket_shift,
                                            KeyCount expected_recv_count,
                                            std::atomic<KeyCount>* frequency_histogram,
                                            lci_irregular::AmExchangeOptions options);

} // namespace is_lci
