#include "nas/verification_rules.hpp"

namespace is_lci {

void capture_partial_verification_keys(const KeyValue* keys, KeyCount local_key_count, int my_rank,
                                       const VerificationData& verification, KeyCount* local_bucket_counts) {
  for (int test_id = 0; test_id < TEST_ARRAY_SIZE; test_id++) {
    if ((verification.test_index[test_id] / local_key_count) == my_rank) {
      KeyCount local_index = static_cast<KeyCount>(verification.test_index[test_id] % local_key_count);
      local_bucket_counts[NUM_BUCKETS + test_id] = keys[local_index];
    }
  }
}

PartialVerificationResult check_partial_verification_key(int iteration, int test_id, KeyValue min_key_value,
                                                         KeyValue max_key_value, KeyRank lesser_key_count,
                                                         const KeyRank* cumulative_by_key,
                                                         const KeyCount* bucket_size_totals,
                                                         const VerificationData& verification) {
  KeyValue key = static_cast<KeyValue>(bucket_size_totals[NUM_BUCKETS + test_id]);
  PartialVerificationResult result{
      false, false, test_id, key, 0, adjusted_test_rank(iteration, test_id, verification.test_rank[test_id]),
  };

  if (key < min_key_value || key > max_key_value) {
    return result;
  }

  result.checked = true;
  result.computed_rank = cumulative_by_key[key - 1] + lesser_key_count;
  result.passed = result.computed_rank == result.expected_rank;
  return result;
}

} // namespace is_lci
