#pragma once

#include "integer_sort/types.hpp"

#include <atomic>

namespace lci_irregular {
class IrregularRuntime;
}

namespace is_lci {

// Snapshot of the final LCI integer-sort state consumed by the NAS full
// verification rule.
struct FullVerifySnapshot {
  std::atomic<KeyCount>* frequency_histogram = nullptr;
  KeyValue min_key_value = 0;
  KeyValue max_key_value = 0;
  KeyCount total_local_keys = 0;
};

void capture_partial_verification_keys(const KeyValue* keys, KeyCount local_key_count, int my_rank,
                                       KeyCount* local_bucket_counts);

void verify_partial_keys(int iteration, KeyValue min_key_value, KeyValue max_key_value, KeyRank lesser_key_count,
                         const KeyRank* cumulative_by_key, const KeyCount* bucket_size_totals, int my_rank,
                         int* passed_verification);

void full_verify(KeyValue* key_array, const FullVerifySnapshot& snapshot, int my_rank, int comm_size,
                 const lci_irregular::IrregularRuntime& runtime, int* passed_verification);

} // namespace is_lci
