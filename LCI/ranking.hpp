#pragma once

#include "is_config.hpp"

#include <atomic>

namespace is_lci {

// Paper Step 5: prefix-sum one rank's key-frequency histogram into cumulative
// key ranks for the local verification and full-verify stages.
void compute_local_ranks(const std::atomic<Count>* frequency_histogram,
                         Rank* cumulative_ranks,
                         Count min_key_value,
                         Count max_key_value);

} // namespace is_lci
