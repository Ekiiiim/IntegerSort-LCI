/*************************************************************************
 * LCI integer-sort iteration orchestration.
 *
 * This file wires NAS benchmark rules, reusable sort stages, and LCI
 * communication into one IS iteration. Pure NAS rules live under nas/;
 * reusable sort stages live under sort_core/; LCI communication lives under
 * communication/.
 *************************************************************************/

#include "benchmark_support/iteration_driver.hpp"

#include "sort_core/bucket_plan.hpp"
#include "sort_core/ranking.hpp"
#include "benchmark_support/benchmark_timers.hpp"
#include "communication/profiling/a2a_thread_profile.hpp"
#include "communication/reductions.hpp"
#include "nas/run_rules.hpp"
#include "nas/verification_rules.hpp"

#include "c_timers.h"

#include <omp.h>

#include <atomic>

namespace is_lci {

void run_sort_iteration(SortIterationContext* context) {
  int shift = MAX_KEY_LOG_2 - NUM_BUCKETS_LOG_2;

  timer_start_if_enabled(T_RANK, context->timeron);
  timer_start_if_enabled(T_RANK_1, context->timeron);

  apply_iteration_key_changes(context->keys, context->iteration, context->my_rank);

#pragma omp parallel for schedule(static)
  for (int i = 0; i < NUM_BUCKETS + TEST_ARRAY_SIZE; i++) {
    context->local_bucket_counts[i] = 0;
    context->global_bucket_counts[i] = 0;
    context->first_bucket_by_rank[i] = 0;
    context->last_bucket_by_rank[i] = 0;
    context->bucket_to_rank[i] = 0;
  }

  capture_partial_verification_keys(context->keys, context->local_key_count, context->my_rank, *context->verification,
                                    context->local_bucket_counts);

  timer_start_if_enabled(T_RANK_1_1, context->timeron);

  count_local_buckets(context->keys, context->local_key_count, shift, context->local_bucket_counts, NUM_BUCKETS);

  timer_stop_if_enabled(T_RANK_1_1, context->timeron);
  timer_stop_if_enabled(T_RANK_1, context->timeron);
  timer_stop_if_enabled(T_RANK, context->timeron);

  timer_start_if_enabled(T_RCOMM, context->timeron);

  lci::reduce_x(context->local_bucket_counts, context->global_bucket_counts, NUM_BUCKETS + TEST_ARRAY_SIZE,
                sizeof(KeyCount), sum_op_int, 0)
      .device((*context->devices)[0])();
  lci::broadcast_x(context->global_bucket_counts, (NUM_BUCKETS + TEST_ARRAY_SIZE) * sizeof(KeyCount), 0)
      .device((*context->devices)[0])();

  timer_stop_if_enabled(T_RCOMM, context->timeron);

  timer_start_if_enabled(T_RANK, context->timeron);
  timer_start_if_enabled(T_RANK_2, context->timeron);

  BucketPlan bucket_plan =
      build_bucket_plan(context->local_bucket_counts, context->global_bucket_counts, context->bucket_to_rank,
                        context->first_bucket_by_rank, context->last_bucket_by_rank, context->comm_size,
                        context->my_rank, NUM_BUCKETS, shift, context->local_key_count);

  KeyValue min_key_value = bucket_plan.min_key_value;
  KeyValue max_key_value = bucket_plan.max_key_value;

#pragma omp parallel for schedule(static)
  for (KeyValue i = 0; i < max_key_value - min_key_value + 1; i++) {
    context->frequency_storage[i].store(0, std::memory_order_relaxed);
    context->cumulative_storage[i] = 0;
  }

  std::atomic<KeyCount>* shifted_frequency = context->frequency_storage - min_key_value;
  reset_redistributor_iteration(context->redistributor_runtime, shifted_frequency);

  lci::barrier_x().device((*context->devices)[0])();

  timer_stop_if_enabled(T_RANK_2, context->timeron);
  timer_stop_if_enabled(T_RANK, context->timeron);

  timer_start_if_enabled(T_RCOMM, context->timeron);
  timer_start_if_enabled(T_ALLTOALL, context->timeron);
  begin_thread_local_alltoall_timers();

  redistribute_keys(context->keys, context->local_key_count, context->bucket_to_rank, shift,
                    bucket_plan.expected_recv_count, context->comm_size, context->my_rank, *context->devices,
                    context->redistributor_options, context->redistributor_runtime);

  timer_stop_if_enabled(T_ALLTOALL, context->timeron);
  timer_stop_if_enabled(T_RCOMM, context->timeron);

  timer_start_if_enabled(T_RANK, context->timeron);
  timer_start_if_enabled(T_RANK_3, context->timeron);

  KeyRank* shifted_cumulative = context->cumulative_storage - min_key_value;
  compute_local_ranks(shifted_frequency, shifted_cumulative, min_key_value, max_key_value);

  verify_partial_keys(context->iteration, min_key_value, max_key_value, bucket_plan.lesser_key_count,
                      shifted_cumulative, context->global_bucket_counts, *context->verification, context->my_rank,
                      context->passed_verification);

  timer_stop_if_enabled(T_RANK_3, context->timeron);
  timer_stop_if_enabled(T_RANK, context->timeron);

  finish_thread_local_alltoall_timers(context->iteration, context->my_rank, bucket_plan.local_key_count,
                                      timer_read(T_RANK));

  if (context->iteration == MAX_ITERATIONS) {
    context->final_snapshot->frequency_histogram = shifted_frequency;
    context->final_snapshot->min_key_value = min_key_value;
    context->final_snapshot->max_key_value = max_key_value;
    context->final_snapshot->total_local_keys = static_cast<KeyCount>(bucket_plan.local_key_count);
  }
}

} // namespace is_lci
