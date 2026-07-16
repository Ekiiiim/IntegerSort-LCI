/*************************************************************************
 *                                                                       *
 *        N  A  S     P A R A L L E L     B E N C H M A R K S  3.4       *
 *                                                                       *
 *                                  I S                                  *
 *                                                                       *
 *************************************************************************
 *                                                                       *
 *   This benchmark is part of the NAS Parallel Benchmark 3.4 suite.     *
 *   It is described in NAS Technical Report 95-020.                     *
 *                                                                       *
 *   Permission to use, copy, distribute and modify this software        *
 *   for any purpose with or without fee is hereby granted.  We          *
 *   request, however, that all derived work reference the NAS           *
 *   Parallel Benchmarks 3.4. This software is provided "as is"          *
 *   without express or implied warranty.                                *
 *                                                                       *
 *   Information on NPB 3.4, including the technical report, the         *
 *   original specifications, source code, results and information       *
 *   on how to submit new results, is available at:                      *
 *                                                                       *
 *          http://www.nas.nasa.gov/Software/NPB                         *
 *                                                                       *
 *   Send comments or suggestions to  npb@nas.nasa.gov                   *
 *                                                                       *
 *         NAS Parallel Benchmarks Group                                 *
 *         NASA Ames Research Center                                     *
 *         Moffett Field, CA   94035-1000                                *
 *                                                                       *
 *************************************************************************
 *                                                                       *
 *   Author: M. Yarrow                                                   *
 *           H. Jin                                                      *
 *                                                                       *
 *************************************************************************/

/*************************************************************************
 *  LCI + OpenMP variant of NPB IS -- Multithreaded Fine-grained
 *  Asynchronous BSP (FA-BSP).
 *
 *  This file is intentionally a thin benchmark executable. NAS benchmark
 *  contract code lives under nas/, executable support lives under
 *  benchmark_support/, reusable LCI communication lives under communication/,
 *  and reusable integer-sort stages live under sort_core/.
 *************************************************************************/

#include "benchmark_support/iteration_driver.hpp"
#include "benchmark_support/run_options.hpp"
#include "benchmark_support/results.hpp"
#include "benchmark_support/benchmark_timers.hpp"
#include "benchmark_support/verification.hpp"
#include "nas/key_generation.hpp"
#include "nas/run_rules.hpp"
#include "nas/verification_cases.hpp"
#include "communication/lci_redistributor.hpp"
#include "communication/reductions.hpp"

#include "c_timers.h"

#include <lci.hpp>
#include <omp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using is_lci::KeyCount;
using is_lci::KeyRank;
using is_lci::KeyValue;

int msg_batch_size;
KeyCount num_keys;
KeyCount size_of_buffers;

int my_rank;
int np_total;
int comm_size;

int timeron;
int use_upacket;
int use_loopback;

KeyValue* key_array;
std::atomic<KeyCount>* key_buff1;
KeyRank* cumulative_key_buff_ptr;

KeyCount bucket_size[NUM_BUCKETS + TEST_ARRAY_SIZE];
KeyCount bucket_size_totals[NUM_BUCKETS + TEST_ARRAY_SIZE];
int process_bucket_distrib_ptr1[NUM_BUCKETS + TEST_ARRAY_SIZE];
int process_bucket_distrib_ptr2[NUM_BUCKETS + TEST_ARRAY_SIZE];
int bucket_i_to_process_ranks[NUM_BUCKETS + TEST_ARRAY_SIZE];

int passed_verification;

is_lci::VerificationData verification;
is_lci::FullVerifySnapshot final_snapshot;
is_lci::RedistributorRuntime redistributor_runtime;
std::vector<lci::device_t> devices;

void allocate_work_arrays() {
  num_keys = is_lci::compute_local_key_count(comm_size);
  size_of_buffers = is_lci::compute_work_buffer_size(comm_size, num_keys);
  msg_batch_size = lci::get_max_bcopy_size() / sizeof(KeyValue) - 1;

  key_array = static_cast<KeyValue*>(malloc(sizeof(KeyValue) * size_of_buffers));
  key_buff1 = static_cast<std::atomic<KeyCount>*>(malloc(sizeof(std::atomic<KeyCount>) * size_of_buffers));
  cumulative_key_buff_ptr = static_cast<KeyRank*>(malloc(sizeof(KeyRank) * size_of_buffers));

  if (!key_array || !key_buff1 || !cumulative_key_buff_ptr) {
    printf("ERROR: memory allocation failed\n");
    lci::g_runtime_fina();
    exit(1);
  }
}

void free_work_arrays() {
  free(key_array);
  free(key_buff1);
  free(cumulative_key_buff_ptr);

  key_array = nullptr;
  key_buff1 = nullptr;
  cumulative_key_buff_ptr = nullptr;
}

int get_num_threads_per_device() {
  const char* env_val = getenv("NUM_THREADS_PER_DEVICE");
  if (!env_val) {
    if (my_rank == 0) {
      fprintf(stderr, "[Warning] NUM_THREADS_PER_DEVICE not set, using 1\n");
    }
    return 1;
  }

  int threads_per_device = atoi(env_val);
  if (threads_per_device <= 0 || threads_per_device > omp_get_max_threads()) {
    if (my_rank == 0) {
      fprintf(stderr, "[Warning] Invalid NUM_THREADS_PER_DEVICE value %d, using 1 instead\n", threads_per_device);
    }
    return 1;
  }
  return threads_per_device;
}

void allocate_devices() {
  int threads_per_proc = omp_get_max_threads();
  int threads_per_device = get_num_threads_per_device();
  int num_devices = threads_per_proc / threads_per_device;

  size_t npackets = lci::get_default_packet_pool().get_attr_npackets();
  size_t max_nrecvs_per_device = std::min(npackets / 8 / num_devices, 4096UL);
  size_t max_nsends_per_device = std::min(npackets / 4 / lci::get_rank_n() / num_devices, 64UL);
  max_nsends_per_device = std::max(max_nsends_per_device, 4UL);

  for (int i = 0; i < num_devices; ++i) {
    devices.push_back(
        lci::alloc_device_x().net_max_sends(max_nsends_per_device).net_max_recvs(max_nrecvs_per_device)());
  }
}

void free_devices() {
  for (auto& device : devices) {
    lci::free_device(&device);
  }
  devices.clear();
}

int determine_active_comm_size() {
  comm_size = is_lci::greatest_power_of_two_at_most(np_total);

  int abort_for_non_power_of_two = 1;
  if (comm_size != np_total) {
    if (my_rank == 0) {
      abort_for_non_power_of_two =
          is_lci::should_abort_for_non_power_of_two_process_count(getenv("NPB_NPROCS_STRICT")) ? 1 : 0;
    }
    lci::broadcast_x(&abort_for_non_power_of_two, sizeof(int), 0).device(devices[0])();

    if (abort_for_non_power_of_two) {
      if (my_rank == 0) {
        fprintf(stderr,
                "\n ERROR: Number of processes (%d) is not a power of two (%d?)\n"
                " Exiting program!\n\n",
                np_total, comm_size);
      }
      return -1;
    }

    return (my_rank >= comm_size) ? 0 : 1;
  }

  return 1;
}

void exit_after_lci_cleanup(int exit_code) {
  free_devices();
  lci::g_runtime_fina();
  exit(exit_code);
}

is_lci::RedistributorOptions redistributor_options() {
  return is_lci::RedistributorOptions{
      use_upacket == 1,
      use_loopback == 1,
      msg_batch_size,
  };
}

void run_iteration(int iteration) {
  is_lci::SortIterationContext context{
      iteration,
      my_rank,
      comm_size,
      num_keys,
      timeron,
      key_array,
      key_buff1,
      cumulative_key_buff_ptr,
      bucket_size,
      bucket_size_totals,
      process_bucket_distrib_ptr1,
      process_bucket_distrib_ptr2,
      bucket_i_to_process_ranks,
      &verification,
      &passed_verification,
      &final_snapshot,
      &devices,
      redistributor_options(),
      &redistributor_runtime,
  };
  is_lci::run_sort_iteration(&context);
}

} // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  setvbuf(stderr, nullptr, _IONBF, 0);
  lci::g_runtime_init_x().alloc_default_device(false)();
  my_rank = lci::get_rank_me();
  np_total = lci::get_rank_n();
  allocate_devices();
  lci::barrier_x().device(devices[0])();

  if (!is_lci::is_process_count_in_range(np_total)) {
    if (my_rank == 0) {
      printf("\n ERROR: number of processes %d not within range %d-%d"
             "\n Exiting program!\n\n",
             np_total, MIN_PROCS, MAX_PROCS);
    }
    exit_after_lci_cleanup(1);
  }

  int active = determine_active_comm_size();
  if (active < 0) {
    exit_after_lci_cleanup(1);
  }
  if (!active) {
    exit_after_lci_cleanup(0);
  }

  is_lci::initialize_verification_data(&verification);

  if (my_rank == 0) {
    use_upacket = is_lci::read_use_upacket_flag();
    use_loopback = is_lci::read_loopback_flag();
    timeron = is_lci::check_timer_flag_from_file();
  }
  lci::broadcast_x(&use_upacket, sizeof(int), 0).device(devices[0])();
  lci::broadcast_x(&use_loopback, sizeof(int), 0).device(devices[0])();
  lci::broadcast_x(&timeron, sizeof(int), 0).device(devices[0])();
  is_lci::print_initial_status(my_rank, np_total, comm_size, use_upacket, use_loopback);

  is_lci::clear_timers();
  allocate_work_arrays();

  is_lci::RedistributorOptions options = redistributor_options();
  is_lci::allocate_fallback_buffers(&redistributor_runtime, comm_size, omp_get_max_threads(), options);

  is_lci::generate_keys(key_array, num_keys, static_cast<KeyRank>(MAX_KEY),
                        is_lci::find_my_seed(my_rank, comm_size, is_lci::compute_total_random_numbers(),
                                             is_lci::NAS_RANDOM_SEED, is_lci::NAS_RANDOM_MULTIPLIER),
                        is_lci::NAS_RANDOM_MULTIPLIER);

  is_lci::initialize_redistributor(&redistributor_runtime, options);
  lci::barrier_x().device(devices[0])();

  run_iteration(1);

  passed_verification = 0;
  if (my_rank == 0 && CLASS != 'S') {
    printf("\n   iteration\n");
  }

  timer_clear(0);
  is_lci::clear_timers();
  timer_start(0);

  for (int iteration = 1; iteration <= MAX_ITERATIONS; iteration++) {
    if (my_rank == 0 && CLASS != 'S') {
      printf("        %d\n", iteration);
    }
    lci::barrier_x().device(devices[0])();
    run_iteration(iteration);
  }

  timer_stop(0);
  double timecounter = timer_read(0);
  double maxtime = 0.0;
  lci::reduce_x(&timecounter, &maxtime, 1, sizeof(double), is_lci::max_op, 0).device(devices[0])();

  is_lci::full_verify(key_array, final_snapshot, my_rank, comm_size, devices, timeron, &passed_verification);

  int local_passed_verification = passed_verification;
  lci::reduce_x(&local_passed_verification, &passed_verification, 1, sizeof(int), is_lci::sum_op_int, 0)
      .device(devices[0])();

  free_work_arrays();

  if (my_rank == 0) {
    is_lci::print_final_results(comm_size, np_total, maxtime, passed_verification);
  }

  if (timeron) {
    is_lci::print_timer_summary(comm_size, devices);
  }

  is_lci::finalize_redistributor(&redistributor_runtime);
  free_devices();
  lci::g_runtime_fina();

  return 0;
}
