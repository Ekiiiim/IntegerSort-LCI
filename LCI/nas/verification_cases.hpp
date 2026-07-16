#pragma once

#include "types.hpp"

namespace is_lci {

struct VerificationData {
  KeyRank test_index[TEST_ARRAY_SIZE];
  KeyRank test_rank[TEST_ARRAY_SIZE];
};

void initialize_verification_data(VerificationData* verification);
KeyRank adjusted_test_rank(int iteration, int test_id, KeyRank base_rank);

} // namespace is_lci
