/*************************************************************************
 * Based on NAS benchmark
 *
 * Source logic: NPB 3.4 MPI IS partial-verification tables, rank-adjustment
 * rules, partial verification, and full verification.
 *
 * LCI adaptations that keep the benchmark logic unchanged:
 * - keeps NAS verification cases internal to this module instead of storing
 *   them in the benchmark driver;
 * - consumes caller-owned key, bucket, histogram, and rank buffers instead of
 *   NAS global arrays;
 * - uses the LCI point-to-point API for the full-verify neighbor key exchange
 *   while preserving the same ordering checks.
 *************************************************************************/

#include "nas/verification.hpp"

#include "irregular/irregular_runtime.hpp"
#include "nas/timers.hpp"

#include <lci.hpp>
#include <omp.h>

#include <cstdio>

namespace is_lci {
namespace {

struct VerificationData {
  KeyRank test_index[TEST_ARRAY_SIZE];
  KeyRank test_rank[TEST_ARRAY_SIZE];
};

struct PartialVerificationResult {
  bool checked;
  bool passed;
  int test_id;
  KeyValue key;
  KeyRank computed_rank;
  KeyRank expected_rank;
};

const int S_test_index_array[TEST_ARRAY_SIZE] = {48427, 17148, 23627, 62548, 4431};
const int S_test_rank_array[TEST_ARRAY_SIZE] = {0, 18, 346, 64917, 65463};

const int W_test_index_array[TEST_ARRAY_SIZE] = {357773, 934767, 875723, 898999, 404505};
const int W_test_rank_array[TEST_ARRAY_SIZE] = {1249, 11698, 1039987, 1043896, 1048018};

const int A_test_index_array[TEST_ARRAY_SIZE] = {2112377, 662041, 5336171, 3642833, 4250760};
const int A_test_rank_array[TEST_ARRAY_SIZE] = {104, 17523, 123928, 8288932, 8388264};

const int B_test_index_array[TEST_ARRAY_SIZE] = {41869, 812306, 5102857, 18232239, 26860214};
const int B_test_rank_array[TEST_ARRAY_SIZE] = {33422937, 10244, 59149, 33135281, 99};

const int C_test_index_array[TEST_ARRAY_SIZE] = {44172927, 72999161, 74326391, 129606274, 21736814};
const int C_test_rank_array[TEST_ARRAY_SIZE] = {61147, 882988, 266290, 133997595, 133525895};

const long D_test_index_array[TEST_ARRAY_SIZE] = {1317351170, 995930646, 1157283250, 1503301535, 1453734525};
const long D_test_rank_array[TEST_ARRAY_SIZE] = {1, 36538729, 1978098519, 2145192618, 2147425337};

const long E_test_index_array[TEST_ARRAY_SIZE] = {21492309536L, 24606226181L, 12608530949L, 4065943607L, 3324513396L};
const long E_test_rank_array[TEST_ARRAY_SIZE] = {3L, 27580354L, 3248475153L, 30048754302L, 31485259697L};

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

VerificationData make_verification_data() {
  VerificationData verification{};
  for (int i = 0; i < TEST_ARRAY_SIZE; i++) {
    switch (CLASS) {
    case 'S':
      verification.test_index[i] = S_test_index_array[i];
      verification.test_rank[i] = S_test_rank_array[i];
      break;
    case 'A':
      verification.test_index[i] = A_test_index_array[i];
      verification.test_rank[i] = A_test_rank_array[i];
      break;
    case 'W':
      verification.test_index[i] = W_test_index_array[i];
      verification.test_rank[i] = W_test_rank_array[i];
      break;
    case 'B':
      verification.test_index[i] = B_test_index_array[i];
      verification.test_rank[i] = B_test_rank_array[i];
      break;
    case 'C':
      verification.test_index[i] = C_test_index_array[i];
      verification.test_rank[i] = C_test_rank_array[i];
      break;
    case 'D':
      verification.test_index[i] = D_test_index_array[i];
      verification.test_rank[i] = D_test_rank_array[i];
      break;
    case 'E':
      verification.test_index[i] = E_test_index_array[i];
      verification.test_rank[i] = E_test_rank_array[i];
      break;
    }
  }
  return verification;
}

const VerificationData& nas_verification_data() {
  static const VerificationData verification = make_verification_data();
  return verification;
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

} // namespace

void capture_partial_verification_keys(const KeyValue* keys, KeyCount local_key_count, int my_rank,
                                       KeyCount* local_bucket_counts) {
  const VerificationData& verification = nas_verification_data();
  for (int test_id = 0; test_id < TEST_ARRAY_SIZE; test_id++) {
    if ((verification.test_index[test_id] / local_key_count) == my_rank) {
      KeyCount local_index = static_cast<KeyCount>(verification.test_index[test_id] % local_key_count);
      local_bucket_counts[NUM_BUCKETS + test_id] = keys[local_index];
    }
  }
}

void verify_partial_keys(int iteration, KeyValue min_key_value, KeyValue max_key_value, KeyRank lesser_key_count,
                         const KeyRank* cumulative_by_key, const KeyCount* bucket_size_totals, int my_rank,
                         int* passed_verification) {
  const VerificationData& verification = nas_verification_data();
  for (int i = 0; i < TEST_ARRAY_SIZE; i++) {
    PartialVerificationResult result =
        check_partial_verification_key(iteration, i, min_key_value, max_key_value, lesser_key_count, cumulative_by_key,
                                       bucket_size_totals, verification);
    if (!result.checked) {
      continue;
    }

    if (result.passed) {
      (*passed_verification)++;
    } else {
      printf("Failed partial verification: iteration %d, processor %d, test key %d, key rank %ld\n", iteration, my_rank,
             result.test_id, static_cast<long>(result.computed_rank));
    }
  }
}

void full_verify(KeyValue* key_array, const FullVerifySnapshot& snapshot, int my_rank, int comm_size,
                 const lci_irregular::IrregularRuntime& runtime, int* passed_verification) {
  const lci::device_t& control_device = lci_irregular::detail::control_device(runtime);
  lci::comp_t sync = lci::alloc_sync();
  lci::comp_t sync_send = lci::alloc_sync();

  timer_start_if_enabled(T_VERIFY);

  /*  Now, finally, sort the keys:  */
  KeyCount idx = 0;
  for (KeyRank key = snapshot.min_key_value; key <= snapshot.max_key_value; ++key) {
    KeyCount count = snapshot.frequency_histogram[key].load(std::memory_order_relaxed);
    for (KeyCount c = 0; c < count; ++c) {
      key_array[idx++] = static_cast<KeyValue>(key);
    }
  }

  KeyValue previous_rank_last_key = 0;
  /*  Send largest key value to next processor  */
  if (my_rank > 0) {
    lci::post_recv_x(my_rank - 1, &previous_rank_last_key, sizeof(KeyValue), 1000, sync)
        .device(control_device)
        .allow_done(false)();
  }
  if (my_rank < comm_size - 1) {
    KeyCount last_local_key = (idx == 0) ? idx : (idx - 1);
    while (lci::post_send_x(my_rank + 1, &key_array[last_local_key], sizeof(KeyValue), 1000, sync_send)
               .device(control_device)
               .allow_done(false)()
               .is_retry()) {
      lci::progress_x().device(control_device)();
    }
    lci::sync_wait_x(sync_send, nullptr).device(control_device)();
  }
  if (my_rank > 0) {
    lci::sync_wait_x(sync, nullptr).device(control_device)();
  }

  lci::free_comp(&sync);
  lci::free_comp(&sync_send);

  /*  Confirm that neighbor's greatest key value
      is not greater than my least key value       */
  KeyCount out_of_order = 0;
  if (my_rank > 0 && snapshot.total_local_keys > 0 && previous_rank_last_key > key_array[0]) {
    out_of_order++;
  }

  /*  Confirm keys correctly sorted: count incorrectly sorted keys, if any */
  #pragma omp parallel for schedule(static) reduction(+ : out_of_order)
  for (KeyCount i = 1; i < snapshot.total_local_keys; i++) {
    if (key_array[i - 1] > key_array[i]) {
      out_of_order++;
    }
  }

  if (out_of_order != 0) {
    printf("Processor %d:  Full_verify: number of keys out of sort: %d\n", my_rank, out_of_order);
  } else {
    (*passed_verification)++;
  }

  timer_stop_if_enabled(T_VERIFY);
}

} // namespace is_lci
