#pragma once

#include "nas/verification_cases.hpp"

namespace is_lci {

// NAS IS partial-verification result without benchmark-side printing/counters.
struct PartialVerificationResult {
  bool checked;
  bool passed;
  int test_id;
  KeyValue key;
  KeyRank computed_rank;
  KeyRank expected_rank;
};

void capture_partial_verification_keys(const KeyValue* keys, KeyCount local_key_count, int my_rank,
                                       const VerificationData& verification, KeyCount* local_bucket_counts);

PartialVerificationResult check_partial_verification_key(int iteration, int test_id, KeyValue min_key_value,
                                                         KeyValue max_key_value, KeyRank lesser_key_count,
                                                         const KeyRank* cumulative_by_key,
                                                         const KeyCount* bucket_size_totals,
                                                         const VerificationData& verification);

} // namespace is_lci
