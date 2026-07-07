/*************************************************************************
 * NAS Parallel Benchmarks IS partial-verification cases.
 *
 * The test-index/test-rank tables and rank-adjustment rules are split out
 * from the NPB 3.4 MPI IS implementation.
 *************************************************************************/

#include "benchmark/nas/verification_cases.hpp"

namespace {

int S_test_index_array[TEST_ARRAY_SIZE] = {48427, 17148, 23627, 62548, 4431};
int S_test_rank_array[TEST_ARRAY_SIZE] = {0, 18, 346, 64917, 65463};

int W_test_index_array[TEST_ARRAY_SIZE] = {357773, 934767, 875723, 898999, 404505};
int W_test_rank_array[TEST_ARRAY_SIZE] = {1249, 11698, 1039987, 1043896, 1048018};

int A_test_index_array[TEST_ARRAY_SIZE] = {2112377, 662041, 5336171, 3642833, 4250760};
int A_test_rank_array[TEST_ARRAY_SIZE] = {104, 17523, 123928, 8288932, 8388264};

int B_test_index_array[TEST_ARRAY_SIZE] = {41869, 812306, 5102857, 18232239, 26860214};
int B_test_rank_array[TEST_ARRAY_SIZE] = {33422937, 10244, 59149, 33135281, 99};

int C_test_index_array[TEST_ARRAY_SIZE] = {44172927, 72999161, 74326391, 129606274, 21736814};
int C_test_rank_array[TEST_ARRAY_SIZE] = {61147, 882988, 266290, 133997595, 133525895};

long D_test_index_array[TEST_ARRAY_SIZE] = {1317351170, 995930646, 1157283250, 1503301535, 1453734525};
long D_test_rank_array[TEST_ARRAY_SIZE] = {1, 36538729, 1978098519, 2145192618, 2147425337};

long E_test_index_array[TEST_ARRAY_SIZE] = {21492309536L, 24606226181L, 12608530949L, 4065943607L, 3324513396L};
long E_test_rank_array[TEST_ARRAY_SIZE] = {3L, 27580354L, 3248475153L, 30048754302L, 31485259697L};

} // namespace

namespace is_lci {

KeyRank adjusted_test_rank(int iteration, int test_id, KeyRank base_rank) {
  KeyRank test_rank = base_rank;

  switch (CLASS) {
  case 'S':
    test_rank += (test_id <= 2) ? iteration : -iteration;
    break;
  case 'W':
    test_rank += (test_id < 2) ? iteration - 2 : -iteration;
    break;
  case 'A':
    test_rank += (test_id <= 2) ? iteration - 1 : -(iteration - 1);
    break;
  case 'B':
    test_rank += (test_id == 1 || test_id == 2 || test_id == 4) ? iteration : -iteration;
    break;
  case 'C':
    test_rank += (test_id <= 2) ? iteration : -iteration;
    break;
  case 'D':
    test_rank += (test_id < 2) ? iteration : -iteration;
    break;
  case 'E':
    if (test_id < 2) {
      test_rank += iteration - 2;
    } else if (test_id == 2) {
      test_rank += iteration - 2;
      if (iteration > 4) {
        test_rank -= 2;
      } else if (iteration > 2) {
        test_rank -= 1;
      }
    } else {
      test_rank -= iteration - 2;
    }
    break;
  }

  return test_rank;
}

void initialize_verification_data(VerificationData* verification) {
  for (int i = 0; i < TEST_ARRAY_SIZE; i++) {
    switch (CLASS) {
    case 'S':
      verification->test_index[i] = S_test_index_array[i];
      verification->test_rank[i] = S_test_rank_array[i];
      break;
    case 'A':
      verification->test_index[i] = A_test_index_array[i];
      verification->test_rank[i] = A_test_rank_array[i];
      break;
    case 'W':
      verification->test_index[i] = W_test_index_array[i];
      verification->test_rank[i] = W_test_rank_array[i];
      break;
    case 'B':
      verification->test_index[i] = B_test_index_array[i];
      verification->test_rank[i] = B_test_rank_array[i];
      break;
    case 'C':
      verification->test_index[i] = C_test_index_array[i];
      verification->test_rank[i] = C_test_rank_array[i];
      break;
    case 'D':
      verification->test_index[i] = D_test_index_array[i];
      verification->test_rank[i] = D_test_rank_array[i];
      break;
    case 'E':
      verification->test_index[i] = E_test_index_array[i];
      verification->test_rank[i] = E_test_rank_array[i];
      break;
    }
  }
}

} // namespace is_lci
