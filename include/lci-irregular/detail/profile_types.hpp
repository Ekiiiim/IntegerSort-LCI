#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lci_irregular {

struct ProfilingOptions {
  bool enabled = false;
  size_t worker_count = 1;
  std::string output_directory;
  std::string file_prefix = "lci-irregular-profile";
};

struct AmOperationProfile {
  uint64_t calls = 0;
  uint64_t records = 0;
  uint64_t payload_bytes = 0;
  uint64_t elapsed_nanoseconds = 0;
};

struct AmWorkerProfile {
  AmOperationProfile flush;
  AmOperationProfile progress;
  AmOperationProfile remote_receive;
  AmOperationProfile loopback_receive;
};

struct AmExchangeProfile {
  bool enabled = false;
  uint64_t exchange_sequence = 0;
  std::string name;
  AmWorkerProfile aggregate;
  std::vector<AmWorkerProfile> workers;
};

} // namespace lci_irregular
