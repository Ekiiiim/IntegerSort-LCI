#pragma once

#include "benchmark/verification/verification.hpp"
#include "communication/lci_redistributor.hpp"
#include "types.hpp"

#include <lci.hpp>

#include <atomic>
#include <vector>

namespace is_lci {

struct SortIterationContext {
  int iteration;
  int my_rank;
  int comm_size;
  KeyCount local_key_count;
  int timeron;

  KeyValue* keys;
  std::atomic<KeyCount>* frequency_storage;
  KeyRank* cumulative_storage;

  KeyCount* local_bucket_counts;
  KeyCount* global_bucket_counts;
  int* first_bucket_by_rank;
  int* last_bucket_by_rank;
  int* bucket_to_rank;

  const VerificationData* verification;
  int* passed_verification;
  FullVerifySnapshot* final_snapshot;

  const std::vector<lci::device_t>* devices;
  RedistributorOptions redistributor_options;
  RedistributorRuntime* redistributor_runtime;
};

void run_sort_iteration(SortIterationContext* context);

} // namespace is_lci
