#pragma once

#include "benchmark/nas/verification_cases.hpp"

#include <lci.hpp>

#include <atomic>
#include <vector>

namespace is_lci {

struct FullVerifySnapshot {
  std::atomic<KeyCount>* frequency_histogram = nullptr;
  KeyValue min_key_value = 0;
  KeyValue max_key_value = 0;
  KeyCount total_local_keys = 0;
};

void verify_partial_keys(int iteration, KeyValue min_key_value, KeyValue max_key_value, KeyRank lesser_key_count,
                         const KeyRank* cumulative_by_key, const KeyCount* bucket_size_totals,
                         const VerificationData& verification, int my_rank, int* passed_verification);

void full_verify(KeyValue* key_array, const FullVerifySnapshot& snapshot, int my_rank, int comm_size,
                 const std::vector<lci::device_t>& devices, int timeron, int* passed_verification);

} // namespace is_lci
