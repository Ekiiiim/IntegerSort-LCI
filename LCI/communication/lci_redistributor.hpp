#pragma once

#include "types.hpp"

#include <lci.hpp>

#include <atomic>
#include <cstddef>
#include <vector>

namespace is_lci {

// Paper Step 4 interface: runtime options for the LCI active-message redistributor.
struct RedistributorOptions {
  bool use_upacket;
  bool use_loopback;
  int message_batch_size;
};

// Paper Step 4 interface: runtime state for the LCI active-message redistributor.
struct RedistributorRuntime {
  lci::comp_t handler = nullptr;
  lci::rcomp_t rcomp = 0;
  lci::comp_t send_counter = nullptr;
  std::atomic<KeyCount>* frequency_histogram = nullptr;
  std::atomic<size_t> received_count{0};
  void* fallback_buffer = nullptr;
  size_t fallback_buffer_bytes = 0;
};

// Paper Step 4 setup: initialize the LCI active-message handler and
// redistribution runtime shared across sort iterations.
void initialize_redistributor(RedistributorRuntime* runtime, const RedistributorOptions& options);

// Paper Step 4 teardown: release the LCI handler state and any fallback
// message-buffer storage owned by the redistributor runtime.
void finalize_redistributor(RedistributorRuntime* runtime);

// Paper Step 4 setup: allocate per-thread/per-destination fallback buffers
// used when active-message transfers do not use LCI upackets.
void allocate_fallback_buffers(RedistributorRuntime* runtime, int comm_size, int max_threads,
                               const RedistributorOptions& options);

// Paper Step 4 per-iteration reset: attach the shifted local histogram and
// clear counters before redistributing the current iteration's keys.
void reset_redistributor_iteration(RedistributorRuntime* runtime, std::atomic<KeyCount>* frequency_histogram);

// Paper Step 4: redistribute one rank's keys with LCI active messages,
// tallying received keys into the provided local frequency histogram.
void redistribute_keys(const KeyValue* keys, KeyCount local_key_count, const int* bucket_to_rank, int bucket_shift,
                       KeyCount expected_recv_count, int comm_size, int my_rank,
                       const std::vector<lci::device_t>& devices, const RedistributorOptions& options,
                       RedistributorRuntime* runtime);

} // namespace is_lci
