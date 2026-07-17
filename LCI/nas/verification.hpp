#pragma once

#include "types.hpp"

#include <lci.hpp>

#include <atomic>
#include <vector>

namespace is_lci {

struct VerificationData {
  KeyRank test_index[TEST_ARRAY_SIZE];
  KeyRank test_rank[TEST_ARRAY_SIZE];
};

// NAS IS partial-verification result.
struct PartialVerificationResult {
  bool checked;
  bool passed;
  int test_id;
  KeyValue key;
  KeyRank computed_rank;
  KeyRank expected_rank;
};

// Snapshot of the final LCI integer-sort state consumed by the NAS full
// verification rule.
struct FullVerifySnapshot {
  std::atomic<KeyCount>* frequency_histogram = nullptr;
  KeyValue min_key_value = 0;
  KeyValue max_key_value = 0;
  KeyCount total_local_keys = 0;
};

void initialize_verification_data(VerificationData* verification);

void capture_partial_verification_keys(const KeyValue* keys, KeyCount local_key_count, int my_rank,
                                       const VerificationData& verification, KeyCount* local_bucket_counts);

PartialVerificationResult check_partial_verification_key(int iteration, int test_id, KeyValue min_key_value,
                                                         KeyValue max_key_value, KeyRank lesser_key_count,
                                                         const KeyRank* cumulative_by_key,
                                                         const KeyCount* bucket_size_totals,
                                                         const VerificationData& verification);

void verify_partial_keys(int iteration, KeyValue min_key_value, KeyValue max_key_value, KeyRank lesser_key_count,
                         const KeyRank* cumulative_by_key, const KeyCount* bucket_size_totals,
                         const VerificationData& verification, int my_rank, int* passed_verification);

void full_verify(KeyValue* key_array, const FullVerifySnapshot& snapshot, int my_rank, int comm_size,
                 const std::vector<lci::device_t>& devices, int* passed_verification);

} // namespace is_lci
