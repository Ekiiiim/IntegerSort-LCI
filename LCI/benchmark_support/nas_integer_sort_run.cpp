#include "benchmark_support/nas_integer_sort_run.hpp"

#include "benchmark_support/benchmark_timers.hpp"
#include "benchmark_support/results.hpp"
#include "benchmark_support/run_options.hpp"
#include "benchmark_support/verification.hpp"
#include "communication/profiling/a2a_thread_profile.hpp"
#include "communication/reductions.hpp"
#include "integer_sort/ranking.hpp"
#include "nas/key_generation.hpp"
#include "nas/problem_config.hpp"
#include "nas/run_rules.hpp"
#include "nas/verification_rules.hpp"

#include "c_timers.h"

#include <lci.hpp>

#include <cstdio>
#include <cstdlib>

namespace is_lci {

NasIntegerSortRun::NasIntegerSortRun(LciRuntime& runtime) : runtime_(runtime) {}

NasIntegerSortRun::~NasIntegerSortRun() {
  if (redistributor_initialized_) {
    finalize_redistributor(&redistributor_runtime_);
  }
}

bool NasIntegerSortRun::initialize() {
  configure_output();

  if (!is_process_count_in_range(runtime_.rank_count())) {
    if (runtime_.rank() == 0) {
      printf("\n ERROR: number of processes %d not within range %d-%d"
             "\n Exiting program!\n\n",
             runtime_.rank_count(), MIN_PROCS, MAX_PROCS);
    }
    exit_code_ = 1;
    return false;
  }

  if (!determine_active_rank_count()) {
    return false;
  }

  initialize_verification_data(&verification_);

  if (runtime_.rank() == 0) {
    use_upacket_ = read_use_upacket_flag();
    use_loopback_ = read_loopback_flag();
    timeron_ = check_timer_flag_from_file();
  }
  runtime_.broadcast_int(&use_upacket_, 0);
  runtime_.broadcast_int(&use_loopback_, 0);
  runtime_.broadcast_int(&timeron_, 0);
  print_initial_status(runtime_.rank(), runtime_.rank_count(), active_rank_count_, use_upacket_, use_loopback_);

  clear_timers();

  KeyCount local_key_count = compute_local_key_count(active_rank_count_);
  KeyCount work_buffer_size = compute_work_buffer_size(active_rank_count_, local_key_count);
  workspace_ = std::make_unique<IntegerSortWorkspace>(local_key_count, work_buffer_size, NUM_BUCKETS + TEST_ARRAY_SIZE);

  message_batch_size_ = lci::get_max_bcopy_size() / sizeof(KeyValue) - 1;
  RedistributorOptions options = redistributor_options();
  allocate_fallback_buffers(&redistributor_runtime_, active_rank_count_, runtime_.max_threads(), options);
  initialize_redistributor(&redistributor_runtime_, options);
  redistributor_initialized_ = true;

  return true;
}

int NasIntegerSortRun::exit_code() const {
  return exit_code_;
}

void NasIntegerSortRun::generate_keys() {
  is_lci::generate_keys(workspace().keys(), workspace().local_key_count(), static_cast<KeyRank>(MAX_KEY),
                        find_my_seed(runtime_.rank(), active_rank_count_, compute_total_random_numbers(),
                                     NAS_RANDOM_SEED, NAS_RANDOM_MULTIPLIER),
                        NAS_RANDOM_MULTIPLIER);
  runtime_.barrier();
}

void NasIntegerSortRun::run_warmup_iteration() {
  current_iteration_ = 1;
  run_current_iteration_pipeline();
}

void NasIntegerSortRun::start_timed_region() {
  passed_verification_ = 0;
  if (runtime_.rank() == 0 && CLASS != 'S') {
    printf("\n   iteration\n");
  }

  timer_clear(0);
  clear_timers();
  timer_start(0);
}

void NasIntegerSortRun::begin_iteration(int iteration) {
  current_iteration_ = iteration;
  if (runtime_.rank() == 0 && CLASS != 'S') {
    printf("        %d\n", iteration);
  }
  runtime_.barrier();
}

void NasIntegerSortRun::apply_nas_key_changes() {
  timer_start_if_enabled(T_RANK, timeron_);
  timer_start_if_enabled(T_RANK_1, timeron_);

  apply_iteration_key_changes(workspace().keys(), current_iteration_, runtime_.rank());
}

void NasIntegerSortRun::organize_keys_into_buckets() {
  workspace().clear_bucket_metadata();

  capture_partial_verification_keys(workspace().keys(), workspace().local_key_count(), runtime_.rank(), verification_,
                                    workspace().local_bucket_counts());

  int bucket_shift = MAX_KEY_LOG_2 - NUM_BUCKETS_LOG_2;
  timer_start_if_enabled(T_RANK_1_1, timeron_);
  count_local_buckets(workspace().keys(), workspace().local_key_count(), bucket_shift,
                      workspace().local_bucket_counts(), NUM_BUCKETS);
  timer_stop_if_enabled(T_RANK_1_1, timeron_);
  timer_stop_if_enabled(T_RANK_1, timeron_);
  timer_stop_if_enabled(T_RANK, timeron_);
}

void NasIntegerSortRun::compute_global_bucket_sizes() {
  timer_start_if_enabled(T_RCOMM, timeron_);

  runtime_.reduce(workspace().local_bucket_counts(), workspace().global_bucket_counts(), NUM_BUCKETS + TEST_ARRAY_SIZE,
                  sizeof(KeyCount), sum_op_int, 0);
  runtime_.broadcast_bytes(workspace().global_bucket_counts(), (NUM_BUCKETS + TEST_ARRAY_SIZE) * sizeof(KeyCount), 0);

  timer_stop_if_enabled(T_RCOMM, timeron_);
}

void NasIntegerSortRun::assign_buckets_to_processes() {
  int bucket_shift = MAX_KEY_LOG_2 - NUM_BUCKETS_LOG_2;

  timer_start_if_enabled(T_RANK, timeron_);
  timer_start_if_enabled(T_RANK_2, timeron_);

  current_bucket_plan_ = build_bucket_plan(workspace().local_bucket_counts(), workspace().global_bucket_counts(),
                                           workspace().bucket_to_rank(), workspace().first_bucket_by_rank(),
                                           workspace().last_bucket_by_rank(), active_rank_count_, runtime_.rank(),
                                           NUM_BUCKETS, bucket_shift, workspace().local_key_count());

  workspace().clear_frequency_range(current_bucket_plan_.min_key_value, current_bucket_plan_.max_key_value);
  reset_redistributor_iteration(&redistributor_runtime_,
                                workspace().frequency_by_key(current_bucket_plan_.min_key_value));

  runtime_.barrier();

  timer_stop_if_enabled(T_RANK_2, timeron_);
  timer_stop_if_enabled(T_RANK, timeron_);
}

void NasIntegerSortRun::redistribute_keys_with_lci() {
  int bucket_shift = MAX_KEY_LOG_2 - NUM_BUCKETS_LOG_2;

  timer_start_if_enabled(T_RCOMM, timeron_);
  timer_start_if_enabled(T_ALLTOALL, timeron_);
  begin_thread_local_alltoall_timers();

  redistribute_keys(workspace().keys(), workspace().local_key_count(), workspace().bucket_to_rank(), bucket_shift,
                    current_bucket_plan_.expected_recv_count, active_rank_count_, runtime_.rank(), runtime_.devices(),
                    redistributor_options(), &redistributor_runtime_);

  timer_stop_if_enabled(T_ALLTOALL, timeron_);
  timer_stop_if_enabled(T_RCOMM, timeron_);
}

void NasIntegerSortRun::compute_final_ranking() {
  timer_start_if_enabled(T_RANK, timeron_);
  timer_start_if_enabled(T_RANK_3, timeron_);

  compute_local_ranks(workspace().frequency_by_key(current_bucket_plan_.min_key_value),
                      workspace().cumulative_by_key(current_bucket_plan_.min_key_value),
                      current_bucket_plan_.min_key_value, current_bucket_plan_.max_key_value);
}

void NasIntegerSortRun::verify_partial_ranking() {
  verify_partial_keys(current_iteration_, current_bucket_plan_.min_key_value, current_bucket_plan_.max_key_value,
                      current_bucket_plan_.lesser_key_count,
                      workspace().cumulative_by_key(current_bucket_plan_.min_key_value),
                      workspace().global_bucket_counts(), verification_, runtime_.rank(), &passed_verification_);

  timer_stop_if_enabled(T_RANK_3, timeron_);
  timer_stop_if_enabled(T_RANK, timeron_);
}

void NasIntegerSortRun::finish_iteration() {
  finish_thread_local_alltoall_timers(current_iteration_, runtime_.rank(), current_bucket_plan_.local_key_count,
                                      timer_read(T_RANK));

  if (current_iteration_ == MAX_ITERATIONS) {
    final_snapshot_.frequency_histogram = workspace().frequency_by_key(current_bucket_plan_.min_key_value);
    final_snapshot_.min_key_value = current_bucket_plan_.min_key_value;
    final_snapshot_.max_key_value = current_bucket_plan_.max_key_value;
    final_snapshot_.total_local_keys = static_cast<KeyCount>(current_bucket_plan_.local_key_count);
  }
}

void NasIntegerSortRun::stop_timed_region() {
  timer_stop(0);
  double timecounter = timer_read(0);
  runtime_.reduce(&timecounter, &max_time_, 1, sizeof(double), max_op, 0);
}

void NasIntegerSortRun::verify_full_ranking() {
  full_verify(workspace().keys(), final_snapshot_, runtime_.rank(), active_rank_count_, runtime_.devices(), timeron_,
              &passed_verification_);

  int local_passed_verification = passed_verification_;
  runtime_.reduce(&local_passed_verification, &passed_verification_, 1, sizeof(int), sum_op_int, 0);
}

void NasIntegerSortRun::print_results() {
  if (runtime_.rank() == 0) {
    print_final_results(active_rank_count_, runtime_.rank_count(), max_time_, passed_verification_);
  }

  if (timeron_) {
    print_timer_summary(active_rank_count_, runtime_.devices());
  }
}

bool NasIntegerSortRun::determine_active_rank_count() {
  active_rank_count_ = greatest_power_of_two_at_most(runtime_.rank_count());

  int abort_for_non_power_of_two = 1;
  if (active_rank_count_ != runtime_.rank_count()) {
    if (runtime_.rank() == 0) {
      abort_for_non_power_of_two = should_abort_for_non_power_of_two_process_count(getenv("NPB_NPROCS_STRICT")) ? 1 : 0;
    }
    runtime_.broadcast_int(&abort_for_non_power_of_two, 0);

    if (abort_for_non_power_of_two) {
      if (runtime_.rank() == 0) {
        fprintf(stderr,
                "\n ERROR: Number of processes (%d) is not a power of two (%d?)\n"
                " Exiting program!\n\n",
                runtime_.rank_count(), active_rank_count_);
      }
      exit_code_ = 1;
      return false;
    }

    if (runtime_.rank() >= active_rank_count_) {
      exit_code_ = 0;
      return false;
    }
  }

  return true;
}

void NasIntegerSortRun::configure_output() {
  setvbuf(stderr, nullptr, _IONBF, 0);
}

RedistributorOptions NasIntegerSortRun::redistributor_options() const {
  return RedistributorOptions{
      use_upacket_ == 1,
      use_loopback_ == 1,
      message_batch_size_,
  };
}

IntegerSortWorkspace& NasIntegerSortRun::workspace() {
  return *workspace_;
}

const IntegerSortWorkspace& NasIntegerSortRun::workspace() const {
  return *workspace_;
}

void NasIntegerSortRun::run_current_iteration_pipeline() {
  apply_nas_key_changes();
  organize_keys_into_buckets();
  compute_global_bucket_sizes();
  assign_buckets_to_processes();
  redistribute_keys_with_lci();
  compute_final_ranking();
  verify_partial_ranking();
  finish_iteration();
}

} // namespace is_lci
