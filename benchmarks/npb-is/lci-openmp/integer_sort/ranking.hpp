#pragma once

#include "integer_sort/types.hpp"

#include <atomic>

namespace is_lci {

// Paper Step 5: prefix-sum one rank's key-frequency histogram into cumulative
// key ranks for the local verification and full-verify stages.
void compute_local_ranks(const std::atomic<KeyCount>* frequency_histogram, KeyRank* cumulative_ranks,
                         KeyValue min_key_value, KeyValue max_key_value);

} // namespace is_lci
