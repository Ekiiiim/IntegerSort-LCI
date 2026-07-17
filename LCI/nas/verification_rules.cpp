#include "nas/verification_rules.hpp"

#include "nas/timers.hpp"

#include <omp.h>

#include <cstdio>

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

void verify_partial_keys(int iteration, KeyValue min_key_value, KeyValue max_key_value, KeyRank lesser_key_count,
                         const KeyRank* cumulative_by_key, const KeyCount* bucket_size_totals,
                         const VerificationData& verification, int my_rank, int* passed_verification) {
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
                 const std::vector<lci::device_t>& devices, int* passed_verification) {
  lci::comp_t sync = lci::alloc_sync();
  lci::comp_t sync_send = lci::alloc_sync();

  timer_start_if_enabled(T_VERIFY);

  KeyCount idx = 0;
  for (KeyRank key = snapshot.min_key_value; key <= snapshot.max_key_value; ++key) {
    KeyCount count = snapshot.frequency_histogram[key].load(std::memory_order_relaxed);
    for (KeyCount c = 0; c < count; ++c) {
      key_array[idx++] = static_cast<KeyValue>(key);
    }
  }

  KeyValue previous_rank_last_key = 0;
  if (my_rank > 0) {
    lci::post_recv_x(my_rank - 1, &previous_rank_last_key, sizeof(KeyValue), 1000, sync)
        .device(devices[0])
        .allow_done(false)();
  }
  if (my_rank < comm_size - 1) {
    KeyCount last_local_key = (idx == 0) ? idx : (idx - 1);
    while (lci::post_send_x(my_rank + 1, &key_array[last_local_key], sizeof(KeyValue), 1000, sync_send)
               .device(devices[0])
               .allow_done(false)()
               .is_retry()) {
      lci::progress_x().device(devices[0])();
    }
    lci::sync_wait_x(sync_send, nullptr).device(devices[0])();
  }
  if (my_rank > 0) {
    lci::sync_wait_x(sync, nullptr).device(devices[0])();
  }

  lci::free_comp(&sync);
  lci::free_comp(&sync_send);

  KeyCount out_of_order = 0;
  if (my_rank > 0 && snapshot.total_local_keys > 0 && previous_rank_last_key > key_array[0]) {
    out_of_order++;
  }

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
