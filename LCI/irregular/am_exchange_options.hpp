#pragma once

#include <cstddef>

namespace lci_irregular {

// Runtime-level LCI resource tuning. Zero send/receive limits use the default
// packet-pool heuristic from the original IS LCI implementation.
struct IrregularRuntimeOptions {
  int threads_per_device = 1;
  size_t max_sends_per_device = 0;
  size_t max_recvs_per_device = 0;
};

// Per-exchange active-message aggregation settings.
struct AmExchangeOptions {
  bool use_upacket = true;
  bool use_loopback = true;
  size_t batch_records = 0;
};

} // namespace lci_irregular
