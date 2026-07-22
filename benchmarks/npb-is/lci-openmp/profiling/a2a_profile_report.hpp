#pragma once

#include "integer_sort/types.hpp"

#include <lci-irregular/irregular_runtime.hpp>

namespace is_lci {

void print_a2a_profile_report(const lci_irregular::AmExchangeProfile& profile, int iteration, int rank,
                              KeyCount local_key_count, double rank_time);

} // namespace is_lci
