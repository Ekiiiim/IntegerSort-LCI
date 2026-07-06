#pragma once

#include "is_config.hpp"

#include <lci.hpp>

#include <atomic>
#include <cstddef>
#include <vector>

namespace is_lci {

struct RedistributorOptions {
  bool use_upacket;
  bool use_loopback;
  int message_batch_size;
};

struct RedistributorRuntime {
  lci::comp_t handler = nullptr;
  lci::rcomp_t rcomp = nullptr;
  lci::comp_t send_counter = nullptr;
  std::atomic<Count>* frequency_histogram = nullptr;
  std::atomic<size_t> received_count{0};
  void* fallback_buffer = nullptr;
  size_t fallback_buffer_bytes = 0;
};

// Paper Step 4 setup: initialize the LCI active-message handler and
// redistribution runtime shared across rank() iterations.
void initialize_redistributor(RedistributorRuntime* runtime,
                              const RedistributorOptions& options);

// Paper Step 4 teardown: release the LCI handler state and any fallback
// message-buffer storage owned by the redistributor runtime.
void finalize_redistributor(RedistributorRuntime* runtime);

// Paper Step 4 setup: allocate per-thread/per-destination fallback buffers
// used when active-message transfers do not use LCI upackets.
void allocate_fallback_buffers(RedistributorRuntime* runtime,
                               int comm_size,
                               int max_threads,
                               const RedistributorOptions& options);

// Paper Step 4 per-iteration reset: attach the shifted local histogram and
// clear counters before redistributing the current iteration's keys.
void reset_redistributor_iteration(RedistributorRuntime* runtime,
                                   std::atomic<Count>* frequency_histogram);

// Paper Step 4: redistribute one rank's keys with LCI active messages,
// tallying received keys into the provided local frequency histogram.
void redistribute_keys(const Count* keys,
                       int local_key_count,
                       const int* bucket_to_rank,
                       int bucket_shift,
                       Count expected_recv_count,
                       int comm_size,
                       int my_rank,
                       const std::vector<lci::device_t>& devices,
                       const RedistributorOptions& options,
                       RedistributorRuntime* runtime);

} // namespace is_lci
