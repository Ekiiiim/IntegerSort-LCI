#pragma once

#include <cstddef>

namespace lci_irregular {

// Runtime-level LCI resource tuning. The application decides how many LCI
// devices its workers should share; the irregular runtime does not create or
// query application threads.
struct IrregularRuntimeOptions {
  int device_count = 1;
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
