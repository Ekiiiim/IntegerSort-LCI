/*************************************************************************
 * LCI integer-sort iteration orchestration.
 *
 * This file is the implementation-facing algorithm skeleton for one timed
 * IS iteration. Benchmark timing and verification live under benchmark/;
 * NAS problem data stays in benchmark/nas; individual algorithm stages stay
 * in algorithm, and LCI communication stays in communication.
 *************************************************************************/

#include "algorithm/sort_iteration.hpp"

#include "algorithm/bucket_plan.hpp"
#include "algorithm/ranking.hpp"
#include "benchmark/timing/timers.hpp"
#include "benchmark/profiling/a2a_tl_timers.hpp"
#include "communication/reductions.hpp"

#include "c_timers.h"

#include <omp.h>

#include <atomic>

namespace is_lci {

void run_sort_iteration(SortIterationContext* context) {
  int shift = MAX_KEY_LOG_2 - NUM_BUCKETS_LOG_2;

  timer_start_if_enabled(T_RANK, context->timeron);
  timer_start_if_enabled(T_RANK_1, context->timeron);

  if (context->my_rank == 0) {
    context->keys[context->iteration] = context->iteration;
    context->keys[context->iteration + MAX_ITERATIONS] = MAX_KEY - context->iteration;
  }

#pragma omp parallel for schedule(static)
  for (int i = 0; i < NUM_BUCKETS + TEST_ARRAY_SIZE; i++) {
    context->local_bucket_counts[i] = 0;
    context->global_bucket_counts[i] = 0;
    context->first_bucket_by_rank[i] = 0;
    context->last_bucket_by_rank[i] = 0;
    context->bucket_to_rank[i] = 0;
  }

  for (int i = 0; i < TEST_ARRAY_SIZE; i++) {
    if ((context->verification->test_index[i] / context->local_key_count) == context->my_rank) {
      context->local_bucket_counts[NUM_BUCKETS + i] =
          context->keys[context->verification->test_index[i] % context->local_key_count];
    }
  }

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
